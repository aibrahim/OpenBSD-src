/*	$OpenBSD: fpgetsticky.c,v 1.2 2003/01/07 22:01:29 miod Exp $	*/

/*
 * Written by J.T. Conklin, Apr 10, 1995
 * Public domain.
 * Ported to 88k by Nivas Madhur
 */

#include <ieeefp.h>

#if defined(LIBC_SCCS) && !defined(lint)
static char rcsid[] = "$OpenBSD: fpgetsticky.c,v 1.2 2003/01/07 22:01:29 miod Exp $";
#endif /* LIBC_SCCS and not lint */

fp_except
fpgetsticky()
{
	int x;

	__asm__ volatile("fldcr %0,fcr62" : "=r" (x));
	return x & 0x1f;
}
