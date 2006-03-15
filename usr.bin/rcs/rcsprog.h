/*	$OpenBSD: rcsprog.h,v 1.38 2006/03/15 03:29:01 ray Exp $	*/
/*
 * Copyright (c) 2005 Joris Vink <joris@openbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL  DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RCSPROG_H
#define RCSPROG_H

#include "log.h"
#include "rcs.h"
#include "worklist.h"
#include "xmalloc.h"

#define RCS_DEFAULT_SUFFIX	",v/"
#define RCS_TMPDIR_DEFAULT	"/tmp"

/* flags specific to ci.c */
#define CI_SYMFORCE	(1<<0)
#define CI_DEFAULT	(1<<1)
#define CI_INIT		(1<<2)
#define CI_KEYWORDSCAN  (1<<3)

/* flags specific to co.c */
#define CO_ACLAPPEND	(1<<4)
#define CO_AUTHOR	(1<<5)
#define CO_LOCK		(1<<6)
#define CO_REVDATE	(1<<7)
#define CO_STATE	(1<<8)
#define CO_UNLOCK	(1<<9)

/* shared flags  */
#define FORCE		(1<<10)
#define INTERACTIVE	(1<<11)
#define NEWFILE		(1<<12)
#define PRESERVETIME	(1<<13)

extern char	*__progname;
extern const char	rcs_version[];
extern int	verbose;
extern int	pipeout;

extern int	 rcs_optind;
extern char	*rcs_optarg;
extern char	*rcs_suffixes;
extern char	*rcs_tmpdir;
extern struct cvs_wklhead rcs_temp_files;

/* date.y */
time_t  cvs_date_parse(const char *);

/* ci.c */
int	checkin_main(int, char **);
void	checkin_usage(void);

/* co.c */
int	checkout_main(int, char **);
int	checkout_rev(RCSFILE *, RCSNUM *, const char *, int, const char *,
	    const char *, const char *, const char *);
void	checkout_usage(void);

/* ident.c */
int	ident_main(int, char **);
void	ident_usage(void);

/* rcsclean.c */
int	rcsclean_main(int, char **);
void	rcsclean_usage(void);

/* rcsdiff.c */
int	rcsdiff_main(int, char **);
void	rcsdiff_usage(void);

/* rcsmerge.c */
int	rcsmerge_main(int, char **);
void	rcsmerge_usage(void);

/* rcsprog.c */
int	rcs_init(char *, char **, int);
int	rcs_getopt(int, char **, const char *);
int	rcs_main(int, char **);
int	rcs_set_mtime(const char *, time_t);
char 	*rcs_choosefile(const char *);
int	rcs_statfile(char *, char *, size_t);
time_t	rcs_get_mtime(const char *);
void	rcs_set_rev(const char *, RCSNUM **);
void	rcs_usage(void);
void	(*usage)(void);

/* rlog.c */
int	rlog_main(int, char **);
void	rlog_usage(void);

#endif	/* RCSPROG_H */
