/* $OpenBSD: fuse_device.c,v 1.4 2013/07/11 11:38:10 syl Exp $ */
/*
 * Copyright (c) 2012-2013 Sylvestre Gallon <ccna.syl@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/pool.h>
#include <sys/statvfs.h>
#include <sys/vnode.h>
#include <sys/fusebuf.h>

#include "fusefs_node.h"
#include "fusefs.h"

#ifdef	FUSE_DEBUG
#define	DPRINTF(fmt, arg...)	printf("%s: " fmt, "fuse", ##arg)
#else
#define	DPRINTF(fmt, arg...)
#endif

SIMPLEQ_HEAD(fusebuf_head, fusebuf);

struct fuse_d {
	struct fusefs_mnt *fd_fmp;
	int fd_unit;

	/*fusebufs queues*/
	struct fusebuf_head fd_fbufs_in;
	struct fusebuf_head fd_fbufs_wait;

	/* kq fields */
	struct selinfo fd_rsel;
	LIST_ENTRY(fuse_d) fd_list;
};

int stat_fbufs_in = 0;
int stat_fbufs_wait = 0;
int stat_opened_fusedev = 0;

LIST_HEAD(, fuse_d) fuse_d_list;
struct fuse_d *fuse_create(int);
struct fuse_d *fuse_lookup(int);
void	fuse_destroy(dev_t dev, struct fuse_d *fd);

void	fuseattach(int);
int	fuseopen(dev_t, int, int, struct proc *);
int	fuseclose(dev_t, int, int, struct proc *);
int	fuseioctl(dev_t, u_long, caddr_t, int, struct proc *);
int	fuseread(dev_t, struct uio *, int);
int	fusewrite(dev_t, struct uio *, int);
int	fusepoll(dev_t, int, struct proc *);
int	fusekqfilter(dev_t dev, struct knote *kn);
int	filt_fuse_read(struct knote *, long);
void	filt_fuse_rdetach(struct knote *);

const static struct filterops fuse_rd_filtops = {
	1,
	NULL,
	filt_fuse_rdetach,
	filt_fuse_read
};

const static struct filterops fuse_seltrue_filtops = {
	1,
	NULL,
	filt_fuse_rdetach,
	filt_seltrue
};

#ifdef	FUSE_DEBUG
static void
fuse_dump_buff(char *buff, int len)
{
	char text[17];
	int i;

	bzero(text, 17);
	for (i = 0; i < len; i++) {
		if (i != 0 && (i % 16) == 0) {
			printf(": %s\n", text);
			bzero(text, 17);
		}

		printf("%.2x ", buff[i] & 0xff);

		if (buff[i] > ' ' && buff[i] < '~')
			text[i%16] = buff[i] & 0xff;
		else
			text[i%16] = '.';
	}

	if ((i % 16) != 0)
		while ((i % 16) != 0) {
			printf("   ");
			i++;
		}

	printf(": %s\n", text);
}
#endif

struct fuse_d *
fuse_lookup(int unit)
{
	struct fuse_d *fd;

	LIST_FOREACH(fd, &fuse_d_list, fd_list)
		if (fd->fd_unit == unit)
			return (fd);
	return (NULL);
}

struct fuse_d *
fuse_create(int unit)
{
	struct fuse_d *fd;

	if ((fd = fuse_lookup(unit)) != NULL)
		return (NULL);

	fd = malloc(sizeof(*fd), M_DEVBUF, M_WAITOK | M_ZERO);
	fd->fd_unit = unit;
	SIMPLEQ_INIT(&fd->fd_fbufs_in);
	SIMPLEQ_INIT(&fd->fd_fbufs_wait);
	LIST_INSERT_HEAD(&fuse_d_list, fd, fd_list);
	return (fd);
}

void
fuse_destroy(dev_t dev, struct fuse_d *fd)
{
	LIST_REMOVE(fd, fd_list);
	fuse_device_cleanup(dev, NULL);
	free(fd, M_DEVBUF);
}

/*
 * if fbuf == NULL cleanup all msgs else remove fbuf from
 * sc_fbufs_in and sc_fbufs_wait.
 */
void
fuse_device_cleanup(dev_t dev, struct fusebuf *fbuf)
{
	struct fuse_d *fd;
	struct fusebuf *f;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return;

	fd->fd_fmp = NULL;
	/* clear FIFO IN*/
	while ((f = SIMPLEQ_FIRST(&fd->fd_fbufs_in))) {
		if (fbuf == f || fbuf == NULL) {
			DPRINTF("cleanup unprocessed msg in sc_fbufs_in\n");
			SIMPLEQ_REMOVE_HEAD(&fd->fd_fbufs_in, fb_next);
			stat_fbufs_in--;
			if (fbuf == NULL)
				pool_put(&fusefs_fbuf_pool, f);
		}
	}

	/* clear FIFO WAIT*/
	while ((f = SIMPLEQ_FIRST(&fd->fd_fbufs_wait))) {
		if (fbuf == f || fbuf == NULL) {
			DPRINTF("umount unprocessed msg in sc_fbufs_wait\n");
			SIMPLEQ_REMOVE_HEAD(&fd->fd_fbufs_wait, fb_next);
			stat_fbufs_wait--;
			if (fbuf == NULL)
				pool_put(&fusefs_fbuf_pool, f);
		}
	}
}

void
fuse_device_queue_fbuf(dev_t dev, struct fusebuf *fbuf)
{
	struct fuse_d *fd;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return;

	SIMPLEQ_INSERT_TAIL(&fd->fd_fbufs_in, fbuf, fb_next);
	stat_fbufs_in++;
	selwakeup(&fd->fd_rsel);
}

void
fuse_device_set_fmp(struct fusefs_mnt *fmp)
{
	struct fuse_d *fd;

	fd = fuse_lookup(minor(fmp->dev));
	if (fd == NULL)
		return;

	fd->fd_fmp = fmp;
}

void
fuseattach(int num)
{
	LIST_INIT(&fuse_d_list);
}

int
fuseopen(dev_t dev, int flags, int fmt, struct proc * p)
{
	struct fuse_d *fd;

	if (flags & O_EXCL)
		return (EBUSY); /* No exclusive opens */

	if ((fd = fuse_create(minor(dev))) == NULL)
		return (EBUSY);

	stat_opened_fusedev++;
	return (0);
}

int
fuseclose(dev_t dev, int flags, int fmt, struct proc *p)
{
	struct fuse_d *fd;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return (EINVAL);

	if (fd->fd_fmp) {
		printf("libfuse close the device without umount\n");
		fd->fd_fmp->sess_init = 0;
		fd->fd_fmp = NULL;
	}

	fuse_destroy(dev, fd);
	stat_opened_fusedev--;
	return (0);
}

int
fuseioctl(dev_t dev, u_long cmd, caddr_t addr, int flags, struct proc *p)
{
	int error = 0;

	switch (cmd) {
	default:
		DPRINTF("bad ioctl number %d\n", cmd);
		return (ENODEV);
	}

	return (error);
}

int
fuseread(dev_t dev, struct uio *uio, int ioflag)
{
	struct fuse_d *fd;
	struct fusebuf *fbuf;
	struct fb_hdr hdr;
	int error = 0;
	char *F_dat;
	int remain;
	int size;
	size_t len;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return (ENXIO);

	if (SIMPLEQ_EMPTY(&fd->fd_fbufs_in)) {
		if (ioflag & O_NONBLOCK)
			return (EAGAIN);

		goto end;
	}
	fbuf = SIMPLEQ_FIRST(&fd->fd_fbufs_in);

	/*
	 * If it was not taken by last read
	 * fetch the fb_hdr.
	 */
	len = sizeof(struct fb_hdr);
	if (fbuf->fb_resid == -1) {
		/* we get the whole header or nothing */
		if (uio->uio_resid < len)
			return (EINVAL);

		memcpy(&hdr, &fbuf->fb_hdr, sizeof(hdr));
		bzero(&hdr.fh_next, sizeof(hdr.fh_next));
		error = uiomove(&hdr, len, uio);
		if (error)
			goto end;

#ifdef FUSE_DEBUG
		fuse_dump_buff((char *)fbuf, len);
#endif
		fbuf->fb_resid = 0;
	}

	/* fetch F_dat if there is something present */
	if ((fbuf->fb_len > 0) && uio->uio_resid) {
		size = MIN(fbuf->fb_len - fbuf->fb_resid, uio->uio_resid);
		F_dat = (char *)&fbuf->F_dat;

		error = uiomove(&F_dat[fbuf->fb_resid], size, uio);
		if (error)
			goto end;

#ifdef FUSE_DEBUG
		fuse_dump_buff(&F_dat[fbuf->fb_resid], size);
#endif
		fbuf->fb_resid += size;
	}

	remain = (fbuf->fb_len - fbuf->fb_resid);
	DPRINTF("size remaining : %i\n", remain);

	/*
	 * fbuf moves from a simpleq to another
	 */
	if (remain == 0) {
		SIMPLEQ_REMOVE_HEAD(&fd->fd_fbufs_in, fb_next);
		stat_fbufs_in--;
		SIMPLEQ_INSERT_TAIL(&fd->fd_fbufs_wait, fbuf, fb_next);
		stat_fbufs_wait++;
	}

end:
	return (error);
}

int
fusewrite(dev_t dev, struct uio *uio, int ioflag)
{
	struct fusebuf *lastfbuf;
	struct fuse_d *fd;
	struct fusebuf *fbuf;
	struct fb_hdr hdr;
	int error = 0;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return (ENXIO);

	if (uio->uio_resid < sizeof(hdr)) {
		return (EINVAL);
	}

	/*
	 * get out header
	 */
	if ((error = uiomove(&hdr, sizeof(hdr), uio)) != 0)
		return (error);

	SIMPLEQ_FOREACH(fbuf, &fd->fd_fbufs_wait, fb_next) {
		if (fbuf->fb_uuid == hdr.fh_uuid) {
			DPRINTF("catch unique %lu\n", fbuf->fb_uuid);
			break;
		}

		lastfbuf = fbuf;
	}

#ifdef FUSE_DEBUG
	fuse_dump_buff((char *)&hdr, sizeof(hdr));
#endif

	if (fbuf != NULL) {
		fbuf->fb_len = hdr.fh_len;
		fbuf->fb_err = hdr.fh_err;
		fbuf->fb_ino = hdr.fh_ino;

		if (uio->uio_resid != hdr.fh_len ||
		    (uio->uio_resid != 0 && hdr.fh_err) ||
		    SIMPLEQ_EMPTY(&fd->fd_fbufs_wait)) {
			printf("corrupted fuse header or queue empty\n");
			return (EINVAL);
		}

		if (uio->uio_resid > 0  && fbuf->fb_len > 0) {
			error = uiomove(&fbuf->F_dat, fbuf->fb_len, uio);
			if (error)
				return error;
#ifdef FUSE_DEBUG
			fuse_dump_buff((char *)&fbuf->F_dat, fbuf->fb_len);
#endif
		}

		if (!error) {
			switch (fbuf->fb_type) {
			case FBT_INIT:
				fd->fd_fmp->sess_init = 1;
				break ;
			case FBT_DESTROY:
				fd->fd_fmp = NULL;
				break ;
			}

			wakeup(fbuf);
		}

		/* the fbuf could not be the HEAD fbuf */
		if (fbuf == SIMPLEQ_FIRST(&fd->fd_fbufs_wait))
			SIMPLEQ_REMOVE_HEAD(&fd->fd_fbufs_wait, fb_next);
		else
			SIMPLEQ_REMOVE_AFTER(&fd->fd_fbufs_wait, lastfbuf,
			    fb_next);
		stat_fbufs_wait--;

		if (fbuf->fb_type == FBT_INIT)
			pool_put(&fusefs_fbuf_pool, fbuf);

	} else
		error = EINVAL;

	return (error);
}

int
fusepoll(dev_t dev, int events, struct proc *p)
{
	struct fuse_d *fd;
	int revents = 0;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return (EINVAL);

	if (events & (POLLIN | POLLRDNORM))
		if (!SIMPLEQ_EMPTY(&fd->fd_fbufs_in))
			revents |= events & (POLLIN | POLLRDNORM);

	if (events & (POLLOUT | POLLWRNORM))
		revents |= events & (POLLOUT | POLLWRNORM);

	if (revents == 0)
		if (events & (POLLIN | POLLRDNORM))
			selrecord(p, &fd->fd_rsel);

	return (revents);
}

int
fusekqfilter(dev_t dev, struct knote *kn)
{
	struct fuse_d *fd;
	struct klist *klist;

	fd = fuse_lookup(minor(dev));
	if (fd == NULL)
		return (EINVAL);

	switch (kn->kn_filter) {
	case EVFILT_READ:
		klist = &fd->fd_rsel.si_note;
		kn->kn_fop = &fuse_rd_filtops;
		break;
	case EVFILT_WRITE:
		klist = &fd->fd_rsel.si_note;
		kn->kn_fop = &fuse_seltrue_filtops;
		break;
	default:
		return (EINVAL);
	}

	kn->kn_hook = fd;

	SLIST_INSERT_HEAD(klist, kn, kn_selnext);

	return (0);
}

void
filt_fuse_rdetach(struct knote *kn)
{
	struct fuse_d *fd = kn->kn_hook;
	struct klist *klist = &fd->fd_rsel.si_note;

	SLIST_REMOVE(klist, kn, knote, kn_selnext);
}

int
filt_fuse_read(struct knote *kn, long hint)
{
	struct fuse_d *fd = kn->kn_hook;
	int event = 0;

	if (!SIMPLEQ_EMPTY(&fd->fd_fbufs_in))
		event = 1;

	return (event);
}
