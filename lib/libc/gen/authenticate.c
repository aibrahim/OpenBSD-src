/*	$OpenBSD: authenticate.c,v 1.5 2001/07/09 06:57:42 deraadt Exp $	*/

/*-
 * Copyright (c) 1997 Berkeley Software Design, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Berkeley Software Design,
 *	Inc.
 * 4. The name of Berkeley Software Design, Inc.  may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY BERKELEY SOFTWARE DESIGN, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL BERKELEY SOFTWARE DESIGN, INC. BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	BSDI $From: authenticate.c,v 2.21 1999/09/08 22:33:26 prb Exp $
 */
#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <login_cap.h>
#include <paths.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <bsd_auth.h>

static int _auth_checknologin(login_cap_t *, int);

char *
auth_mkvalue(char *value)
{
	char *big, *p;

	big = malloc(strlen(value) * 4 + 1);
	if (big == NULL)
		return (NULL);
	/*
	 * XXX - There should be a more standardized
	 * routine for doing this sort of thing.
	 */
	for (p = big; *value; ++value) {
		switch (*value) {
		case '\r':
			*p++ = '\\';
			*p++ = 'r';
			break;
		case '\n':
			*p++ = '\\';
			*p++ = 'n';
			break;
		case '\\':
			*p++ = '\\';
			*p++ = *value;
			break;
		case '\t':
		case ' ':
			if (p == big)
				*p++ = '\\';
			*p++ = *value;
			break;
		default:
			if (!isprint(*value)) {
				*p++ = '\\';
				*p++ = ((*value >> 6) & 0x3) + '0';
				*p++ = ((*value >> 3) & 0x7) + '0';
				*p++ = ((*value     ) & 0x7) + '0';
			} else
				*p++ = *value;
			break;
		}
	}
	*p = '\0';
	return (big);
}

void
auth_checknologin(login_cap_t *lc)
{
	if (_auth_checknologin(lc, 1))
		exit(1);
}

static int
_auth_checknologin(login_cap_t *lc, int print)
{
	struct stat sb;
	char *nologin;

	/*
	 * If we fail to get the nologin file due to a database error,
	 * assume there should have been one...
	 */
	if ((nologin = login_getcapstr(lc, "nologin", "", NULL)) == NULL) {
		if (print) {
			printf("Logins are not allowed at this time.\n");
			fflush(stdout);
		}
		return (-1);
	}
	if (*nologin && stat(nologin, &sb) >= 0) {
		if (print && auth_cat(nologin) == 0) {
			printf("Logins are not allowed at this time.\n");
			fflush(stdout);
		}
		return (-1);
	}

	if (login_getcapbool(lc, "ignorenologin", 0))
		return(0);

	if (stat(_PATH_NOLOGIN, &sb) >= 0) {
		if (print && auth_cat(_PATH_NOLOGIN) == 0) {
			printf("Logins are not allowed at this time.\n");
			fflush(stdout);
		}
		return (-1);
	}
	return(0);
}

int
auth_cat(char *file)
{
	int fd, nchars;
	char tbuf[8192];

	if ((fd = open(file, O_RDONLY, 0)) < 0)
		return (0);
	while ((nchars = read(fd, tbuf, sizeof(tbuf))) > 0)
		(void)write(fileno(stdout), tbuf, nchars);
	(void)close(fd);
	return (1);
}

int
auth_approval(auth_session_t *as, login_cap_t *lc, char *name, char *type)
{
	int close_on_exit, close_lc_on_exit;
	struct passwd *pwd;
	char *approve, *s, path[MAXPATHLEN];

	pwd = NULL;
	close_on_exit = as == NULL;
	close_lc_on_exit = lc == NULL;

	if (as != NULL && name == NULL)
		name = auth_getitem(as, AUTHV_NAME);

	if (as != NULL)
		pwd = auth_getpwd(as);

	if (pwd == NULL) {
		if (name != NULL)
			pwd = getpwnam(name);
		else {
			if ((pwd = getpwuid(getuid())) == NULL) {
				syslog(LOG_ERR, "no such user id %d", getuid());
				_warnx("cannot approve who we don't recognize");
				return (0);
			}
			name = pwd->pw_name;
		}
	}

	if (name == NULL)
		name = pwd->pw_name;

	if (lc == NULL) {
		if (strlen(name) >= MAXPATHLEN) {
			syslog(LOG_ERR, "username to login %.*s...",
			    MAXPATHLEN, name);
			_warnx("username too long");
			return (0);
		}
		if (pwd == NULL && (approve = strchr(name, '.')) != NULL) {
			strcpy(path, name);
			path[approve-name] = '\0';
			pwd = getpwnam(name);
		}
		lc = login_getclass(pwd ? pwd->pw_class : NULL);
		if (lc == NULL) {
			_warnx("unable to classify user");
			return (0);
		}
	}

	if (!type)
		type = LOGIN_DEFSERVICE;
	else {
		if (strncmp(type, "approve-", 8) == 0)
			type += 8;

		snprintf(path, sizeof(path), "approve-%s", type);
	}

	if ((approve = login_getcapstr(lc, s = path, NULL, NULL)) == NULL)
		approve = login_getcapstr(lc, s = "approve", NULL, NULL);

	if (approve && approve[0] != '/') {
		if (close_lc_on_exit)
			login_close(lc);
		syslog(LOG_ERR, "Invalid %s script: %s", s, approve);
		_warnx("invalid path to approval script");
		return (0);
	}

	if (as == NULL && (as = auth_open()) == NULL) {
		if (close_lc_on_exit)
			login_close(lc);
		syslog(LOG_ERR, "%m");
		_warn(NULL);
		return (0);
	}

	auth_setstate(as, AUTH_OKAY);
	if (auth_setitem(as, AUTHV_NAME, name) < 0) {
		syslog(LOG_ERR, "%m");
		_warn(NULL);
		goto out;
	}
	if (auth_check_expire(as))	/* is this account expired */
		goto out;
	if (_auth_checknologin(lc,
	    auth_getitem(as, AUTHV_INTERACTIVE) != NULL)) {
		auth_setstate(as, (auth_getstate(as) & ~AUTH_ALLOW));
		goto out;
	}
	if (login_getcapbool(lc, "requirehome", 0) && pwd && pwd->pw_dir &&
	    pwd->pw_dir[0]) {
		struct stat sb;

		if (stat(pwd->pw_dir, &sb) < 0 ||
		    (sb.st_mode & 0170000) != S_IFDIR ||
		    (pwd->pw_uid && sb.st_uid == pwd->pw_uid &&
		     (sb.st_mode & S_IXUSR) == 0)) {
			auth_setstate(as, (auth_getstate(as) & ~AUTH_ALLOW));
			goto out;
		}
	}
	if (approve)
	    auth_call(as, approve, strrchr(approve, '/') + 1, name,
		lc->lc_class, type, 0);

out:
	if (close_lc_on_exit)
		login_close(lc);

	if (close_on_exit)
		return (auth_close(as));
	return (auth_getstate(as) & AUTH_ALLOW);
}

auth_session_t *
auth_usercheck(char *name, char *style, char *type, char *password)
{
	char namebuf[MAXLOGNAME + 1 + NAME_MAX + 1];
	auth_session_t *as;
	login_cap_t *lc;
	struct passwd *pwd;
	char *sep, save;

	if (strlen(name) >= sizeof(namebuf))
		return (NULL);
	strcpy(namebuf, name);
	name = namebuf;

	/*
	 * Split up user:style names if we were not given a style
	 */
	if (style == NULL && (style = strchr(name, ':')) != NULL)
		*style++ = '\0';

	/*
	 * Cope with user[./]instance.  We are only using this to get
	 * the class so it is okay if we strip a root instance
	 * The actual login script will pay attention to the instance.
	 */
	if ((pwd = getpwnam(name)) == NULL) {
		if ((sep = strpbrk(name, "./")) != NULL) {
			save = *sep;
			*sep = '\0';
			pwd = getpwnam(name);
			*sep = save;
		}
	}
	if ((lc = login_getclass(pwd ? pwd->pw_class : NULL)) == NULL)
		return (NULL);

	if ((style = login_getstyle(lc, style, type)) == NULL) {
		login_close(lc);
		return (NULL);
	}

	if (password) {
		if ((as = auth_open()) == NULL) {
			login_close(lc);
			return (NULL);
		}
		auth_setitem(as, AUTHV_SERVICE, "response");
		auth_setdata(as, "", 1);
		auth_setdata(as, password, strlen(password) + 1);
	} else
		as = NULL;
	as = auth_verify(as, style, name, lc->lc_class, NULL);
	login_close(lc);
	return (as);
}

int
auth_userokay(char *name, char *style, char *type, char *password)
{
	auth_session_t *as;

	as = auth_usercheck(name, style, type, password);

	return (as != NULL ? auth_close(as) : 0);
}

auth_session_t *
auth_userchallenge(char *name, char *style, char *type, char **challengep)
{
	char namebuf[MAXLOGNAME + 1 + NAME_MAX + 1];
	auth_session_t *as;
	login_cap_t *lc;
	struct passwd *pwd;
	char *sep, save;

	if (strlen(name) >= sizeof(namebuf))
		return (NULL);
	strcpy(namebuf, name);
	name = namebuf;

	/*
	 * Split up user:style names if we were not given a style
	 */
	if (style == NULL && (style = strchr(name, ':')) != NULL)
		*style++ = '\0';

	/*
	 * Cope with user[./]instance.  We are only using this to get
	 * the class so it is okay if we strip a root instance
	 * The actual login script will pay attention to the instance.
	 */
	if ((pwd = getpwnam(name)) == NULL) {
		if ((sep = strpbrk(name, "./")) != NULL) {
			save = *sep;
			*sep = '\0';
			pwd = getpwnam(name);
			*sep = save;
		}
	}
	if ((lc = login_getclass(pwd ? pwd->pw_class : NULL)) == NULL)
		return (NULL);

	if ((style = login_getstyle(lc, style, type)) == NULL ||
	    (as = auth_open()) == NULL) {
		login_close(lc);
		return (NULL);
	}
	if (auth_setitem(as, AUTHV_STYLE, style) < 0 ||
	    auth_setitem(as, AUTHV_NAME, name) < 0 ||
	    auth_setitem(as, AUTHV_CLASS, lc->lc_class) < 0) {
		auth_close(as);
		login_close(lc);
		return (NULL);
	}
	login_close(lc);
	*challengep = auth_challenge(as);
	return (as);
}

int
auth_userresponse(auth_session_t *as, char *response, int more)
{
	char path[MAXPATHLEN];
	char *style, *name, *challenge, *class;

	if (as == NULL)
		return (0);

	auth_setstate(as, 0);

	if ((style = auth_getitem(as, AUTHV_STYLE)) == NULL ||
	    (name = auth_getitem(as, AUTHV_NAME)) == NULL) {
		if (more == 0)
			return (auth_close(as));
		return(0);
	}
	challenge = auth_getitem(as, AUTHV_CHALLENGE);
	class = auth_getitem(as, AUTHV_CLASS);

	if (challenge)
		auth_setdata(as, challenge, strlen(challenge) + 1);
	else
		auth_setdata(as, "", 1);
	if (response)
		auth_setdata(as, response, strlen(response) + 1);
	else
		auth_setdata(as, "", 1);

	snprintf(path, sizeof(path), _PATH_AUTHPROG "%s", style);

	auth_call(as, path, style, "-s", "response", name, class, NULL);

	/*
	 * If they authenticated then make sure they did not expire
	 */
	if (auth_getstate(as) & AUTH_ALLOW)
		auth_check_expire(as);
	if (more == 0)
		return (auth_close(as));
	return (auth_getstate(as) & AUTH_ALLOW);
}

/*
 * Authenticate name with the specified style.
 * If ``as'' is NULL a new session is formed with the default service.
 * Returns NULL only if ``as'' is NULL and we were unable to allocate
 * a new session.
 *
 * Use auth_close() or auth_getstate() to determine if the authentication
 * worked.
 */
auth_session_t *
auth_verify(auth_session_t *as, char *style, char *name, ...)
{
	va_list ap;
	char path[MAXPATHLEN];

	if ((name == NULL || style == NULL) && as == NULL)
		return (as);

	if (as == NULL && (as = auth_open()) == NULL)
		return (NULL);
	auth_setstate(as, 0);

	if (style != NULL && auth_setitem(as, AUTHV_STYLE, style) < 0)
		return (as);

	if (name != NULL && auth_setitem(as, AUTHV_NAME, name) < 0)
		return (as);

	style = auth_getitem(as, AUTHV_STYLE);
	name = auth_getitem(as, AUTHV_NAME);

	snprintf(path, sizeof(path), _PATH_AUTHPROG "%s", style);
	va_start(ap, name);
	auth_set_va_list(as, ap);
	auth_call(as, path, auth_getitem(as, AUTHV_STYLE), "-s",
	    auth_getitem(as, AUTHV_SERVICE), name, NULL);
	return (as);
}
