/* $OpenBSD: mux.c,v 1.10 2010/01/26 01:28:35 djm Exp $ */
/*
 * Copyright (c) 2002-2008 Damien Miller <djm@openbsd.org>
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

/* ssh session multiplexing support */

// XXX signal of slave passed to master

/*
 * TODO:
 *   - Better signalling from master to slave, especially passing of
 *      error messages
 *   - Better fall-back from mux slave error to new connection.
 *   - ExitOnForwardingFailure
 *   - Maybe extension mechanisms for multi-X11/multi-agent forwarding
 *   - Support ~^Z in mux slaves.
 *   - Inspect or control sessions in master.
 *   - If we ever support the "signal" channel request, send signals on
 *     sessions in master.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include <paths.h>

#include "atomicio.h"
#include "xmalloc.h"
#include "log.h"
#include "ssh.h"
#include "pathnames.h"
#include "misc.h"
#include "match.h"
#include "buffer.h"
#include "channels.h"
#include "msg.h"
#include "packet.h"
#include "monitor_fdpass.h"
#include "sshpty.h"
#include "key.h"
#include "readconf.h"
#include "clientloop.h"

/* from ssh.c */
extern int tty_flag;
extern int force_tty_flag;
extern Options options;
extern int stdin_null_flag;
extern char *host;
extern int subsystem_flag;
extern Buffer command;
extern volatile sig_atomic_t quit_pending;
extern char *stdio_forward_host;
extern int stdio_forward_port;

/* Context for session open confirmation callback */
struct mux_session_confirm_ctx {
	u_int want_tty;
	u_int want_subsys;
	u_int want_x_fwd;
	u_int want_agent_fwd;
	Buffer cmd;
	char *term;
	struct termios tio;
	char **env;
};

/* fd to control socket */
int muxserver_sock = -1;

/* client request id */
u_int muxclient_request_id = 0;

/* Multiplexing control command */
u_int muxclient_command = 0;

/* Set when signalled. */
static volatile sig_atomic_t muxclient_terminate = 0;

/* PID of multiplex server */
static u_int muxserver_pid = 0;

static Channel *mux_listener_channel = NULL;

struct mux_master_state {
	int hello_rcvd;
};

/* mux protocol messages */
#define MUX_MSG_HELLO		0x00000001
#define MUX_C_NEW_SESSION	0x10000002
#define MUX_C_ALIVE_CHECK	0x10000004
#define MUX_C_TERMINATE		0x10000005
#define MUX_C_OPEN_FWD		0x10000006
#define MUX_C_CLOSE_FWD		0x10000007
#define MUX_C_NEW_STDIO_FWD	0x10000008
#define MUX_S_OK		0x80000001
#define MUX_S_PERMISSION_DENIED	0x80000002
#define MUX_S_FAILURE		0x80000003
#define MUX_S_EXIT_MESSAGE	0x80000004
#define MUX_S_ALIVE		0x80000005
#define MUX_S_SESSION_OPENED	0x80000006

/* type codes for MUX_C_OPEN_FWD and MUX_C_CLOSE_FWD */
#define MUX_FWD_LOCAL   1
#define MUX_FWD_REMOTE  2
#define MUX_FWD_DYNAMIC 3

static void mux_session_confirm(int, void *);

static int process_mux_master_hello(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_new_session(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_alive_check(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_terminate(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_open_fwd(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_close_fwd(u_int, Channel *, Buffer *, Buffer *);
static int process_mux_stdio_fwd(u_int, Channel *, Buffer *, Buffer *);

static const struct {
	u_int type;
	int (*handler)(u_int, Channel *, Buffer *, Buffer *);
} mux_master_handlers[] = {
	{ MUX_MSG_HELLO, process_mux_master_hello },
	{ MUX_C_NEW_SESSION, process_mux_new_session },
	{ MUX_C_ALIVE_CHECK, process_mux_alive_check },
	{ MUX_C_TERMINATE, process_mux_terminate },
	{ MUX_C_OPEN_FWD, process_mux_open_fwd },
	{ MUX_C_CLOSE_FWD, process_mux_close_fwd },
	{ MUX_C_NEW_STDIO_FWD, process_mux_stdio_fwd },
	{ 0, NULL }
};

/* Cleanup callback fired on closure of mux slave _session_ channel */
/* ARGSUSED */
static void
mux_master_session_cleanup_cb(int cid, void *unused)
{
	Channel *cc, *c = channel_by_id(cid);

	debug3("%s: entering for channel %d", __func__, cid);
	if (c == NULL)
		fatal("%s: channel_by_id(%i) == NULL", __func__, cid);
	if (c->ctl_chan != -1) {
		if ((cc = channel_by_id(c->ctl_chan)) == NULL)
			fatal("%s: channel %d missing control channel %d",
			    __func__, c->self, c->ctl_chan);
		c->ctl_chan = -1;
		cc->remote_id = -1;
		chan_rcvd_oclose(cc);
	}
	channel_cancel_cleanup(c->self);
}

/* Cleanup callback fired on closure of mux slave _control_ channel */
/* ARGSUSED */
static void
mux_master_control_cleanup_cb(int cid, void *unused)
{
	Channel *sc, *c = channel_by_id(cid);

	debug3("%s: entering for channel %d", __func__, cid);
	if (c == NULL)
		fatal("%s: channel_by_id(%i) == NULL", __func__, cid);
	if (c->remote_id != -1) {
		if ((sc = channel_by_id(c->remote_id)) == NULL)
			debug2("%s: channel %d n session channel %d",
			    __func__, c->self, c->remote_id);
		c->remote_id = -1;
		sc->ctl_chan = -1;
		chan_mark_dead(sc);
	}
	channel_cancel_cleanup(c->self);
}

/* Check mux client environment variables before passing them to mux master. */
static int
env_permitted(char *env)
{
	int i, ret;
	char name[1024], *cp;

	if ((cp = strchr(env, '=')) == NULL || cp == env)
		return 0;
	ret = snprintf(name, sizeof(name), "%.*s", (int)(cp - env), env);
	if (ret <= 0 || (size_t)ret >= sizeof(name)) {
		error("env_permitted: name '%.100s...' too long", env);
		return 0;
	}

	for (i = 0; i < options.num_send_env; i++)
		if (match_pattern(name, options.send_env[i]))
			return 1;

	return 0;
}

/* Mux master protocol message handlers */

static int
process_mux_master_hello(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	u_int ver;
	struct mux_master_state *state = (struct mux_master_state *)c->mux_ctx;

	if (state == NULL)
		fatal("%s: channel %d: c->mux_ctx == NULL", __func__, c->self);
	if (state->hello_rcvd) {
		error("%s: HELLO received twice", __func__);
		return -1;
	}
	if (buffer_get_int_ret(&ver, m) != 0) {
 malf:
		error("%s: malformed message", __func__);
		return -1;
	}
	if (ver != SSHMUX_VER) {
		error("Unsupported multiplexing protocol version %d "
		    "(expected %d)", ver, SSHMUX_VER);
		return -1;
	}
	debug2("%s: channel %d slave version %u", __func__, c->self, ver);

	/* No extensions are presently defined */
	while (buffer_len(m) > 0) {
		char *name = buffer_get_string_ret(m, NULL);
		char *value = buffer_get_string_ret(m, NULL);

		if (name == NULL || value == NULL) {
			if (name != NULL)
				xfree(name);
			goto malf;
		}
		debug2("Unrecognised slave extension \"%s\"", name);
		xfree(name);
		xfree(value);
	}
	state->hello_rcvd = 1;
	return 0;
}

static int
process_mux_new_session(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	Channel *nc;
	struct mux_session_confirm_ctx *cctx;
	char *reserved, *cmd, *cp;
	u_int i, j, len, env_len, escape_char, window, packetmax;
	int new_fd[3];

	/* Reply for SSHMUX_COMMAND_OPEN */
	cctx = xcalloc(1, sizeof(*cctx));
	cctx->term = NULL;
	cmd = NULL;
	if ((reserved = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&cctx->want_tty, m) != 0 ||
	    buffer_get_int_ret(&cctx->want_x_fwd, m) != 0 ||
	    buffer_get_int_ret(&cctx->want_agent_fwd, m) != 0 ||
	    buffer_get_int_ret(&cctx->want_subsys, m) != 0 ||
	    buffer_get_int_ret(&escape_char, m) != 0 ||
	    (cctx->term = buffer_get_string_ret(m, &len)) == NULL ||
	    (cmd = buffer_get_string_ret(m, &len)) == NULL) {
 malf:
		if (cctx->term != NULL)
			xfree(cctx->term);
		error("%s: malformed message", __func__);
		return -1;
	}
	xfree(reserved);

	cctx->env = NULL;
	env_len = 0;
	while (buffer_len(m) > 0) {
#define MUX_MAX_ENV_VARS	4096
		if ((cp = buffer_get_string_ret(m, &len)) == NULL) {
			xfree(cmd);
			goto malf;
		}
		if (!env_permitted(cp)) {
			xfree(cp);
			continue;
		}
		cctx->env = xrealloc(cctx->env, env_len + 2,
		    sizeof(*cctx->env));
		cctx->env[env_len++] = cp;
		cctx->env[env_len] = NULL;
		if (env_len > MUX_MAX_ENV_VARS) {
			error(">%d environment variables received, ignoring "
			    "additional", MUX_MAX_ENV_VARS);
			break;
		}
	}

	debug2("%s: channel %d: request tty %d, X %d, agent %d, subsys %d, "
	    "term \"%s\", cmd \"%s\", env %u", __func__, c->self,
	    cctx->want_tty, cctx->want_x_fwd, cctx->want_agent_fwd,
	    cctx->want_subsys, cctx->term, cmd, env_len);

	buffer_init(&cctx->cmd);
	buffer_append(&cctx->cmd, cmd, strlen(cmd));
	xfree(cmd);

	/* Gather fds from client */
	for(i = 0; i < 3; i++) {
		if ((new_fd[i] = mm_receive_fd(c->sock)) == -1) {
			error("%s: failed to receive fd %d from slave",
			    __func__, i);
			for (j = 0; j < i; j++)
				close(new_fd[j]);
			for (j = 0; j < env_len; j++)
				xfree(cctx->env[j]);
			if (env_len > 0)
				xfree(cctx->env);
			xfree(cctx->term);
			buffer_free(&cctx->cmd);
			xfree(cctx);

			/* prepare reply */
			buffer_put_int(r, MUX_S_FAILURE);
			buffer_put_int(r, rid);
			buffer_put_cstring(r,
			    "did not receive file descriptors");
			return -1;
		}
	}

	debug3("%s: got fds stdin %d, stdout %d, stderr %d", __func__,
	    new_fd[0], new_fd[1], new_fd[2]);

	/* XXX support multiple child sessions in future */
	if (c->remote_id != -1) {
		debug2("%s: session already open", __func__);
		/* prepare reply */
		buffer_put_int(r, MUX_S_FAILURE);
		buffer_put_int(r, rid);
		buffer_put_cstring(r, "Multiple sessions not supported");
 cleanup:
		close(new_fd[0]);
		close(new_fd[1]);
		close(new_fd[2]);
		xfree(cctx->term);
		if (env_len != 0) {
			for (i = 0; i < env_len; i++)
				xfree(cctx->env[i]);
			xfree(cctx->env);
		}
		buffer_free(&cctx->cmd);
		return 0;
	}

	if (options.control_master == SSHCTL_MASTER_ASK ||
	    options.control_master == SSHCTL_MASTER_AUTO_ASK) {
		if (!ask_permission("Allow shared connection to %s? ", host)) {
			debug2("%s: session refused by user", __func__);
			/* prepare reply */
			buffer_put_int(r, MUX_S_PERMISSION_DENIED);
			buffer_put_int(r, rid);
			buffer_put_cstring(r, "Permission denied");
			goto cleanup;
		}
	}

	/* Try to pick up ttymodes from client before it goes raw */
	if (cctx->want_tty && tcgetattr(new_fd[0], &cctx->tio) == -1)
		error("%s: tcgetattr: %s", __func__, strerror(errno));

	/* enable nonblocking unless tty */
	if (!isatty(new_fd[0]))
		set_nonblock(new_fd[0]);
	if (!isatty(new_fd[1]))
		set_nonblock(new_fd[1]);
	if (!isatty(new_fd[2]))
		set_nonblock(new_fd[2]);

	window = CHAN_SES_WINDOW_DEFAULT;
	packetmax = CHAN_SES_PACKET_DEFAULT;
	if (cctx->want_tty) {
		window >>= 1;
		packetmax >>= 1;
	}

	nc = channel_new("session", SSH_CHANNEL_OPENING,
	    new_fd[0], new_fd[1], new_fd[2], window, packetmax,
	    CHAN_EXTENDED_WRITE, "client-session", /*nonblock*/0);

	nc->ctl_chan = c->self;		/* link session -> control channel */
	c->remote_id = nc->self; 	/* link control -> session channel */

	if (cctx->want_tty && escape_char != 0xffffffff) {
		channel_register_filter(nc->self,
		    client_simple_escape_filter, NULL,
		    client_filter_cleanup,
		    client_new_escape_filter_ctx((int)escape_char));
	}

	debug2("%s: channel_new: %d linked to control channel %d",
	    __func__, nc->self, nc->ctl_chan);

	channel_send_open(nc->self);
	channel_register_open_confirm(nc->self, mux_session_confirm, cctx);
	channel_register_cleanup(nc->self, mux_master_session_cleanup_cb, 0);

	/* prepare reply */
	/* XXX defer until mux_session_confirm() fires */
	buffer_put_int(r, MUX_S_SESSION_OPENED);
	buffer_put_int(r, rid);
	buffer_put_int(r, nc->self);

	return 0;
}

static int
process_mux_alive_check(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	debug2("%s: channel %d: alive check", __func__, c->self);

	/* prepare reply */
	buffer_put_int(r, MUX_S_ALIVE);
	buffer_put_int(r, rid);
	buffer_put_int(r, (u_int)getpid());

	return 0;
}

static int
process_mux_terminate(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	debug2("%s: channel %d: terminate request", __func__, c->self);

	if (options.control_master == SSHCTL_MASTER_ASK ||
	    options.control_master == SSHCTL_MASTER_AUTO_ASK) {
		if (!ask_permission("Terminate shared connection to %s? ",
		    host)) {
			debug2("%s: termination refused by user", __func__);
			buffer_put_int(r, MUX_S_PERMISSION_DENIED);
			buffer_put_int(r, rid);
			buffer_put_cstring(r, "Permission denied");
			return 0;
		}
	}

	quit_pending = 1;
	buffer_put_int(r, MUX_S_OK);
	buffer_put_int(r, rid);
	/* XXX exit happens too soon - message never makes it to client */
	return 0;
}

static char *
format_forward(u_int ftype, Forward *fwd)
{
	char *ret;

	switch (ftype) {
	case MUX_FWD_LOCAL:
		xasprintf(&ret, "local forward %.200s:%d -> %.200s:%d",
		    (fwd->listen_host == NULL) ?
		    (options.gateway_ports ? "*" : "LOCALHOST") :
		    fwd->listen_host, fwd->listen_port,
		    fwd->connect_host, fwd->connect_port);
		break;
	case MUX_FWD_DYNAMIC:
		xasprintf(&ret, "dynamic forward %.200s:%d -> *",
		    (fwd->listen_host == NULL) ?
		    (options.gateway_ports ? "*" : "LOCALHOST") :
		     fwd->listen_host, fwd->listen_port);
		break;
	case MUX_FWD_REMOTE:
		xasprintf(&ret, "remote forward %.200s:%d -> %.200s:%d",
		    (fwd->listen_host == NULL) ?
		    "LOCALHOST" : fwd->listen_host,
		    fwd->listen_port,
		    fwd->connect_host, fwd->connect_port);
		break;
	default:
		fatal("%s: unknown forward type %u", __func__, ftype);
	}
	return ret;
}

static int
compare_host(const char *a, const char *b)
{
	if (a == NULL && b == NULL)
		return 1;
	if (a == NULL || b == NULL)
		return 0;
	return strcmp(a, b) == 0;
}

static int
compare_forward(Forward *a, Forward *b)
{
	if (!compare_host(a->listen_host, b->listen_host))
		return 0;
	if (a->listen_port != b->listen_port)
		return 0;
	if (!compare_host(a->connect_host, b->connect_host))
		return 0;
	if (a->connect_port != b->connect_port)
		return 0;

	return 1;
}

static int
process_mux_open_fwd(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	Forward fwd;
	char *fwd_desc = NULL;
	u_int ftype;
	int i, ret = 0, freefwd = 1;

	fwd.listen_host = fwd.connect_host = NULL;
	if (buffer_get_int_ret(&ftype, m) != 0 ||
	    (fwd.listen_host = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&fwd.listen_port, m) != 0 ||
	    (fwd.connect_host = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&fwd.connect_port, m) != 0) {
		error("%s: malformed message", __func__);
		ret = -1;
		goto out;
	}

	if (*fwd.listen_host == '\0') {
		xfree(fwd.listen_host);
		fwd.listen_host = NULL;
	}
	if (*fwd.connect_host == '\0') {
		xfree(fwd.connect_host);
		fwd.connect_host = NULL;
	}

	debug2("%s: channel %d: request %s", __func__, c->self,
	    (fwd_desc = format_forward(ftype, &fwd)));

	if (ftype != MUX_FWD_LOCAL && ftype != MUX_FWD_REMOTE &&
	    ftype != MUX_FWD_DYNAMIC) {
		logit("%s: invalid forwarding type %u", __func__, ftype);
 invalid:
		xfree(fwd.listen_host);
		xfree(fwd.connect_host);
		buffer_put_int(r, MUX_S_FAILURE);
		buffer_put_int(r, rid);
		buffer_put_cstring(r, "Invalid forwarding request");
		return 0;
	}
	/* XXX support rport0 forwarding with reply of port assigned */
	if (fwd.listen_port == 0 || fwd.listen_port >= 65536) {
		logit("%s: invalid listen port %u", __func__,
		    fwd.listen_port);
		goto invalid;
	}
	if (fwd.connect_port >= 65536 || (ftype != MUX_FWD_DYNAMIC &&
	    ftype != MUX_FWD_REMOTE && fwd.connect_port == 0)) {
		logit("%s: invalid connect port %u", __func__,
		    fwd.connect_port);
		goto invalid;
	}
	if (ftype != MUX_FWD_DYNAMIC && fwd.connect_host == NULL) {
		logit("%s: missing connect host", __func__);
		goto invalid;
	}

	/* Skip forwards that have already been requested */
	switch (ftype) {
	case MUX_FWD_LOCAL:
	case MUX_FWD_DYNAMIC:
		for (i = 0; i < options.num_local_forwards; i++) {
			if (compare_forward(&fwd,
			    options.local_forwards + i)) {
 exists:
				debug2("%s: found existing forwarding",
				    __func__);
				buffer_put_int(r, MUX_S_OK);
				buffer_put_int(r, rid);
				goto out;
			}
		}
		break;
	case MUX_FWD_REMOTE:
		for (i = 0; i < options.num_remote_forwards; i++) {
			if (compare_forward(&fwd,
			    options.remote_forwards + i))
				goto exists;
		}
		break;
	}

	if (options.control_master == SSHCTL_MASTER_ASK ||
	    options.control_master == SSHCTL_MASTER_AUTO_ASK) {
		if (!ask_permission("Open %s on %s?", fwd_desc, host)) {
			debug2("%s: forwarding refused by user", __func__);
			buffer_put_int(r, MUX_S_PERMISSION_DENIED);
			buffer_put_int(r, rid);
			buffer_put_cstring(r, "Permission denied");
			goto out;
		}
	}

	if (ftype == MUX_FWD_LOCAL || ftype == MUX_FWD_DYNAMIC) {
		if (options.num_local_forwards + 1 >=
		    SSH_MAX_FORWARDS_PER_DIRECTION ||
		    channel_setup_local_fwd_listener(fwd.listen_host,
		    fwd.listen_port, fwd.connect_host, fwd.connect_port,
		    options.gateway_ports) < 0) {
 fail:
			logit("slave-requested %s failed", fwd_desc);
			buffer_put_int(r, MUX_S_FAILURE);
			buffer_put_int(r, rid);
			buffer_put_cstring(r, "Port forwarding failed");
			goto out;
		}
		add_local_forward(&options, &fwd);
		freefwd = 0;
	} else {
		/* XXX wait for remote to confirm */
		if (options.num_remote_forwards + 1 >=
		    SSH_MAX_FORWARDS_PER_DIRECTION ||
		    channel_request_remote_forwarding(fwd.listen_host,
		    fwd.listen_port, fwd.connect_host, fwd.connect_port) < 0)
			goto fail;
		add_remote_forward(&options, &fwd);
		freefwd = 0;
	}
	buffer_put_int(r, MUX_S_OK);
	buffer_put_int(r, rid);
 out:
	if (fwd_desc != NULL)
		xfree(fwd_desc);
	if (freefwd) {
		if (fwd.listen_host != NULL)
			xfree(fwd.listen_host);
		if (fwd.connect_host != NULL)
			xfree(fwd.connect_host);
	}
	return ret;
}

static int
process_mux_close_fwd(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	Forward fwd;
	char *fwd_desc = NULL;
	u_int ftype;
	int ret = 0;

	fwd.listen_host = fwd.connect_host = NULL;
	if (buffer_get_int_ret(&ftype, m) != 0 ||
	    (fwd.listen_host = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&fwd.listen_port, m) != 0 ||
	    (fwd.connect_host = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&fwd.connect_port, m) != 0) {
		error("%s: malformed message", __func__);
		ret = -1;
		goto out;
	}

	if (*fwd.listen_host == '\0') {
		xfree(fwd.listen_host);
		fwd.listen_host = NULL;
	}
	if (*fwd.connect_host == '\0') {
		xfree(fwd.connect_host);
		fwd.connect_host = NULL;
	}

	debug2("%s: channel %d: request %s", __func__, c->self,
	    (fwd_desc = format_forward(ftype, &fwd)));

	/* XXX implement this */
	buffer_put_int(r, MUX_S_FAILURE);
	buffer_put_int(r, rid);
	buffer_put_cstring(r, "unimplemented");

 out:
	if (fwd_desc != NULL)
		xfree(fwd_desc);
	if (fwd.listen_host != NULL)
		xfree(fwd.listen_host);
	if (fwd.connect_host != NULL)
		xfree(fwd.connect_host);

	return ret;
}

static int
process_mux_stdio_fwd(u_int rid, Channel *c, Buffer *m, Buffer *r)
{
	Channel *nc;
	char *reserved, *chost;
	u_int cport, i, j;
	int new_fd[2];

	if ((reserved = buffer_get_string_ret(m, NULL)) == NULL ||
	   (chost = buffer_get_string_ret(m, NULL)) == NULL ||
	    buffer_get_int_ret(&cport, m) != 0) {
		if (chost != NULL)
			xfree(chost);
		error("%s: malformed message", __func__);
		return -1;
	}
	xfree(reserved);

	debug2("%s: channel %d: request stdio fwd to %s:%u",
	    __func__, c->self, chost, cport);

	/* Gather fds from client */
	for(i = 0; i < 2; i++) {
		if ((new_fd[i] = mm_receive_fd(c->sock)) == -1) {
			error("%s: failed to receive fd %d from slave",
			    __func__, i);
			for (j = 0; j < i; j++)
				close(new_fd[j]);
			xfree(chost);

			/* prepare reply */
			buffer_put_int(r, MUX_S_FAILURE);
			buffer_put_int(r, rid);
			buffer_put_cstring(r,
			    "did not receive file descriptors");
			return -1;
		}
	}

	debug3("%s: got fds stdin %d, stdout %d", __func__,
	    new_fd[0], new_fd[1]);

	/* XXX support multiple child sessions in future */
	if (c->remote_id != -1) {
		debug2("%s: session already open", __func__);
		/* prepare reply */
		buffer_put_int(r, MUX_S_FAILURE);
		buffer_put_int(r, rid);
		buffer_put_cstring(r, "Multiple sessions not supported");
 cleanup:
		close(new_fd[0]);
		close(new_fd[1]);
		xfree(chost);
		return 0;
	}

	if (options.control_master == SSHCTL_MASTER_ASK ||
	    options.control_master == SSHCTL_MASTER_AUTO_ASK) {
		if (!ask_permission("Allow forward to to %s:%u? ",
		    chost, cport)) {
			debug2("%s: stdio fwd refused by user", __func__);
			/* prepare reply */
			buffer_put_int(r, MUX_S_PERMISSION_DENIED);
			buffer_put_int(r, rid);
			buffer_put_cstring(r, "Permission denied");
			goto cleanup;
		}
	}

	/* enable nonblocking unless tty */
	if (!isatty(new_fd[0]))
		set_nonblock(new_fd[0]);
	if (!isatty(new_fd[1]))
		set_nonblock(new_fd[1]);

	nc = channel_connect_stdio_fwd(chost, cport, new_fd[0], new_fd[1]);

	nc->ctl_chan = c->self;		/* link session -> control channel */
	c->remote_id = nc->self; 	/* link control -> session channel */

	debug2("%s: channel_new: %d linked to control channel %d",
	    __func__, nc->self, nc->ctl_chan);

	channel_register_cleanup(nc->self, mux_master_session_cleanup_cb, 0);

	/* prepare reply */
	/* XXX defer until channel confirmed */
	buffer_put_int(r, MUX_S_SESSION_OPENED);
	buffer_put_int(r, rid);
	buffer_put_int(r, nc->self);

	return 0;
}

/* Channel callbacks fired on read/write from mux slave fd */
static int
mux_master_read_cb(Channel *c)
{
	struct mux_master_state *state = (struct mux_master_state *)c->mux_ctx;
	Buffer in, out;
	void *ptr;
	u_int type, rid, have, i;
	int ret = -1;

	/* Setup ctx and  */
	if (c->mux_ctx == NULL) {
		state = xcalloc(1, sizeof(state));
		c->mux_ctx = state;
		channel_register_cleanup(c->self,
		    mux_master_control_cleanup_cb, 0);

		/* Send hello */
		buffer_init(&out);
		buffer_put_int(&out, MUX_MSG_HELLO);
		buffer_put_int(&out, SSHMUX_VER);
		/* no extensions */
		buffer_put_string(&c->output, buffer_ptr(&out),
		    buffer_len(&out));
		buffer_free(&out);
		debug3("%s: channel %d: hello sent", __func__, c->self);
		return 0;
	}

	buffer_init(&in);
	buffer_init(&out);

	/* Channel code ensures that we receive whole packets */
	if ((ptr = buffer_get_string_ptr_ret(&c->input, &have)) == NULL) {
 malf:
		error("%s: malformed message", __func__);
		goto out;
	}
	buffer_append(&in, ptr, have);

	if (buffer_get_int_ret(&type, &in) != 0)
		goto malf;
	debug3("%s: channel %d packet type 0x%08x len %u",
	    __func__, c->self, type, buffer_len(&in));

	if (type == MUX_MSG_HELLO)
		rid = 0;
	else {
		if (!state->hello_rcvd) {
			error("%s: expected MUX_MSG_HELLO(0x%08x), "
			    "received 0x%08x", __func__, MUX_MSG_HELLO, type);
			goto out;
		}
		if (buffer_get_int_ret(&rid, &in) != 0)
			goto malf;
	}

	for (i = 0; mux_master_handlers[i].handler != NULL; i++) {
		if (type == mux_master_handlers[i].type) {
			ret = mux_master_handlers[i].handler(rid, c, &in, &out);
			break;
		}
	}
	if (mux_master_handlers[i].handler == NULL) {
		error("%s: unsupported mux message 0x%08x", __func__, type);
		buffer_put_int(&out, MUX_S_FAILURE);
		buffer_put_int(&out, rid);
		buffer_put_cstring(&out, "unsupported request");
		ret = 0;
	}
	/* Enqueue reply packet */
	if (buffer_len(&out) != 0) {
		buffer_put_string(&c->output, buffer_ptr(&out),
		    buffer_len(&out));
	}
 out:
	buffer_free(&in);
	buffer_free(&out);
	return ret;
}

void
mux_exit_message(Channel *c, int exitval)
{
	Buffer m;
	Channel *mux_chan;

	debug3("%s: channel %d: exit message, evitval %d", __func__, c->self,
	    exitval);

	if ((mux_chan = channel_by_id(c->ctl_chan)) == NULL)
		fatal("%s: channel %d missing mux channel %d",
		    __func__, c->self, c->ctl_chan);

	/* Append exit message packet to control socket output queue */
	buffer_init(&m);
	buffer_put_int(&m, MUX_S_EXIT_MESSAGE);
	buffer_put_int(&m, c->self);
	buffer_put_int(&m, exitval);

	buffer_put_string(&mux_chan->output, buffer_ptr(&m), buffer_len(&m));
	buffer_free(&m);
}

/* Prepare a mux master to listen on a Unix domain socket. */
void
muxserver_listen(void)
{
	struct sockaddr_un addr;
	mode_t old_umask;

	if (options.control_path == NULL ||
	    options.control_master == SSHCTL_MASTER_NO)
		return;

	debug("setting up multiplex master socket");

	memset(&addr, '\0', sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_len = offsetof(struct sockaddr_un, sun_path) +
	    strlen(options.control_path) + 1;

	if (strlcpy(addr.sun_path, options.control_path,
	    sizeof(addr.sun_path)) >= sizeof(addr.sun_path))
		fatal("ControlPath too long");

	if ((muxserver_sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal("%s socket(): %s", __func__, strerror(errno));

	old_umask = umask(0177);
	if (bind(muxserver_sock, (struct sockaddr *)&addr, addr.sun_len) == -1) {
		muxserver_sock = -1;
		if (errno == EINVAL || errno == EADDRINUSE) {
			error("ControlSocket %s already exists, "
			    "disabling multiplexing", options.control_path);
			close(muxserver_sock);
			muxserver_sock = -1;
			xfree(options.control_path);
			options.control_path = NULL;
			options.control_master = SSHCTL_MASTER_NO;
			return;
		} else
			fatal("%s bind(): %s", __func__, strerror(errno));
	}
	umask(old_umask);

	if (listen(muxserver_sock, 64) == -1)
		fatal("%s listen(): %s", __func__, strerror(errno));

	set_nonblock(muxserver_sock);

	mux_listener_channel = channel_new("mux listener",
	    SSH_CHANNEL_MUX_LISTENER, muxserver_sock, muxserver_sock, -1,
	    CHAN_TCP_WINDOW_DEFAULT, CHAN_TCP_PACKET_DEFAULT,
	    0, addr.sun_path, 1);
	mux_listener_channel->mux_rcb = mux_master_read_cb;
	debug3("%s: mux listener channel %d fd %d", __func__,
	    mux_listener_channel->self, mux_listener_channel->sock);
}

/* Callback on open confirmation in mux master for a mux client session. */
static void
mux_session_confirm(int id, void *arg)
{
	struct mux_session_confirm_ctx *cctx = arg;
	const char *display;
	Channel *c;
	int i;

	if (cctx == NULL)
		fatal("%s: cctx == NULL", __func__);
	if ((c = channel_by_id(id)) == NULL)
		fatal("%s: no channel for id %d", __func__, id);

	display = getenv("DISPLAY");
	if (cctx->want_x_fwd && options.forward_x11 && display != NULL) {
		char *proto, *data;
		/* Get reasonable local authentication information. */
		client_x11_get_proto(display, options.xauth_location,
		    options.forward_x11_trusted, &proto, &data);
		/* Request forwarding with authentication spoofing. */
		debug("Requesting X11 forwarding with authentication spoofing.");
		x11_request_forwarding_with_spoofing(id, display, proto, data);
		/* XXX wait for reply */
	}

	if (cctx->want_agent_fwd && options.forward_agent) {
		debug("Requesting authentication agent forwarding.");
		channel_request_start(id, "auth-agent-req@openssh.com", 0);
		packet_send();
	}

	client_session2_setup(id, cctx->want_tty, cctx->want_subsys,
	    cctx->term, &cctx->tio, c->rfd, &cctx->cmd, cctx->env);

	c->open_confirm_ctx = NULL;
	buffer_free(&cctx->cmd);
	xfree(cctx->term);
	if (cctx->env != NULL) {
		for (i = 0; cctx->env[i] != NULL; i++)
			xfree(cctx->env[i]);
		xfree(cctx->env);
	}
	xfree(cctx);
}

/* ** Multiplexing client support */

/* Exit signal handler */
static void
control_client_sighandler(int signo)
{
	muxclient_terminate = signo;
}

/*
 * Relay signal handler - used to pass some signals from mux client to
 * mux master.
 */
static void
control_client_sigrelay(int signo)
{
	int save_errno = errno;

	if (muxserver_pid > 1)
		kill(muxserver_pid, signo);

	errno = save_errno;
}

static int
mux_client_read(int fd, Buffer *b, u_int need)
{
	u_int have;
	ssize_t len;
	u_char *p;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLIN;
	p = buffer_append_space(b, need);
	for (have = 0; have < need; ) {
		if (muxclient_terminate) {
			errno = EINTR;
			return -1;
		}
		len = read(fd, p + have, need - have);
		if (len < 0) {
			switch (errno) {
			case EAGAIN:
				(void)poll(&pfd, 1, -1);
				/* FALLTHROUGH */
			case EINTR:
				continue;
			default:
				return -1;
			}
		}
		if (len == 0) {
			errno = EPIPE;
			return -1;
		}
		have += (u_int)len;
	}
	return 0;
}

static int
mux_client_write_packet(int fd, Buffer *m)
{
	Buffer queue;
	u_int have, need;
	int oerrno, len;
	u_char *ptr;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = POLLOUT;
	buffer_init(&queue);
	buffer_put_string(&queue, buffer_ptr(m), buffer_len(m));

	need = buffer_len(&queue);
	ptr = buffer_ptr(&queue);

	for (have = 0; have < need; ) {
		if (muxclient_terminate) {
			buffer_free(&queue);
			errno = EINTR;
			return -1;
		}
		len = write(fd, ptr + have, need - have);
		if (len < 0) {
			switch (errno) {
			case EAGAIN:
				(void)poll(&pfd, 1, -1);
				/* FALLTHROUGH */
			case EINTR:
				continue;
			default:
				oerrno = errno;
				buffer_free(&queue);
				errno = oerrno;
				return -1;
			}
		}
		if (len == 0) {
			buffer_free(&queue);
			errno = EPIPE;
			return -1;
		}
		have += (u_int)len;
	}
	buffer_free(&queue);
	return 0;
}

static int
mux_client_read_packet(int fd, Buffer *m)
{
	Buffer queue;
	u_int need, have;
	void *ptr;
	int oerrno;

	buffer_init(&queue);
	if (mux_client_read(fd, &queue, 4) != 0) {
		if ((oerrno = errno) == EPIPE)
		debug3("%s: read header failed: %s", __func__, strerror(errno));
		errno = oerrno;
		return -1;
	}
	need = get_u32(buffer_ptr(&queue));
	if (mux_client_read(fd, &queue, need) != 0) {
		oerrno = errno;
		debug3("%s: read body failed: %s", __func__, strerror(errno));
		errno = oerrno;
		return -1;
	}
	ptr = buffer_get_string_ptr(&queue, &have);
	buffer_append(m, ptr, have);
	buffer_free(&queue);
	return 0;
}

static int
mux_client_hello_exchange(int fd)
{
	Buffer m;
	u_int type, ver;

	buffer_init(&m);
	buffer_put_int(&m, MUX_MSG_HELLO);
	buffer_put_int(&m, SSHMUX_VER);
	/* no extensions */

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	buffer_clear(&m);

	/* Read their HELLO */
	if (mux_client_read_packet(fd, &m) != 0) {
		buffer_free(&m);
		return -1;
	}

	type = buffer_get_int(&m);
	if (type != MUX_MSG_HELLO)
		fatal("%s: expected HELLO (%u) received %u",
		    __func__, MUX_MSG_HELLO, type);
	ver = buffer_get_int(&m);
	if (ver != SSHMUX_VER)
		fatal("Unsupported multiplexing protocol version %d "
		    "(expected %d)", ver, SSHMUX_VER);
	debug2("%s: master version %u", __func__, ver);
	/* No extensions are presently defined */
	while (buffer_len(&m) > 0) {
		char *name = buffer_get_string(&m, NULL);
		char *value = buffer_get_string(&m, NULL);

		debug2("Unrecognised master extension \"%s\"", name);
		xfree(name);
		xfree(value);
	}
	buffer_free(&m);
	return 0;
}

static u_int
mux_client_request_alive(int fd)
{
	Buffer m;
	char *e;
	u_int pid, type, rid;

	debug3("%s: entering", __func__);

	buffer_init(&m);
	buffer_put_int(&m, MUX_C_ALIVE_CHECK);
	buffer_put_int(&m, muxclient_request_id);

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	buffer_clear(&m);

	/* Read their reply */
	if (mux_client_read_packet(fd, &m) != 0) {
		buffer_free(&m);
		return 0;
	}

	type = buffer_get_int(&m);
	if (type != MUX_S_ALIVE) {
		e = buffer_get_string(&m, NULL);
		fatal("%s: master returned error: %s", __func__, e);
	}

	if ((rid = buffer_get_int(&m)) != muxclient_request_id)
		fatal("%s: out of sequence reply: my id %u theirs %u",
		    __func__, muxclient_request_id, rid);
	pid = buffer_get_int(&m);
	buffer_free(&m);

	debug3("%s: done pid = %u", __func__, pid);

	muxclient_request_id++;

	return pid;
}

static void
mux_client_request_terminate(int fd)
{
	Buffer m;
	char *e;
	u_int type, rid;

	debug3("%s: entering", __func__);

	buffer_init(&m);
	buffer_put_int(&m, MUX_C_TERMINATE);
	buffer_put_int(&m, muxclient_request_id);

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	buffer_clear(&m);

	/* Read their reply */
	if (mux_client_read_packet(fd, &m) != 0) {
		/* Remote end exited already */
		if (errno == EPIPE) {
			buffer_free(&m);
			return;
		}
		fatal("%s: read from master failed: %s",
		    __func__, strerror(errno));
	}

	type = buffer_get_int(&m);
	if ((rid = buffer_get_int(&m)) != muxclient_request_id)
		fatal("%s: out of sequence reply: my id %u theirs %u",
		    __func__, muxclient_request_id, rid);
	switch (type) {
	case MUX_S_OK:
		break;
	case MUX_S_PERMISSION_DENIED:
		e = buffer_get_string(&m, NULL);
		fatal("Master refused termination request: %s", e);
	case MUX_S_FAILURE:
		e = buffer_get_string(&m, NULL);
		fatal("%s: termination request failed: %s", __func__, e);
	default:
		fatal("%s: unexpected response from master 0x%08x",
		    __func__, type);
	}
	buffer_free(&m);
	muxclient_request_id++;
}

static int
mux_client_request_forward(int fd, u_int ftype, Forward *fwd)
{
	Buffer m;
	char *e, *fwd_desc;
	u_int type, rid;

	fwd_desc = format_forward(ftype, fwd);
	debug("Requesting %s", fwd_desc);
	xfree(fwd_desc);

	buffer_init(&m);
	buffer_put_int(&m, MUX_C_OPEN_FWD);
	buffer_put_int(&m, muxclient_request_id);
	buffer_put_int(&m, ftype);
	buffer_put_cstring(&m,
	    fwd->listen_host == NULL ? "" : fwd->listen_host);
	buffer_put_int(&m, fwd->listen_port);
	buffer_put_cstring(&m,
	    fwd->connect_host == NULL ? "" : fwd->connect_host);
	buffer_put_int(&m, fwd->connect_port);

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	buffer_clear(&m);

	/* Read their reply */
	if (mux_client_read_packet(fd, &m) != 0) {
		buffer_free(&m);
		return -1;
	}

	type = buffer_get_int(&m);
	if ((rid = buffer_get_int(&m)) != muxclient_request_id)
		fatal("%s: out of sequence reply: my id %u theirs %u",
		    __func__, muxclient_request_id, rid);
	switch (type) {
	case MUX_S_OK:
		break;
	case MUX_S_PERMISSION_DENIED:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		error("Master refused forwarding request: %s", e);
		return -1;
	case MUX_S_FAILURE:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		error("%s: session request failed: %s", __func__, e);
		return -1;
	default:
		fatal("%s: unexpected response from master 0x%08x",
		    __func__, type);
	}
	buffer_free(&m);

	muxclient_request_id++;
	return 0;
}

static int
mux_client_request_forwards(int fd)
{
	int i;

	debug3("%s: requesting forwardings: %d local, %d remote", __func__,
	    options.num_local_forwards, options.num_remote_forwards);

	/* XXX ExitOnForwardingFailure */
	for (i = 0; i < options.num_local_forwards; i++) {
		if (mux_client_request_forward(fd,
		    options.local_forwards[i].connect_port == 0 ?
		    MUX_FWD_DYNAMIC : MUX_FWD_LOCAL,
		    options.local_forwards + i) != 0)
			return -1;
	}
	for (i = 0; i < options.num_remote_forwards; i++) {
		if (mux_client_request_forward(fd, MUX_FWD_REMOTE,
		    options.remote_forwards + i) != 0)
			return -1;
	}
	return 0;
}

static int
mux_client_request_session(int fd)
{
	Buffer m;
	char *e, *term;
	u_int i, rid, sid, esid, exitval, type, exitval_seen;
	extern char **environ;
	int devnull;

	debug3("%s: entering", __func__);

	if ((muxserver_pid = mux_client_request_alive(fd)) == 0) {
		error("%s: master alive request failed", __func__);
		return -1;
	}

	signal(SIGPIPE, SIG_IGN);

	if (stdin_null_flag) {
		if ((devnull = open(_PATH_DEVNULL, O_RDONLY)) == -1)
			fatal("open(/dev/null): %s", strerror(errno));
		if (dup2(devnull, STDIN_FILENO) == -1)
			fatal("dup2: %s", strerror(errno));
		if (devnull > STDERR_FILENO)
			close(devnull);
	}

	term = getenv("TERM");

	buffer_init(&m);
	buffer_put_int(&m, MUX_C_NEW_SESSION);
	buffer_put_int(&m, muxclient_request_id);
	buffer_put_cstring(&m, ""); /* reserved */
	buffer_put_int(&m, tty_flag);
	buffer_put_int(&m, options.forward_x11);
	buffer_put_int(&m, options.forward_agent);
	buffer_put_int(&m, subsystem_flag);
	buffer_put_int(&m, options.escape_char == SSH_ESCAPECHAR_NONE ?
	    0xffffffff : (u_int)options.escape_char);
	buffer_put_cstring(&m, term == NULL ? "" : term);
	buffer_put_string(&m, buffer_ptr(&command), buffer_len(&command));

	if (options.num_send_env > 0 && environ != NULL) {
		/* Pass environment */
		for (i = 0; environ[i] != NULL; i++) {
			if (env_permitted(environ[i])) {
				buffer_put_cstring(&m, environ[i]);
			}
		}
	}

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	/* Send the stdio file descriptors */
	if (mm_send_fd(fd, STDIN_FILENO) == -1 ||
	    mm_send_fd(fd, STDOUT_FILENO) == -1 ||
	    mm_send_fd(fd, STDERR_FILENO) == -1)
		fatal("%s: send fds failed", __func__);

	debug3("%s: session request sent", __func__);

	/* Read their reply */
	buffer_clear(&m);
	if (mux_client_read_packet(fd, &m) != 0) {
		error("%s: read from master failed: %s",
		    __func__, strerror(errno));
		buffer_free(&m);
		return -1;
	}

	type = buffer_get_int(&m);
	if ((rid = buffer_get_int(&m)) != muxclient_request_id)
		fatal("%s: out of sequence reply: my id %u theirs %u",
		    __func__, muxclient_request_id, rid);
	switch (type) {
	case MUX_S_SESSION_OPENED:
		sid = buffer_get_int(&m);
		debug("%s: master session id: %u", __func__, sid);
		break;
	case MUX_S_PERMISSION_DENIED:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		error("Master refused forwarding request: %s", e);
		return -1;
	case MUX_S_FAILURE:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		error("%s: forwarding request failed: %s", __func__, e);
		return -1;
	default:
		buffer_free(&m);
		error("%s: unexpected response from master 0x%08x",
		    __func__, type);
		return -1;
	}
	muxclient_request_id++;

	signal(SIGHUP, control_client_sighandler);
	signal(SIGINT, control_client_sighandler);
	signal(SIGTERM, control_client_sighandler);
	signal(SIGWINCH, control_client_sigrelay);

	if (tty_flag)
		enter_raw_mode(force_tty_flag);

	/*
	 * Stick around until the controlee closes the client_fd.
	 * Before it does, it is expected to write an exit message.
	 * This process must read the value and wait for the closure of
	 * the client_fd; if this one closes early, the multiplex master will
	 * terminate early too (possibly losing data).
	 */
	for (exitval = 255, exitval_seen = 0;;) {
		buffer_clear(&m);
		if (mux_client_read_packet(fd, &m) != 0)
			break;
		type = buffer_get_int(&m);
		if (type != MUX_S_EXIT_MESSAGE) {
			e = buffer_get_string(&m, NULL);
			fatal("%s: master returned error: %s", __func__, e);
		}
		if ((esid = buffer_get_int(&m)) != sid)
			fatal("%s: exit on unknown session: my id %u theirs %u",
			    __func__, sid, esid);
		debug("%s: master session id: %u", __func__, sid);
		if (exitval_seen)
			fatal("%s: exitval sent twice", __func__);
		exitval = buffer_get_int(&m);
		exitval_seen = 1;
	}

	close(fd);
	leave_raw_mode(force_tty_flag);

	if (muxclient_terminate) {
		debug2("Exiting on signal %d", muxclient_terminate);
		exitval = 255;
	} else if (!exitval_seen) {
		debug2("Control master terminated unexpectedly");
		exitval = 255;
	} else
		debug2("Received exit status from master %d", exitval);

	if (tty_flag && options.log_level != SYSLOG_LEVEL_QUIET)
		fprintf(stderr, "Shared connection to %s closed.\r\n", host);

	exit(exitval);
}

static int
mux_client_request_stdio_fwd(int fd)
{
	Buffer m;
	char *e;
	u_int type, rid, sid;
	int devnull;

	debug3("%s: entering", __func__);

	if ((muxserver_pid = mux_client_request_alive(fd)) == 0) {
		error("%s: master alive request failed", __func__);
		return -1;
	}

	signal(SIGPIPE, SIG_IGN);

	if (stdin_null_flag) {
		if ((devnull = open(_PATH_DEVNULL, O_RDONLY)) == -1)
			fatal("open(/dev/null): %s", strerror(errno));
		if (dup2(devnull, STDIN_FILENO) == -1)
			fatal("dup2: %s", strerror(errno));
		if (devnull > STDERR_FILENO)
			close(devnull);
	}

	buffer_init(&m);
	buffer_put_int(&m, MUX_C_NEW_STDIO_FWD);
	buffer_put_int(&m, muxclient_request_id);
	buffer_put_cstring(&m, ""); /* reserved */
	buffer_put_cstring(&m, stdio_forward_host);
	buffer_put_int(&m, stdio_forward_port);

	if (mux_client_write_packet(fd, &m) != 0)
		fatal("%s: write packet: %s", __func__, strerror(errno));

	/* Send the stdio file descriptors */
	if (mm_send_fd(fd, STDIN_FILENO) == -1 ||
	    mm_send_fd(fd, STDOUT_FILENO) == -1)
		fatal("%s: send fds failed", __func__);

	debug3("%s: stdio forward request sent", __func__);

	/* Read their reply */
	buffer_clear(&m);

	if (mux_client_read_packet(fd, &m) != 0) {
		error("%s: read from master failed: %s",
		    __func__, strerror(errno));
		buffer_free(&m);
		return -1;
	}

	type = buffer_get_int(&m);
	if ((rid = buffer_get_int(&m)) != muxclient_request_id)
		fatal("%s: out of sequence reply: my id %u theirs %u",
		    __func__, muxclient_request_id, rid);
	switch (type) {
	case MUX_S_SESSION_OPENED:
		sid = buffer_get_int(&m);
		debug("%s: master session id: %u", __func__, sid);
		break;
	case MUX_S_PERMISSION_DENIED:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		fatal("Master refused forwarding request: %s", e);
	case MUX_S_FAILURE:
		e = buffer_get_string(&m, NULL);
		buffer_free(&m);
		fatal("%s: stdio forwarding request failed: %s", __func__, e);
	default:
		buffer_free(&m);
		error("%s: unexpected response from master 0x%08x",
		    __func__, type);
		return -1;
	}
	muxclient_request_id++;

	signal(SIGHUP, control_client_sighandler);
	signal(SIGINT, control_client_sighandler);
	signal(SIGTERM, control_client_sighandler);
	signal(SIGWINCH, control_client_sigrelay);

	/*
	 * Stick around until the controlee closes the client_fd.
	 */
	buffer_clear(&m);
	if (mux_client_read_packet(fd, &m) != 0) {
		if (errno == EPIPE ||
		    (errno == EINTR && muxclient_terminate != 0))
			return 0;
		fatal("%s: mux_client_read_packet: %s",
		    __func__, strerror(errno));
	}
	fatal("%s: master returned unexpected message %u", __func__, type);
}

/* Multiplex client main loop. */
void
muxclient(const char *path)
{
	struct sockaddr_un addr;
	int sock;
	u_int pid;

	if (muxclient_command == 0) {
		if (stdio_forward_host != NULL)
			muxclient_command = SSHMUX_COMMAND_STDIO_FWD;
		else
			muxclient_command = SSHMUX_COMMAND_OPEN;
	}

	switch (options.control_master) {
	case SSHCTL_MASTER_AUTO:
	case SSHCTL_MASTER_AUTO_ASK:
		debug("auto-mux: Trying existing master");
		/* FALLTHROUGH */
	case SSHCTL_MASTER_NO:
		break;
	default:
		return;
	}

	memset(&addr, '\0', sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_len = offsetof(struct sockaddr_un, sun_path) +
	    strlen(path) + 1;

	if (strlcpy(addr.sun_path, path,
	    sizeof(addr.sun_path)) >= sizeof(addr.sun_path))
		fatal("ControlPath too long");

	if ((sock = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		fatal("%s socket(): %s", __func__, strerror(errno));

	if (connect(sock, (struct sockaddr *)&addr, addr.sun_len) == -1) {
		switch (muxclient_command) {
		case SSHMUX_COMMAND_OPEN:
		case SSHMUX_COMMAND_STDIO_FWD:
			break;
		default:
			fatal("Control socket connect(%.100s): %s", path,
			    strerror(errno));
		}
		if (errno == ENOENT)
			debug("Control socket \"%.100s\" does not exist", path);
		else {
			error("Control socket connect(%.100s): %s", path,
			    strerror(errno));
		}
		close(sock);
		return;
	}
	set_nonblock(sock);

	if (mux_client_hello_exchange(sock) != 0) {
		error("%s: master hello exchange failed", __func__);
		close(sock);
		return;
	}

	switch (muxclient_command) {
	case SSHMUX_COMMAND_ALIVE_CHECK:
		if ((pid = mux_client_request_alive(sock)) == 0)
			fatal("%s: master alive check failed", __func__);
		fprintf(stderr, "Master running (pid=%d)\r\n", pid);
		exit(0);
	case SSHMUX_COMMAND_TERMINATE:
		mux_client_request_terminate(sock);
		fprintf(stderr, "Exit request sent.\r\n");
		exit(0);
	case SSHMUX_COMMAND_OPEN:
		if (mux_client_request_forwards(sock) != 0) {
			error("%s: master forward request failed", __func__);
			return;
		}
		mux_client_request_session(sock);
		return;
	case SSHMUX_COMMAND_STDIO_FWD:
		mux_client_request_stdio_fwd(sock);
		exit(0);
	default:
		fatal("unrecognised muxclient_command %d", muxclient_command);
	}
}
