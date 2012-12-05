/* @(#)s_copysign.c 5.1 93/09/24 */
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice 
 * is preserved.
 * ====================================================
 */

/*
 * copysign(double x, double y)
 * copysign(x,y) returns a value with the magnitude of x and
 * with the sign bit of y.
 */

/* LINTLIBRARY */

#include <float.h>
#include <math.h>

#include "math_private.h"

double
copysign(double x, double y)
{
	u_int32_t hx,hy;
	GET_HIGH_WORD(hx,x);
	GET_HIGH_WORD(hy,y);
	SET_HIGH_WORD(x,(hx&0x7fffffff)|(hy&0x80000000));
        return x;
}

#if	LDBL_MANT_DIG == 53
#ifdef	lint
/* PROTOLIB1 */
long double copysignl(long double, long double);
#else	/* lint */
__weak_alias(copysignl, copysign);
#endif	/* lint */
#endif	/* LDBL_MANT_DIG == 53 */
