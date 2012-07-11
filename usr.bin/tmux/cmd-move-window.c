/* $OpenBSD: cmd-move-window.c,v 1.14 2012/07/11 07:10:15 nicm Exp $ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicm@users.sourceforge.net>
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

#include <sys/types.h>

#include <stdlib.h>

#include "tmux.h"

/*
 * Move a window.
 */

enum cmd_retval	 cmd_move_window_exec(struct cmd *, struct cmd_ctx *);

const struct cmd_entry cmd_move_window_entry = {
	"move-window", "movew",
	"dkrs:t:", 0, 0,
	"[-dkr] " CMD_SRCDST_WINDOW_USAGE,
	0,
	NULL,
	NULL,
	cmd_move_window_exec
};

enum cmd_retval
cmd_move_window_exec(struct cmd *self, struct cmd_ctx *ctx)
{
	struct args	*args = self->args;
	struct session	*src, *dst, *s;
	struct winlink	*wl;
	char		*cause;
	int		 idx, kflag, dflag;

	if (args_has(args, 'r')) {
		if ((s = cmd_find_session(ctx, args_get(args, 't'), 0)) == NULL)
			return (CMD_RETURN_ERROR);

		session_renumber_windows(s);
		recalculate_sizes();

		return (CMD_RETURN_NORMAL);
	}

	if ((wl = cmd_find_window(ctx, args_get(args, 's'), &src)) == NULL)
		return (CMD_RETURN_ERROR);
	if ((idx = cmd_find_index(ctx, args_get(args, 't'), &dst)) == -2)
		return (CMD_RETURN_ERROR);

	kflag = args_has(self->args, 'k');
	dflag = args_has(self->args, 'd');
	if (server_link_window(src, wl, dst, idx, kflag, !dflag, &cause) != 0) {
		ctx->error(ctx, "can't move window: %s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	server_unlink_window(src, wl);
	recalculate_sizes();

	return (CMD_RETURN_NORMAL);
}
