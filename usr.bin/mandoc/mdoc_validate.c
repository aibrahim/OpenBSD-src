/*	$Id: mdoc_validate.c,v 1.74 2010/10/24 18:15:43 schwarze Exp $ */
/*
 * Copyright (c) 2008, 2009, 2010 Kristaps Dzonsons <kristaps@bsd.lv>
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
#include <sys/types.h>

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mandoc.h"
#include "libmdoc.h"
#include "libmandoc.h"

/* FIXME: .Bl -diag can't have non-text children in HEAD. */

#define	PRE_ARGS  struct mdoc *mdoc, struct mdoc_node *n
#define	POST_ARGS struct mdoc *mdoc

enum	check_ineq {
	CHECK_LT,
	CHECK_GT,
	CHECK_EQ
};

enum	check_lvl {
	CHECK_WARN,
	CHECK_ERROR,
	CHECK_FATAL
};

typedef	int	(*v_pre)(PRE_ARGS);
typedef	int	(*v_post)(POST_ARGS);

struct	valids {
	v_pre	*pre;
	v_post	*post;
};

static	int	 check_count(struct mdoc *, enum mdoc_type, 
			enum check_lvl, enum check_ineq, int);
static	int	 check_parent(PRE_ARGS, enum mdoct, enum mdoc_type);
static	int	 check_stdarg(PRE_ARGS);
static	int	 check_text(struct mdoc *, int, int, char *);
static	int	 check_argv(struct mdoc *, 
			struct mdoc_node *, struct mdoc_argv *);
static	int	 check_args(struct mdoc *, struct mdoc_node *);

static	int	 ebool(POST_ARGS);
static	int	 berr_ge1(POST_ARGS);
static	int	 bwarn_ge1(POST_ARGS);
static	int	 eerr_eq0(POST_ARGS);
static	int	 eerr_eq1(POST_ARGS);
static	int	 eerr_ge1(POST_ARGS);
static	int	 eerr_le1(POST_ARGS);
static	int	 ewarn_eq0(POST_ARGS);
static	int	 ewarn_ge1(POST_ARGS);
static	int	 herr_eq0(POST_ARGS);
static	int	 herr_ge1(POST_ARGS);
static	int	 hwarn_eq0(POST_ARGS);
static	int	 hwarn_eq1(POST_ARGS);
static	int	 hwarn_le1(POST_ARGS);

static	int	 post_an(POST_ARGS);
static	int	 post_at(POST_ARGS);
static	int	 post_bf(POST_ARGS);
static	int	 post_bl(POST_ARGS);
static	int	 post_bl_head(POST_ARGS);
static	int	 post_dt(POST_ARGS);
static	int	 post_it(POST_ARGS);
static	int	 post_lb(POST_ARGS);
static	int	 post_nm(POST_ARGS);
static	int	 post_root(POST_ARGS);
static	int	 post_rs(POST_ARGS);
static	int	 post_sh(POST_ARGS);
static	int	 post_sh_body(POST_ARGS);
static	int	 post_sh_head(POST_ARGS);
static	int	 post_st(POST_ARGS);
static	int	 post_eoln(POST_ARGS);
static	int	 post_vt(POST_ARGS);
static	int	 pre_an(PRE_ARGS);
static	int	 pre_bd(PRE_ARGS);
static	int	 pre_bl(PRE_ARGS);
static	int	 pre_dd(PRE_ARGS);
static	int	 pre_display(PRE_ARGS);
static	int	 pre_dt(PRE_ARGS);
static	int	 pre_it(PRE_ARGS);
static	int	 pre_os(PRE_ARGS);
static	int	 pre_pp(PRE_ARGS);
static	int	 pre_rv(PRE_ARGS);
static	int	 pre_sh(PRE_ARGS);
static	int	 pre_ss(PRE_ARGS);

static	v_post	 posts_an[] = { post_an, NULL };
static	v_post	 posts_at[] = { post_at, NULL };
static	v_post	 posts_bd_bk[] = { hwarn_eq0, bwarn_ge1, NULL };
static	v_post	 posts_bf[] = { hwarn_le1, post_bf, NULL };
static	v_post	 posts_bl[] = { bwarn_ge1, post_bl, NULL };
static	v_post	 posts_bool[] = { eerr_eq1, ebool, NULL };
static	v_post	 posts_eoln[] = { post_eoln, NULL };
static	v_post	 posts_dt[] = { post_dt, NULL };
static	v_post	 posts_fo[] = { hwarn_eq1, bwarn_ge1, NULL };
static	v_post	 posts_it[] = { post_it, NULL };
static	v_post	 posts_lb[] = { eerr_eq1, post_lb, NULL };
static	v_post	 posts_nd[] = { berr_ge1, NULL };
static	v_post	 posts_nm[] = { post_nm, NULL };
static	v_post	 posts_notext[] = { ewarn_eq0, NULL };
static	v_post	 posts_rs[] = { berr_ge1, herr_eq0, post_rs, NULL };
static	v_post	 posts_sh[] = { herr_ge1, bwarn_ge1, post_sh, NULL };
static	v_post	 posts_sp[] = { eerr_le1, NULL };
static	v_post	 posts_ss[] = { herr_ge1, NULL };
static	v_post	 posts_st[] = { eerr_eq1, post_st, NULL };
static	v_post	 posts_text[] = { eerr_ge1, NULL };
static	v_post	 posts_text1[] = { eerr_eq1, NULL };
static	v_post	 posts_vt[] = { post_vt, NULL };
static	v_post	 posts_wline[] = { bwarn_ge1, herr_eq0, NULL };
static	v_post	 posts_wtext[] = { ewarn_ge1, NULL };
static	v_pre	 pres_an[] = { pre_an, NULL };
static	v_pre	 pres_bd[] = { pre_display, pre_bd, pre_pp, NULL };
static	v_pre	 pres_bl[] = { pre_bl, pre_pp, NULL };
static	v_pre	 pres_d1[] = { pre_display, NULL };
static	v_pre	 pres_dd[] = { pre_dd, NULL };
static	v_pre	 pres_dt[] = { pre_dt, NULL };
static	v_pre	 pres_er[] = { NULL, NULL };
static	v_pre	 pres_ex[] = { NULL, NULL };
static	v_pre	 pres_fd[] = { NULL, NULL };
static	v_pre	 pres_it[] = { pre_it, NULL };
static	v_pre	 pres_os[] = { pre_os, NULL };
static	v_pre	 pres_pp[] = { pre_pp, NULL };
static	v_pre	 pres_rv[] = { pre_rv, NULL };
static	v_pre	 pres_sh[] = { pre_sh, NULL };
static	v_pre	 pres_ss[] = { pre_ss, NULL };

const	struct valids mdoc_valids[MDOC_MAX] = {
	{ NULL, NULL },				/* Ap */
	{ pres_dd, posts_wtext },		/* Dd */
	{ pres_dt, posts_dt },			/* Dt */
	{ pres_os, NULL },			/* Os */
	{ pres_sh, posts_sh },			/* Sh */ 
	{ pres_ss, posts_ss },			/* Ss */ 
	{ pres_pp, posts_notext },		/* Pp */ 
	{ pres_d1, posts_wline },		/* D1 */
	{ pres_d1, posts_wline },		/* Dl */
	{ pres_bd, posts_bd_bk },		/* Bd */
	{ NULL, NULL },				/* Ed */
	{ pres_bl, posts_bl },			/* Bl */ 
	{ NULL, NULL },				/* El */
	{ pres_it, posts_it },			/* It */
	{ NULL, posts_text },			/* Ad */ 
	{ pres_an, posts_an },			/* An */ 
	{ NULL, NULL },				/* Ar */
	{ NULL, posts_text },			/* Cd */ 
	{ NULL, NULL },				/* Cm */
	{ NULL, NULL },				/* Dv */ 
	{ pres_er, posts_text },		/* Er */ 
	{ NULL, NULL },				/* Ev */ 
	{ pres_ex, NULL },			/* Ex */ 
	{ NULL, NULL },				/* Fa */ 
	{ pres_fd, posts_wtext },		/* Fd */
	{ NULL, NULL },				/* Fl */
	{ NULL, posts_text },			/* Fn */ 
	{ NULL, posts_wtext },			/* Ft */ 
	{ NULL, posts_text },			/* Ic */ 
	{ NULL, posts_text1 },			/* In */ 
	{ NULL, NULL },				/* Li */
	{ NULL, posts_nd },			/* Nd */
	{ NULL, posts_nm },			/* Nm */
	{ NULL, posts_wline },			/* Op */
	{ NULL, NULL },				/* Ot */
	{ NULL, NULL },				/* Pa */
	{ pres_rv, NULL },			/* Rv */
	{ NULL, posts_st },			/* St */ 
	{ NULL, NULL },				/* Va */
	{ NULL, posts_vt },			/* Vt */ 
	{ NULL, posts_wtext },			/* Xr */ 
	{ NULL, posts_text },			/* %A */
	{ NULL, posts_text },			/* %B */ /* FIXME: can be used outside Rs/Re. */
	{ NULL, posts_text },			/* %D */ /* FIXME: check date with mandoc_a2time(). */
	{ NULL, posts_text },			/* %I */
	{ NULL, posts_text },			/* %J */
	{ NULL, posts_text },			/* %N */
	{ NULL, posts_text },			/* %O */
	{ NULL, posts_text },			/* %P */
	{ NULL, posts_text },			/* %R */
	{ NULL, posts_text },			/* %T */ /* FIXME: can be used outside Rs/Re. */
	{ NULL, posts_text },			/* %V */
	{ NULL, NULL },				/* Ac */
	{ NULL, NULL },				/* Ao */
	{ NULL, posts_wline },			/* Aq */
	{ NULL, posts_at },			/* At */ 
	{ NULL, NULL },				/* Bc */
	{ NULL, posts_bf },			/* Bf */
	{ NULL, NULL },				/* Bo */
	{ NULL, posts_wline },			/* Bq */
	{ NULL, NULL },				/* Bsx */
	{ NULL, NULL },				/* Bx */
	{ NULL, posts_bool },			/* Db */
	{ NULL, NULL },				/* Dc */
	{ NULL, NULL },				/* Do */
	{ NULL, posts_wline },			/* Dq */
	{ NULL, NULL },				/* Ec */
	{ NULL, NULL },				/* Ef */ 
	{ NULL, NULL },				/* Em */ 
	{ NULL, NULL },				/* Eo */
	{ NULL, NULL },				/* Fx */
	{ NULL, posts_text },			/* Ms */ 
	{ NULL, posts_notext },			/* No */
	{ NULL, posts_notext },			/* Ns */
	{ NULL, NULL },				/* Nx */
	{ NULL, NULL },				/* Ox */
	{ NULL, NULL },				/* Pc */
	{ NULL, posts_text1 },			/* Pf */
	{ NULL, NULL },				/* Po */
	{ NULL, posts_wline },			/* Pq */
	{ NULL, NULL },				/* Qc */
	{ NULL, posts_wline },			/* Ql */
	{ NULL, NULL },				/* Qo */
	{ NULL, posts_wline },			/* Qq */
	{ NULL, NULL },				/* Re */
	{ NULL, posts_rs },			/* Rs */
	{ NULL, NULL },				/* Sc */
	{ NULL, NULL },				/* So */
	{ NULL, posts_wline },			/* Sq */
	{ NULL, posts_bool },			/* Sm */ 
	{ NULL, posts_text },			/* Sx */
	{ NULL, posts_text },			/* Sy */
	{ NULL, posts_text },			/* Tn */
	{ NULL, NULL },				/* Ux */
	{ NULL, NULL },				/* Xc */
	{ NULL, NULL },				/* Xo */
	{ NULL, posts_fo },			/* Fo */ 
	{ NULL, NULL },				/* Fc */ 
	{ NULL, NULL },				/* Oo */
	{ NULL, NULL },				/* Oc */
	{ NULL, posts_bd_bk },			/* Bk */
	{ NULL, NULL },				/* Ek */
	{ NULL, posts_eoln },			/* Bt */
	{ NULL, NULL },				/* Hf */
	{ NULL, NULL },				/* Fr */
	{ NULL, posts_eoln },			/* Ud */
	{ NULL, posts_lb },			/* Lb */
	{ NULL, posts_notext },			/* Lp */ 
	{ NULL, posts_text },			/* Lk */ 
	{ NULL, posts_text },			/* Mt */ 
	{ NULL, posts_wline },			/* Brq */ 
	{ NULL, NULL },				/* Bro */ 
	{ NULL, NULL },				/* Brc */ 
	{ NULL, posts_text },			/* %C */
	{ NULL, NULL },				/* Es */
	{ NULL, NULL },				/* En */
	{ NULL, NULL },				/* Dx */
	{ NULL, posts_text },			/* %Q */
	{ NULL, posts_notext },			/* br */
	{ pres_pp, posts_sp },			/* sp */
	{ NULL, posts_text1 },			/* %U */
	{ NULL, NULL },				/* Ta */
	{ NULL, NULL },				/* TS */
	{ NULL, NULL },				/* TE */
};


int
mdoc_valid_pre(struct mdoc *mdoc, struct mdoc_node *n)
{
	v_pre		*p;
	int		 line, pos;
	char		*tp;

	if (MDOC_TEXT == n->type) {
		tp = n->string;
		line = n->line;
		pos = n->pos;
		return(check_text(mdoc, line, pos, tp));
	}

	if ( ! check_args(mdoc, n))
		return(0);
	if (NULL == mdoc_valids[n->tok].pre)
		return(1);
	for (p = mdoc_valids[n->tok].pre; *p; p++)
		if ( ! (*p)(mdoc, n)) 
			return(0);
	return(1);
}


int
mdoc_valid_post(struct mdoc *mdoc)
{
	v_post		*p;

	if (MDOC_VALID & mdoc->last->flags)
		return(1);
	mdoc->last->flags |= MDOC_VALID;

	if (MDOC_TEXT == mdoc->last->type)
		return(1);
	if (MDOC_ROOT == mdoc->last->type)
		return(post_root(mdoc));

	if (NULL == mdoc_valids[mdoc->last->tok].post)
		return(1);
	for (p = mdoc_valids[mdoc->last->tok].post; *p; p++)
		if ( ! (*p)(mdoc)) 
			return(0);

	return(1);
}

static int
check_count(struct mdoc *m, enum mdoc_type type, 
		enum check_lvl lvl, enum check_ineq ineq, int val)
{
	const char	*p;

	if (m->last->type != type)
		return(1);
	
	switch (ineq) {
	case (CHECK_LT):
		p = "less than ";
		if (m->last->nchild < val)
			return(1);
		break;
	case (CHECK_GT):
		p = "greater than ";
		if (m->last->nchild > val)
			return(1);
		break;
	case (CHECK_EQ):
		p = "";
		if (val == m->last->nchild)
			return(1);
		break;
	}

	if (CHECK_WARN == lvl) {
		return(mdoc_vmsg(m, MANDOCERR_ARGCOUNT,
				m->last->line, m->last->pos,
				"want %s%d children (have %d)",
				p, val, m->last->nchild));
	}

	return(mdoc_vmsg(m, MANDOCERR_ARGCOUNT,
			m->last->line, m->last->pos,
			"require %s%d children (have %d)",
			p, val, m->last->nchild));
}

static int
berr_ge1(POST_ARGS)
{

	return(check_count(mdoc, MDOC_BODY, CHECK_FATAL, CHECK_GT, 0));
}

static int
bwarn_ge1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_BODY, CHECK_WARN, CHECK_GT, 0));
}

static int
eerr_eq0(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_FATAL, CHECK_EQ, 0));
}

static int
eerr_eq1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_FATAL, CHECK_EQ, 1));
}

static int
eerr_ge1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_FATAL, CHECK_GT, 0));
}

static int
eerr_le1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_FATAL, CHECK_LT, 2));
}

static int
ewarn_eq0(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_WARN, CHECK_EQ, 0));
}

static int
ewarn_ge1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_ELEM, CHECK_WARN, CHECK_GT, 0));
}

static int
herr_eq0(POST_ARGS)
{
	return(check_count(mdoc, MDOC_HEAD, CHECK_FATAL, CHECK_EQ, 0));
}

static int
herr_ge1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_HEAD, CHECK_FATAL, CHECK_GT, 0));
}

static int
hwarn_eq0(POST_ARGS)
{
	return(check_count(mdoc, MDOC_HEAD, CHECK_WARN, CHECK_EQ, 0));
}

static int
hwarn_eq1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_HEAD, CHECK_WARN, CHECK_EQ, 1));
}

static int
hwarn_le1(POST_ARGS)
{
	return(check_count(mdoc, MDOC_HEAD, CHECK_WARN, CHECK_LT, 2));
}


static int
check_stdarg(PRE_ARGS)
{

	if (n->args && 1 == n->args->argc)
		if (MDOC_Std == n->args->argv[0].arg)
			return(1);
	return(mdoc_nmsg(mdoc, n, MANDOCERR_NOARGV));
}


static int
check_args(struct mdoc *m, struct mdoc_node *n)
{
	int		 i;

	if (NULL == n->args)
		return(1);

	assert(n->args->argc);
	for (i = 0; i < (int)n->args->argc; i++)
		if ( ! check_argv(m, n, &n->args->argv[i]))
			return(0);

	return(1);
}


static int
check_argv(struct mdoc *m, struct mdoc_node *n, struct mdoc_argv *v)
{
	int		 i;

	for (i = 0; i < (int)v->sz; i++)
		if ( ! check_text(m, v->line, v->pos, v->value[i]))
			return(0);

	if (MDOC_Std == v->arg) {
		if (v->sz || m->meta.name)
			return(1);
		if ( ! mdoc_nmsg(m, n, MANDOCERR_NONAME))
			return(0);
	}

	return(1);
}


static int
check_text(struct mdoc *m, int ln, int pos, char *p)
{
	int		 c;
	size_t		 sz;

	for ( ; *p; p++, pos++) {
		sz = strcspn(p, "\t\\");
		p += (int)sz;

		if ('\0' == *p)
			break;

		pos += (int)sz;

		if ('\t' == *p) {
			if (MDOC_LITERAL & m->flags)
				continue;
			if (mdoc_pmsg(m, ln, pos, MANDOCERR_BADTAB))
				continue;
			return(0);
		}

		/* Check the special character. */

		c = mandoc_special(p);
		if (c) {
			p += c - 1;
			pos += c - 1;
		} else
			mdoc_pmsg(m, ln, pos, MANDOCERR_BADESCAPE);
	}

	return(1);
}


static int
check_parent(PRE_ARGS, enum mdoct tok, enum mdoc_type t)
{

	assert(n->parent);
	if ((MDOC_ROOT == t || tok == n->parent->tok) &&
			(t == n->parent->type))
		return(1);

	mdoc_vmsg(mdoc, MANDOCERR_SYNTCHILD,
				n->line, n->pos, "want parent %s",
				MDOC_ROOT == t ? "<root>" : 
					mdoc_macronames[tok]);
	return(0);
}


static int
pre_display(PRE_ARGS)
{
	struct mdoc_node *node;

	if (MDOC_BLOCK != n->type)
		return(1);

	for (node = mdoc->last->parent; node; node = node->parent) 
		if (MDOC_BLOCK == node->type)
			if (MDOC_Bd == node->tok)
				break;
	if (node)
		mdoc_nmsg(mdoc, n, MANDOCERR_NESTEDDISP);

	return(1);
}


static int
pre_bl(PRE_ARGS)
{
	int		  i, comp, dup;
	const char	 *offs, *width;
	enum mdoc_list	  lt;
	struct mdoc_node *np;

	if (MDOC_BLOCK != n->type) {
		if (ENDBODY_NOT != n->end) {
			assert(n->pending);
			np = n->pending->parent;
		} else
			np = n->parent;

		assert(np);
		assert(MDOC_BLOCK == np->type);
		assert(MDOC_Bl == np->tok);
		assert(np->data.Bl);
		n->data.Bl = np->data.Bl;
		return(1);
	}

	/* 
	 * First figure out which kind of list to use: bind ourselves to
	 * the first mentioned list type and warn about any remaining
	 * ones.  If we find no list type, we default to LIST_item.
	 */

	assert(NULL == n->data.Bl);
	n->data.Bl = mandoc_calloc(1, sizeof(struct mdoc_bl));

	/* LINTED */
	for (i = 0; n->args && i < (int)n->args->argc; i++) {
		lt = LIST__NONE;
		dup = comp = 0;
		width = offs = NULL;
		switch (n->args->argv[i].arg) {
		/* Set list types. */
		case (MDOC_Bullet):
			lt = LIST_bullet;
			break;
		case (MDOC_Dash):
			lt = LIST_dash;
			break;
		case (MDOC_Enum):
			lt = LIST_enum;
			break;
		case (MDOC_Hyphen):
			lt = LIST_hyphen;
			break;
		case (MDOC_Item):
			lt = LIST_item;
			break;
		case (MDOC_Tag):
			lt = LIST_tag;
			break;
		case (MDOC_Diag):
			lt = LIST_diag;
			break;
		case (MDOC_Hang):
			lt = LIST_hang;
			break;
		case (MDOC_Ohang):
			lt = LIST_ohang;
			break;
		case (MDOC_Inset):
			lt = LIST_inset;
			break;
		case (MDOC_Column):
			lt = LIST_column;
			break;
		/* Set list arguments. */
		case (MDOC_Compact):
			dup = n->data.Bl->comp;
			comp = 1;
			break;
		case (MDOC_Width):
			dup = (NULL != n->data.Bl->width);
			width = n->args->argv[i].value[0];
			break;
		case (MDOC_Offset):
			/* NB: this can be empty! */
			if (n->args->argv[i].sz) {
				offs = n->args->argv[i].value[0];
				dup = (NULL != n->data.Bl->offs);
				break;
			}
			if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_IGNARGV))
				return(0);
			break;
		default:
			continue;
		}

		/* Check: duplicate auxiliary arguments. */

		if (dup && ! mdoc_nmsg(mdoc, n, MANDOCERR_ARGVREP))
			return(0);

		if (comp && ! dup)
			n->data.Bl->comp = comp;
		if (offs && ! dup)
			n->data.Bl->offs = offs;
		if (width && ! dup)
			n->data.Bl->width = width;

		/* Check: multiple list types. */

		if (LIST__NONE != lt && n->data.Bl->type != LIST__NONE)
			if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_LISTREP))
				return(0);

		/* Assign list type. */

		if (LIST__NONE != lt && n->data.Bl->type == LIST__NONE) {
			n->data.Bl->type = lt;
			/* Set column information, too. */
			if (LIST_column == lt) {
				n->data.Bl->ncols = 
					n->args->argv[i].sz;
				n->data.Bl->cols = (const char **)
					n->args->argv[i].value;
			}
		}

		/* The list type should come first. */

		if (n->data.Bl->type == LIST__NONE)
			if (n->data.Bl->width || 
					n->data.Bl->offs || 
					n->data.Bl->comp)
				if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_LISTFIRST))
					return(0);

		continue;
	}

	/* Allow lists to default to LIST_item. */

	if (LIST__NONE == n->data.Bl->type) {
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_LISTTYPE))
			return(0);
		n->data.Bl->type = LIST_item;
	}

	/* 
	 * Validate the width field.  Some list types don't need width
	 * types and should be warned about them.  Others should have it
	 * and must also be warned.
	 */

	switch (n->data.Bl->type) {
	case (LIST_tag):
		if (n->data.Bl->width)
			break;
		if (mdoc_nmsg(mdoc, n, MANDOCERR_NOWIDTHARG))
			break;
		return(0);
	case (LIST_column):
		/* FALLTHROUGH */
	case (LIST_diag):
		/* FALLTHROUGH */
	case (LIST_ohang):
		/* FALLTHROUGH */
	case (LIST_inset):
		/* FALLTHROUGH */
	case (LIST_item):
		if (NULL == n->data.Bl->width)
			break;
		if (mdoc_nmsg(mdoc, n, MANDOCERR_WIDTHARG))
			break;
		return(0);
	default:
		break;
	}

	return(1);
}


static int
pre_bd(PRE_ARGS)
{
	int		  i, dup, comp;
	enum mdoc_disp 	  dt;
	const char	 *offs;
	struct mdoc_node *np;

	if (MDOC_BLOCK != n->type) {
		if (ENDBODY_NOT != n->end) {
			assert(n->pending);
			np = n->pending->parent;
		} else
			np = n->parent;

		assert(np);
		assert(MDOC_BLOCK == np->type);
		assert(MDOC_Bd == np->tok);
		assert(np->data.Bd);
		n->data.Bd = np->data.Bd;
		return(1);
	}

	assert(NULL == n->data.Bd);
	n->data.Bd = mandoc_calloc(1, sizeof(struct mdoc_bd));

	/* LINTED */
	for (i = 0; n->args && i < (int)n->args->argc; i++) {
		dt = DISP__NONE;
		dup = comp = 0;
		offs = NULL;

		switch (n->args->argv[i].arg) {
		case (MDOC_Centred):
			dt = DISP_centred;
			break;
		case (MDOC_Ragged):
			dt = DISP_ragged;
			break;
		case (MDOC_Unfilled):
			dt = DISP_unfilled;
			break;
		case (MDOC_Filled):
			dt = DISP_filled;
			break;
		case (MDOC_Literal):
			dt = DISP_literal;
			break;
		case (MDOC_File):
			mdoc_nmsg(mdoc, n, MANDOCERR_BADDISP);
			return(0);
		case (MDOC_Offset):
			/* NB: this can be empty! */
			if (n->args->argv[i].sz) {
				offs = n->args->argv[i].value[0];
				dup = (NULL != n->data.Bd->offs);
				break;
			}
			if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_IGNARGV))
				return(0);
			break;
		case (MDOC_Compact):
			comp = 1;
			dup = n->data.Bd->comp;
			break;
		default:
			abort();
			/* NOTREACHED */
		}

		/* Check whether we have duplicates. */

		if (dup && ! mdoc_nmsg(mdoc, n, MANDOCERR_ARGVREP))
			return(0);

		/* Make our auxiliary assignments. */

		if (offs && ! dup)
			n->data.Bd->offs = offs;
		if (comp && ! dup)
			n->data.Bd->comp = comp;

		/* Check whether a type has already been assigned. */

		if (DISP__NONE != dt && n->data.Bd->type != DISP__NONE)
			if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_DISPREP))
				return(0);

		/* Make our type assignment. */

		if (DISP__NONE != dt && n->data.Bd->type == DISP__NONE)
			n->data.Bd->type = dt;
	}

	if (DISP__NONE == n->data.Bd->type) {
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_DISPTYPE))
			return(0);
		n->data.Bd->type = DISP_ragged;
	}

	return(1);
}


static int
pre_ss(PRE_ARGS)
{

	if (MDOC_BLOCK != n->type)
		return(1);
	return(check_parent(mdoc, n, MDOC_Sh, MDOC_BODY));
}


static int
pre_sh(PRE_ARGS)
{

	if (MDOC_BLOCK != n->type)
		return(1);

	mdoc->regs->regs[(int)REG_nS].set = 0;
	return(check_parent(mdoc, n, MDOC_MAX, MDOC_ROOT));
}


static int
pre_it(PRE_ARGS)
{

	if (MDOC_BLOCK != n->type)
		return(1);
	/* 
	 * FIXME: this can probably be lifted if we make the It into
	 * something else on-the-fly?
	 */
	return(check_parent(mdoc, n, MDOC_Bl, MDOC_BODY));
}


static int
pre_an(PRE_ARGS)
{
	int		 i;

	if (NULL == n->args)
		return(1);
	
	for (i = 1; i < (int)n->args->argc; i++)
		if ( ! mdoc_pmsg(mdoc, n->args->argv[i].line,
			n->args->argv[i].pos, MANDOCERR_IGNARGV))
			return(0);

	if (MDOC_Split == n->args->argv[0].arg)
		n->data.An.auth = AUTH_split;
	else if (MDOC_Nosplit == n->args->argv[0].arg)
		n->data.An.auth = AUTH_nosplit;
	else
		abort();

	return(1);
}


static int
pre_rv(PRE_ARGS)
{

	return(check_stdarg(mdoc, n));
}


static int
post_dt(POST_ARGS)
{
	const struct mdoc_node *nn;
	const char	*p;

	if (NULL != (nn = mdoc->last->child))
		for (p = nn->string; *p; p++) {
			if (toupper((u_char)*p) == *p)
				continue;
			if ( ! mdoc_nmsg(mdoc, nn, MANDOCERR_UPPERCASE))
				return(0);
			break;
		}

	return(1);
}


static int
pre_dt(PRE_ARGS)
{

	if (0 == mdoc->meta.date || mdoc->meta.os)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGOOO))
			return(0);
	if (mdoc->meta.title)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGREP))
			return(0);
	return(1);
}


static int
pre_os(PRE_ARGS)
{

	if (NULL == mdoc->meta.title || 0 == mdoc->meta.date)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGOOO))
			return(0);
	if (mdoc->meta.os)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGREP))
			return(0);
	return(1);
}


static int
pre_dd(PRE_ARGS)
{

	if (mdoc->meta.title || mdoc->meta.os)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGOOO))
			return(0);
	if (mdoc->meta.date)
		if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_PROLOGREP))
			return(0);
	return(1);
}


static int
post_bf(POST_ARGS)
{
	struct mdoc_node *np;
	enum mdocargt	  arg;

	/*
	 * Unlike other data pointers, these are "housed" by the HEAD
	 * element, which contains the goods.
	 */

	if (MDOC_HEAD != mdoc->last->type) {
		if (ENDBODY_NOT != mdoc->last->end) {
			assert(mdoc->last->pending);
			np = mdoc->last->pending->parent->head;
		} else if (MDOC_BLOCK != mdoc->last->type) {
			np = mdoc->last->parent->head;
		} else 
			np = mdoc->last->head;

		assert(np);
		assert(MDOC_HEAD == np->type);
		assert(MDOC_Bf == np->tok);
		assert(np->data.Bf);
		mdoc->last->data.Bf = np->data.Bf;
		return(1);
	}

	np = mdoc->last;
	assert(MDOC_BLOCK == np->parent->type);
	assert(MDOC_Bf == np->parent->tok);
	np->data.Bf = mandoc_calloc(1, sizeof(struct mdoc_bf));

	/* 
	 * Cannot have both argument and parameter.
	 * If neither is specified, let it through with a warning. 
	 */

	if (np->parent->args && np->child) {
		mdoc_nmsg(mdoc, np, MANDOCERR_SYNTARGVCOUNT);
		return(0);
	} else if (NULL == np->parent->args && NULL == np->child)
		return(mdoc_nmsg(mdoc, np, MANDOCERR_FONTTYPE));

	/* Extract argument into data. */
	
	if (np->parent->args) {
		arg = np->parent->args->argv[0].arg;
		if (MDOC_Emphasis == arg)
			np->data.Bf->font = FONT_Em;
		else if (MDOC_Literal == arg)
			np->data.Bf->font = FONT_Li;
		else if (MDOC_Symbolic == arg)
			np->data.Bf->font = FONT_Sy;
		else
			abort();
		return(1);
	}

	/* Extract parameter into data. */

	if (0 == strcmp(np->child->string, "Em"))
		np->data.Bf->font = FONT_Em;
	else if (0 == strcmp(np->child->string, "Li"))
		np->data.Bf->font = FONT_Li;
	else if (0 == strcmp(np->child->string, "Sy"))
		np->data.Bf->font = FONT_Sy;
	else if ( ! mdoc_nmsg(mdoc, np, MANDOCERR_FONTTYPE))
		return(0);

	return(1);
}


static int
post_lb(POST_ARGS)
{

	if (mdoc_a2lib(mdoc->last->child->string))
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADLIB));
}


static int
post_eoln(POST_ARGS)
{

	if (NULL == mdoc->last->child)
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_ARGSLOST));
}


static int
post_vt(POST_ARGS)
{
	const struct mdoc_node *n;

	/*
	 * The Vt macro comes in both ELEM and BLOCK form, both of which
	 * have different syntaxes (yet more context-sensitive
	 * behaviour).  ELEM types must have a child; BLOCK types,
	 * specifically the BODY, should only have TEXT children.
	 */

	if (MDOC_ELEM == mdoc->last->type)
		return(eerr_ge1(mdoc));
	if (MDOC_BODY != mdoc->last->type)
		return(1);
	
	for (n = mdoc->last->child; n; n = n->next)
		if (MDOC_TEXT != n->type) 
			if ( ! mdoc_nmsg(mdoc, n, MANDOCERR_CHILD))
				return(0);

	return(1);
}


static int
post_nm(POST_ARGS)
{

	if (mdoc->last->child)
		return(1);
	if (mdoc->meta.name)
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NONAME));
}


static int
post_at(POST_ARGS)
{

	if (NULL == mdoc->last->child)
		return(1);
	assert(MDOC_TEXT == mdoc->last->child->type);
	if (mdoc_a2att(mdoc->last->child->string))
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADATT));
}


static int
post_an(POST_ARGS)
{
	struct mdoc_node *np;

	np = mdoc->last;
	if (AUTH__NONE != np->data.An.auth && np->child)
		return(eerr_eq0(mdoc));
	/* 
	 * FIXME: make this ewarn and make sure that the front-ends
	 * don't print the arguments.
	 */
	if (AUTH__NONE != np->data.An.auth || np->child)
		return(1);
	return(mdoc_nmsg(mdoc, np, MANDOCERR_NOARGS));
}


static int
post_it(POST_ARGS)
{
	int		  i, cols, rc;
	enum mdoc_list	  lt;
	struct mdoc_node *n, *c;
	enum mandocerr	  er;

	if (MDOC_BLOCK != mdoc->last->type)
		return(1);

	n = mdoc->last->parent->parent;
	assert(n->data.Bl);
	lt = n->data.Bl->type;

	if (LIST__NONE == lt) {
		mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_LISTTYPE);
		return(0);
	}

	switch (lt) {
	case (LIST_tag):
		if (mdoc->last->head->child)
			break;
		/* FIXME: give this a dummy value. */
		if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NOARGS))
			return(0);
		break;
	case (LIST_hang):
		/* FALLTHROUGH */
	case (LIST_ohang):
		/* FALLTHROUGH */
	case (LIST_inset):
		/* FALLTHROUGH */
	case (LIST_diag):
		if (NULL == mdoc->last->head->child)
			if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NOARGS))
				return(0);
		break;
	case (LIST_bullet):
		/* FALLTHROUGH */
	case (LIST_dash):
		/* FALLTHROUGH */
	case (LIST_enum):
		/* FALLTHROUGH */
	case (LIST_hyphen):
		if (NULL == mdoc->last->body->child)
			if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NOBODY))
				return(0);
		/* FALLTHROUGH */
	case (LIST_item):
		if (mdoc->last->head->child)
			if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_ARGSLOST))
				return(0);
		break;
	case (LIST_column):
		cols = (int)n->data.Bl->ncols;

		assert(NULL == mdoc->last->head->child);

		if (NULL == mdoc->last->body->child)
			if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NOBODY))
				return(0);

		for (i = 0, c = mdoc->last->child; c; c = c->next)
			if (MDOC_BODY == c->type)
				i++;

		if (i < cols)
			er = MANDOCERR_ARGCOUNT;
		else if (i == cols || i == cols + 1)
			break;
		else
			er = MANDOCERR_SYNTARGCOUNT;

		rc = mdoc_vmsg(mdoc, er, 
				mdoc->last->line, mdoc->last->pos, 
				"columns == %d (have %d)", cols, i);
		return(rc);
	default:
		break;
	}

	return(1);
}


static int
post_bl_head(POST_ARGS) 
{
	struct mdoc_node *n;

	assert(mdoc->last->parent);
	n = mdoc->last->parent;

	if (LIST_column == n->data.Bl->type) {
		if (n->data.Bl->ncols && mdoc->last->nchild) {
			mdoc_nmsg(mdoc, n, MANDOCERR_COLUMNS);
			return(0);
		}
		return(1);
	}

	/* FIXME: should be ERROR class. */
	return(hwarn_eq0(mdoc));
}


static int
post_bl(POST_ARGS)
{
	struct mdoc_node	*n;

	if (MDOC_HEAD == mdoc->last->type) 
		return(post_bl_head(mdoc));
	if (MDOC_BODY != mdoc->last->type)
		return(1);
	if (NULL == mdoc->last->child)
		return(1);

	/*
	 * We only allow certain children of `Bl'.  This is usually on
	 * `It', but apparently `Sm' occurs here and there, so we let
	 * that one through, too.
	 */

	/* LINTED */
	for (n = mdoc->last->child; n; n = n->next) {
		if (MDOC_BLOCK == n->type && MDOC_It == n->tok)
			continue;
		if (MDOC_Sm == n->tok)
			continue;
		mdoc_nmsg(mdoc, n, MANDOCERR_SYNTCHILD);
		return(0);
	}

	return(1);
}


static int
ebool(struct mdoc *mdoc)
{
	struct mdoc_node *n;

	/* LINTED */
	for (n = mdoc->last->child; n; n = n->next) {
		if (MDOC_TEXT != n->type)
			break;
		if (0 == strcmp(n->string, "on"))
			continue;
		if (0 == strcmp(n->string, "off"))
			continue;
		break;
	}

	if (NULL == n)
		return(1);
	return(mdoc_nmsg(mdoc, n, MANDOCERR_BADBOOL));
}


static int
post_root(POST_ARGS)
{

	if (NULL == mdoc->first->child)
		mdoc_nmsg(mdoc, mdoc->first, MANDOCERR_NODOCBODY);
	else if ( ! (MDOC_PBODY & mdoc->flags))
		mdoc_nmsg(mdoc, mdoc->first, MANDOCERR_NODOCPROLOG);
	else if (MDOC_BLOCK != mdoc->first->child->type)
		mdoc_nmsg(mdoc, mdoc->first, MANDOCERR_NODOCBODY);
	else if (MDOC_Sh != mdoc->first->child->tok)
		mdoc_nmsg(mdoc, mdoc->first, MANDOCERR_NODOCBODY);
	else
		return(1);

	return(0);
}


static int
post_st(POST_ARGS)
{

	if (mdoc_a2st(mdoc->last->child->string))
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADSTANDARD));
}


static int
post_rs(POST_ARGS)
{
	struct mdoc_node	*nn;

	if (MDOC_BODY != mdoc->last->type)
		return(1);

	for (nn = mdoc->last->child; nn; nn = nn->next)
		switch (nn->tok) {
		case(MDOC__U):
			/* FALLTHROUGH */
		case(MDOC__Q):
			/* FALLTHROUGH */
		case(MDOC__C):
			/* FALLTHROUGH */
		case(MDOC__A):
			/* FALLTHROUGH */
		case(MDOC__B):
			/* FALLTHROUGH */
		case(MDOC__D):
			/* FALLTHROUGH */
		case(MDOC__I):
			/* FALLTHROUGH */
		case(MDOC__J):
			/* FALLTHROUGH */
		case(MDOC__N):
			/* FALLTHROUGH */
		case(MDOC__O):
			/* FALLTHROUGH */
		case(MDOC__P):
			/* FALLTHROUGH */
		case(MDOC__R):
			/* FALLTHROUGH */
		case(MDOC__T):
			/* FALLTHROUGH */
		case(MDOC__V):
			break;
		default:
			mdoc_nmsg(mdoc, nn, MANDOCERR_SYNTCHILD);
			return(0);
		}

	return(1);
}


static int
post_sh(POST_ARGS)
{

	if (MDOC_HEAD == mdoc->last->type)
		return(post_sh_head(mdoc));
	if (MDOC_BODY == mdoc->last->type)
		return(post_sh_body(mdoc));

	return(1);
}


static int
post_sh_body(POST_ARGS)
{
	struct mdoc_node *n;

	if (SEC_NAME != mdoc->lastsec)
		return(1);

	/*
	 * Warn if the NAME section doesn't contain the `Nm' and `Nd'
	 * macros (can have multiple `Nm' and one `Nd').  Note that the
	 * children of the BODY declaration can also be "text".
	 */

	if (NULL == (n = mdoc->last->child))
		return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADNAMESEC));

	for ( ; n && n->next; n = n->next) {
		if (MDOC_ELEM == n->type && MDOC_Nm == n->tok)
			continue;
		if (MDOC_TEXT == n->type)
			continue;
		if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADNAMESEC))
			return(0);
	}

	assert(n);
	if (MDOC_BLOCK == n->type && MDOC_Nd == n->tok)
		return(1);
	return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_BADNAMESEC));
}


static int
post_sh_head(POST_ARGS)
{
	char		        buf[BUFSIZ];
	enum mdoc_sec	        sec;
	const struct mdoc_node *n;

	/*
	 * Process a new section.  Sections are either "named" or
	 * "custom"; custom sections are user-defined, while named ones
	 * usually follow a conventional order and may only appear in
	 * certain manual sections.
	 */

	buf[0] = '\0';

	/*
	 * FIXME: yes, these can use a dynamic buffer, but I don't do so
	 * in the interests of simplicity.
	 */

	for (n = mdoc->last->child; n; n = n->next) {
		/* XXX - copied from compact(). */
		assert(MDOC_TEXT == n->type);

		if (strlcat(buf, n->string, BUFSIZ) >= BUFSIZ) {
			mdoc_nmsg(mdoc, n, MANDOCERR_MEM);
			return(0);
		}
		if (NULL == n->next)
			continue;
		if (strlcat(buf, " ", BUFSIZ) >= BUFSIZ) {
			mdoc_nmsg(mdoc, n, MANDOCERR_MEM);
			return(0);
		}
	}

	sec = mdoc_str2sec(buf);

	/* 
	 * Check: NAME should always be first, CUSTOM has no roles,
	 * non-CUSTOM has a conventional order to be followed.
	 */

	if (SEC_NAME != sec && SEC_NONE == mdoc->lastnamed)
		if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_NAMESECFIRST))
			return(0);

	if (SEC_CUSTOM == sec)
		return(1);

	if (sec == mdoc->lastnamed)
		if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_SECREP))
			return(0);

	if (sec < mdoc->lastnamed)
		if ( ! mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_SECOOO))
			return(0);

	/* 
	 * Check particular section/manual conventions.  LIBRARY can
	 * only occur in manual section 2, 3, and 9.
	 */

	switch (sec) {
	case (SEC_LIBRARY):
		assert(mdoc->meta.msec);
		if (*mdoc->meta.msec == '2')
			break;
		if (*mdoc->meta.msec == '3')
			break;
		if (*mdoc->meta.msec == '9')
			break;
		return(mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_SECMSEC));
	default:
		break;
	}

	return(1);
}


static int
pre_pp(PRE_ARGS)
{

	if (NULL == mdoc->last)
		return(1);

	/* Don't allow prior `Lp' or `Pp'. */

	if (MDOC_Pp != mdoc->last->tok && MDOC_Lp != mdoc->last->tok)
		return(1);

	if (MDOC_Bl == n->tok && n->data.Bl->comp)
		return(1);
	if (MDOC_Bd == n->tok && n->data.Bd->comp)
		return(1);

	mdoc_nmsg(mdoc, mdoc->last, MANDOCERR_IGNPAR);
	mdoc_node_delete(mdoc, mdoc->last);
	return(1);
}
