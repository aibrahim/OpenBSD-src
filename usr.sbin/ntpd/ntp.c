/*	$OpenBSD: ntp.c,v 1.26 2004/08/12 16:33:59 henning Exp $ */

/*
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 * Copyright (c) 2004 Alexander Guy <alexander.guy@andern.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntpd.h"
#include "ntp.h"

#define	PFD_PIPE_MAIN	0
#define	PFD_MAX		1

volatile sig_atomic_t	 ntp_quit = 0;
struct imsgbuf		 ibuf_main;
struct l_fixedpt	 ref_ts;
struct ntpd_conf	*conf;

void	ntp_sighdlr(int);
int	ntp_dispatch_imsg(void);

void
ntp_sighdlr(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		ntp_quit = 1;
		break;
	}
}

pid_t
ntp_main(int pipe_prnt[2], struct ntpd_conf *nconf)
{
	int			 nfds, i, j, idx_peers, timeout;
	u_int			 pfd_elms = 0, idx2peer_elms = 0;
	u_int			 listener_cnt, peer_cnt, new_cnt;
	pid_t			 pid;
	struct pollfd		*pfd = NULL;
	struct passwd		*pw;
	struct servent		*se;
	struct listen_addr	*la;
	struct ntp_peer		*p;
	struct ntp_peer		**idx2peer = NULL;
	time_t			 nextaction;
	void			*newp;

	switch (pid = fork()) {
	case -1:
		fatal("cannot fork");
	case 0:
		break;
	default:
		return (pid);
	}

	if ((se = getservbyname("ntp", "udp")) == NULL)
		fatal("getservbyname");

	if ((pw = getpwnam(NTPD_USER)) == NULL)
		fatal(NULL);

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	setproctitle("ntp engine");

	conf = nconf;
	setup_listeners(se, conf, &listener_cnt);

	if (setgroups(1, &pw->pw_gid) ||
	    setegid(pw->pw_gid) || setgid(pw->pw_gid) ||
	    seteuid(pw->pw_uid) || setuid(pw->pw_uid))
		fatal("can't drop privileges");

	endpwent();
	endservent();

	signal(SIGTERM, ntp_sighdlr);
	signal(SIGINT, ntp_sighdlr);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	close(pipe_prnt[0]);
	imsg_init(&ibuf_main, pipe_prnt[1]);

	TAILQ_FOREACH(p, &conf->ntp_peers, entry)
		client_peer_init(p);

	bzero(&conf->status, sizeof(conf->status));
	conf->status.leap = LI_ALARM;

	log_info("ntp engine ready");

	peer_cnt = 0;
	TAILQ_FOREACH(p, &conf->ntp_peers, entry)
		peer_cnt++;

	while (ntp_quit == 0) {
		if (peer_cnt > idx2peer_elms ||
		    peer_cnt + IDX2PEER_RESERVE < idx2peer_elms) {
			if ((newp = realloc(idx2peer, sizeof(void *) *
			    (peer_cnt + IDX2PEER_RESERVE))) == NULL) {
				/* panic for now */
				log_warn("could not resize idx2peer from %u -> "
				    "%u entries", idx2peer_elms,
				    peer_cnt + IDX2PEER_RESERVE);
				fatalx("exiting");
			}
			idx2peer = newp;
			idx2peer_elms = peer_cnt + IDX2PEER_RESERVE;
		}

		new_cnt = PFD_MAX + peer_cnt + listener_cnt;
		if (new_cnt > pfd_elms ||
		    new_cnt + PFD_RESERVE < pfd_elms) {
			if ((newp = realloc(pfd, sizeof(struct pollfd) *
			    (new_cnt + PFD_RESERVE))) == NULL) {
				/* panic for now */
				log_warn("could not resize pfd from %u -> "
				    "%u entries", pfd_elms,
				    new_cnt + PFD_RESERVE);
				fatalx("exiting");
			}
			pfd = newp;
			pfd_elms = new_cnt + PFD_RESERVE;
		}

		bzero(pfd, sizeof(struct pollfd) * pfd_elms);
		bzero(idx2peer, sizeof(void *) * idx2peer_elms);
		nextaction = time(NULL) + 3600;
		pfd[PFD_PIPE_MAIN].fd = ibuf_main.fd;
		pfd[PFD_PIPE_MAIN].events = POLLIN;

		i = 1;
		TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
			pfd[i].fd = la->fd;
			pfd[i].events = POLLIN;
			i++;
		}

		idx_peers = i;
		TAILQ_FOREACH(p, &conf->ntp_peers, entry) {
			if (p->next > 0 && p->next < nextaction)
				nextaction = p->next;
			if (p->next > 0 && p->next <= time(NULL))
				client_query(p);

			if (p->deadline > 0 && p->deadline < nextaction)
				nextaction = p->deadline;
			if (p->deadline > 0 && p->deadline <= time(NULL)) {
				log_debug("no reply from %s received in time",
				    log_sockaddr(
				    (struct sockaddr *)&p->addr->ss));
				if (p->trustlevel >= TRUSTLEVEL_BADPEER &&
				    (p->trustlevel /= 2) < TRUSTLEVEL_BADPEER)
					log_info("peer %s now invalid",
					    log_sockaddr(
					    (struct sockaddr *)&p->addr->ss));
				client_nextaddr(p);
				client_query(p);
			}

			if (p->state == STATE_QUERY_SENT) {
				pfd[i].fd = p->query->fd;
				pfd[i].events = POLLIN;
				idx2peer[i - idx_peers] = p;
				i++;
			}
		}

		if (ibuf_main.w.queued > 0)
			pfd[PFD_PIPE_MAIN].events |= POLLOUT;

		timeout = nextaction - time(NULL);
		if (timeout < 0)
			timeout = 0;

		if ((nfds = poll(pfd, i, timeout * 1000)) == -1)
			if (errno != EINTR) {
				log_warn("poll error");
				ntp_quit = 1;
			}

		if (nfds > 0 && (pfd[PFD_PIPE_MAIN].revents & POLLOUT))
			if (msgbuf_write(&ibuf_main.w) < 0) {
				log_warn("pipe write error (to parent)");
				ntp_quit = 1;
			}

		if (nfds > 0 && pfd[PFD_PIPE_MAIN].revents & POLLIN) {
			nfds--;
			if (ntp_dispatch_imsg() == -1)
				ntp_quit = 1;
		}

		for (j = 1; nfds > 0 && j < idx_peers; j++)
			if (pfd[j].revents & POLLIN) {
				nfds--;
				if (server_dispatch(pfd[j].fd, conf) == -1)
					ntp_quit = 1;
			}

		for (; nfds > 0 && j < i; j++)
			if (pfd[j].revents & POLLIN) {
				nfds--;
				if (client_dispatch(idx2peer[j - idx_peers]) ==
				     -1)
					ntp_quit = 1;
			}
	}

	msgbuf_write(&ibuf_main.w);
	msgbuf_clear(&ibuf_main.w);

	log_info("ntp engine exiting");
	_exit(0);
}

int
ntp_dispatch_imsg(void)
{
	struct imsg		 imsg;
	int			 n;
	struct ntp_peer		*peer, *npeer;
	u_int16_t		 dlen;
	u_char			*p;
	struct ntp_addr		*h;

	if ((n = imsg_read(&ibuf_main)) == -1)
		return (-1);

	if (n == 0) {	/* connection closed */
		log_warnx("ntp_dispatch_imsg in ntp engine: pipe closed");
		return (-1);
	}

	for (;;) {
		if ((n = imsg_get(&ibuf_main, &imsg)) == -1)
			return (-1);

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_HOST_DNS:
			TAILQ_FOREACH(peer, &conf->ntp_peers, entry)
				if (peer->id == imsg.hdr.peerid)
					break;
			if (peer == NULL)
				fatal("IMSG_HOST_DNS with invalid peerID");
			if (peer->addr != NULL) {
				log_warnx("IMSG_HOST_DNS but addr != NULL!");
				break;
			}
			dlen = imsg.hdr.len - IMSG_HEADER_SIZE;
			p = (u_char *)imsg.data;
			while (dlen >= sizeof(struct sockaddr_storage)) {
				if ((h = calloc(1, sizeof(struct ntp_addr))) ==
				    NULL)
					fatal(NULL);
				memcpy(&h->ss, p, sizeof(h->ss));
				p += sizeof(h->ss);
				dlen -= sizeof(h->ss);
				if (peer->addr_head.pool) {
					npeer = new_peer();
					h->next = NULL;
					npeer->addr = h;
					npeer->addr_head.a = h;
					client_peer_init(npeer);
					TAILQ_INSERT_TAIL(&conf->ntp_peers,
					    npeer, entry);
				} else {
					h->next = peer->addr;
					peer->addr = h;
					peer->addr_head.a = peer->addr;
				}
			}
			if (dlen != 0)
				fatal("IMSG_HOST_DNS: dlen != 0");
			if (peer->addr_head.pool) {
				TAILQ_REMOVE(&conf->ntp_peers, peer, entry);
				free(peer);
			} else
				client_addr_init(peer);
			break;
		default:
			break;
		}
		imsg_free(&imsg);
	}
	return (0);
}

void
ntp_adjtime(void)
{
	struct ntp_peer	*p;
	double		 offset_median = 0;
	int		 offset_cnt = 0;

	TAILQ_FOREACH(p, &conf->ntp_peers, entry) {
		if (p->trustlevel < TRUSTLEVEL_BADPEER)
			continue;

		if (!p->update.good)
			return;

		offset_median += p->update.offset;
		offset_cnt++;
	}

	if (offset_cnt > 0) {
		offset_median /= offset_cnt;
		imsg_compose(&ibuf_main, IMSG_ADJTIME, 0,
		    &offset_median, sizeof(offset_median));

		conf->status.reftime = gettime();
		conf->status.leap = LI_NOWARNING;		/* XXX */
	}

	TAILQ_FOREACH(p, &conf->ntp_peers, entry)
		p->update.good = 0;
}

void
ntp_host_dns(char *name, u_int32_t peerid)
{
	u_int16_t	dlen;

	dlen = strlen(name) + 1;
	imsg_compose(&ibuf_main, IMSG_HOST_DNS, peerid, name, dlen);
}
