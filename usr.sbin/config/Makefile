#	$OpenBSD: Makefile,v 1.15 2013/07/05 21:29:51 miod Exp $

.include <bsd.own.mk>

PROG=	config
SRCS=	files.c gram.y hash.c main.c mkheaders.c mkioconf.c mkmakefile.c \
	mkswap.c pack.c scan.l sem.c util.c \
	ukc.c misc.c ukcutil.c cmd.c exec.c
CFLAGS+=-DELF_SUPPORT
SRCS+=  exec_elf.c

CFLAGS+=-I${.CURDIR} -I.

LDADD=	-lkvm
DPADD=	${LIBKVM}

MAN=	config.8

.include <bsd.prog.mk>
