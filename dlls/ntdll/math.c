/*
 * Math functions
 *
 * Copyright 2021 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 * For functions copied from musl libc (http://musl.libc.org/):
 * ====================================================
 * Copyright 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * ====================================================
 */

#include <math.h>
#include <float.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntdll_misc.h"

double math_error( int type, const char *name, double arg1, double arg2, double retval )
{
    return retval;
}

/*********************************************************************
 *                  abs   (NTDLL.@)
 */
int CDECL abs( int i )
{
    return i >= 0 ? i : -i;
}

/*********************************************************************
 *                  ceil   (NTDLL.@)
 *
 * Based on musl: src/math/ceilf.c
 */
double CDECL ceil( double x )
{
    union {double f; UINT64 i;} u = {x};
    int e = (u.i >> 52 & 0x7ff) - 0x3ff;
    UINT64 m;

    if (e >= 52)
        return x;
    if (e >= 0) {
        m = 0x000fffffffffffffULL >> e;
        if ((u.i & m) == 0)
            return x;
        if (u.i >> 63 == 0)
            u.i += m;
        u.i &= ~m;
    } else {
        if (u.i >> 63)
            return -0.0;
        else if (u.i << 1)
            return 1.0;
    }
    return u.f;
}

/*********************************************************************
 *                  fabs   (NTDLL.@)
 *
 * Copied from musl: src/math/fabsf.c
 */
double CDECL fabs( double x )
{
    union { double f; UINT64 i; } u = { x };
    u.i &= ~0ull >> 1;
    return u.f;
}

/*********************************************************************
 *                  floor   (NTDLL.@)
 *
 * Based on musl: src/math/floorf.c
 */
double CDECL floor( double x )
{
    union {double f; UINT64 i;} u = {x};
    int e = (int)(u.i >> 52 & 0x7ff) - 0x3ff;
    UINT64 m;

    if (e >= 52)
        return x;
    if (e >= 0) {
        m = 0x000fffffffffffffULL >> e;
        if ((u.i & m) == 0)
            return x;
        if (u.i >> 63)
            u.i += m;
        u.i &= ~m;
    } else {
        if (u.i >> 63 == 0)
            return 0;
        else if (u.i << 1)
            return -1;
    }
    return u.f;
}

#if (defined(__GNUC__) || defined(__clang__)) && defined(__i386__)

#define FPU_DOUBLE(var) double var; \
    __asm__ __volatile__( "fstpl %0;fwait" : "=m" (var) : )
#define FPU_DOUBLES(var1,var2) double var1,var2; \
    __asm__ __volatile__( "fstpl %0;fwait" : "=m" (var2) : ); \
    __asm__ __volatile__( "fstpl %0;fwait" : "=m" (var1) : )

/*********************************************************************
 *		_CIcos (NTDLL.@)
 */
double CDECL _CIcos(void)
{
    FPU_DOUBLE(x);
    return cos(x);
}

/*********************************************************************
 *		_CIlog (NTDLL.@)
 */
double CDECL _CIlog(void)
{
    FPU_DOUBLE(x);
    return log(x);
}

/*********************************************************************
 *		_CIpow (NTDLL.@)
 */
double CDECL _CIpow(void)
{
    FPU_DOUBLES(x,y);
    return pow(x,y);
}

/*********************************************************************
 *		_CIsin (NTDLL.@)
 */
double CDECL _CIsin(void)
{
    FPU_DOUBLE(x);
    return sin(x);
}

/*********************************************************************
 *		_CIsqrt (NTDLL.@)
 */
double CDECL _CIsqrt(void)
{
    FPU_DOUBLE(x);
    return sqrt(x);
}

/*********************************************************************
 *                  _ftol   (NTDLL.@)
 */
LONGLONG CDECL _ftol(void)
{
    FPU_DOUBLE(x);
    return (LONGLONG)x;
}

#endif /* (defined(__GNUC__) || defined(__clang__)) && defined(__i386__) */
