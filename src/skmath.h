/* ----------------------------------------------------------------------------------------------
 * Copyright (C) 2023-2024 Jure Bagić
 *
 * This file is part of Skooma.
 * Skooma is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Skooma is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Skooma.
 * If not, see <https://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------------------------*/

#ifndef SKMATH_H
#define SKMATH_H

/* Primitive operations over skooma numbers */
#define sk_nadd(vm, a, b) ((a) + (b))
#define sk_nsub(vm, a, b) ((a) - (b))
#define sk_nmul(vm, a, b) ((a) * (b))
#define sk_ndiv(vm, a, b) ((a) / (b))
#define sk_nmod(vm, a, b) cast_lint(cast_lint(a) % cast_lint(b))
#define sk_npow(vm, a, b) sk_pow(a, b)
#define sk_numin(vm, a)   (-(a))

#include <math.h>

/* Functions */
#define sk_abs(x)        abs(x)
#define sk_labs(x)       labs(x)
#define sk_llabs(x)      llabs(x)
#define sk_div(x, y)     div(x, y)
#define sk_ldiv(x, y)    ldiv(x, y)
#define sk_lldiv(x, y)   lldiv(x, y)
#define sk_imaxabs(x)    imaxabs(x)
#define sk_imaxdiv(x, y) imaxdiv(x, y)

/* Basic operations */
#define sk_fabs(x)          fabs(x)
#define sk_fabsf(x)         fabsf(x)
#define sk_fabsl(x)         fabsfl(x)
#define sk_mod(x, y)        mod(x, y)
#define sk_modf(x, y)       modf(x, y)
#define sk_modl(x, y)       modl(x, y)
#define sk_remainder(x, y)  remainder(x, y)
#define sk_remainderf(x, y) remainderf(x, y)
#define sk_remainderl(x, y) remainderl(x, y)
#define sk_remquo(x, y)     remquo(x, y)
#define sk_remquof(x, y)    remquof(x, y)
#define sk_remquol(x, y)    remequol(x, y)
#define sk_fm(x, y, z)      fm(x, y, z)
#define sk_fmf(x, y, z)     fmf(x, y, z)
#define sk_fml(x, y, z)     fml(x, y, z)
#define sk_fmax(x, y)       fmax(x, y)
#define sk_fmaxf(x, y)      fmaxf(x, y)
#define sk_fmaxl(x, y)      fmaxl(x, y)
#define sk_fmin(x, y)       fmin(x)
#define sk_fminf(x, y)      fminf(x)
#define sk_fminl(x, y)      fminl(x)
#define sk_fdim(x, y)       fdim(x, y)
#define sk_fdimf(x, y)      fdimf(x, y)
#define sk_fdiml(x, y)      fdiml(x, y)
#define sk_nan(x)           nan(x)
#define sk_nanl(x)          nanf(x)
#define sk_nanf(x)          nanl(x)

/* Exponential functions */
#define sk_exp(x)    exp(x)
#define sk_expf(x)   expf(x)
#define sk_expl(x)   expl(x)
#define sk_exp2(x)   exp2(x)
#define sk_exp2f(x)  exp2f(x)
#define sk_exp2l(x)  exp2l(x)
#define sk_exp1(x)   expm1(x)
#define sk_exp1f(x)  expm1f(x)
#define sk_exp1l(x)  expm1l(x)
#define sk_log(x)    log(x)
#define sk_logf(x)   logf(x)
#define sk_logl(x)   logl(x)
#define sk_log10(x)  log10(x)
#define sk_log10f(x) log10f(x)
#define sk_log10l(x) log10l(x)
#define sk_log2(x)   log2(x)
#define sk_log2f(x)  log2f(x)
#define sk_log2l(x)  log2l(x)
#define sk_log1p(x)  log1p(x)
#define sk_log1pf(x) log1pf(x)
#define sk_log1pl(x) log1pl(x)

/* Power functions */
#define sk_pow(x, y)    pow(x, y)
#define sk_powf(x, y)   powf(x, y)
#define sk_powl(x, y)   powl(x, y)
#define sk_sqrt(x)      sqrt(x)
#define sk_sqrtf(x)     sqrtf(x)
#define sk_sqrtl(x)     sqrtl(x)
#define sk_cbrt(x)      cbrt(x)
#define sk_cbrtf(x)     cbrtf(x)
#define sk_cbrtl(x)     cbrtl(x)
#define sk_hypot(x, y)  hypot(x, y)
#define sk_hypotf(x, y) hypotf(x, y)
#define sk_hypotl(x, y) hypotl(x, y)

/* Trigonometric functions */
#define sk_sin(x)    sin(x)
#define sk_sinf(x)   sinf(x)
#define sk_sinl(x)   sinl(x)
#define sk_cos(x)    cos(x)
#define sk_cosf(x)   cosf(x)
#define sk_cosl(x)   cosl(x)
#define sk_tan(x)    tan(x)
#define sk_tanf(x)   tanf(x)
#define sk_tanl(x)   tanl(x)
#define sk_asin(x)   asin(x)
#define sk_asinf(x)  asinf(x)
#define sk_asinl(x)  asinl(x)
#define sk_acos(x)   acos(x)
#define sk_acosf(x)  acosf(x)
#define sk_acosl(x)  acosl(x)
#define sk_atan(x)   atan(x)
#define sk_atanf(x)  atanf(x)
#define sk_atanl(x)  atanl(x)
#define sk_atan2(x)  atan2(x)
#define sk_atan2f(x) atan2f(x)
#define sk_atan2l(x) atan2l(x)

/* Hyperbolic functions */
#define sk_sinh(x)   sinh(x)
#define sk_sinhf(x)  sinhf(x)
#define sk_sinhl(x)  sinhl(x)
#define sk_cosh(x)   cosh(x)
#define sk_coshf(x)  coshf(x)
#define sk_coshl(x)  coshl(x)
#define sk_tanh(x)   tanh(x)
#define sk_tanhf(x)  tanhf(x)
#define sk_tanhl(x)  tanhl(x)
#define sk_asinh(x)  asinh(x)
#define sk_asinhf(x) asinhf(x)
#define sk_asinhl(x) asinhl(x)
#define sk_acosh(x)  acosh(x)
#define sk_acoshf(x) acoshf(x)
#define sk_acoshl(x) acoshl(x)
#define sk_atanh(x)  atanh(x)
#define sk_atanhf(x) atanhf(x)
#define sk_atanhl(x) atanhl(x)

/* Error and gamma functions */
#define sk_erf(x)     erf(x)
#define sk_erff(x)    erff(x)
#define sk_erfl(x)    erfl(x)
#define sk_erfc(x)    erfc(x)
#define sk_erfcf(x)   erfcf(x)
#define sk_erfcl(x)   erfcl(x)
#define sk_tgamma(x)  tgamma(x)
#define sk_tgammaf(x) tgammaf(x)
#define sk_tgammal(x) tgammal(x)
#define sk_lgamma(x)  lgamma(x)
#define sk_lgammaf(x) lgammaf(x)
#define sk_lgammal(x) lgammal(x)

/* Nearest integer floating-point operations */
#define sk_ceil(x)       ceil(x)
#define sk_ceilf(x)      ceilf(x)
#define sk_ceill(x)      ceill(x)
#define sk_floor(x)      floor(x)
#define sk_floorf(x)     floorf(x)
#define sk_floorl(x)     floorl(x)
#define sk_trunc(x)      trunc(x)
#define sk_truncf(x)     truncf(x)
#define sk_truncl(x)     truncl(x)
#define sk_round(x)      round(x)
#define sk_roundf(x)     roundf(x)
#define sk_roundl(x)     roundl(x)
#define sk_lround(x)     lround(x)
#define sk_lroundf(x)    lroundf(x)
#define sk_lroundl(x)    lroundl(x)
#define sk_llround(x)    llround(x)
#define sk_llroundf(x)   llroundf(x)
#define sk_llroundl(x)   llroundl(x)
#define sk_nearbyint(x)  nearbyint(x)
#define sk_nearbyintf(x) nearbyintf(x)
#define sk_nearbyintl(x) nearbyintl(x)
#define sk_rint(x)       rint(x)
#define sk_rintf(x)      rintf(x)
#define sk_rintl(x)      rintl(x)
#define sk_lrint(x)      lrint(x)
#define sk_lrintf(x)     lrintf(x)
#define sk_lrintl(x)     lrintl(x)
#define sk_llrint(x)     llrint(x)
#define sk_llrintf(x)    llrintf(x)
#define sk_llrintl(x)    llrintl(x)

/* Classification and comparison */
#define sk_fpclassify(x)        fpclassify(x)
#define sk_isnan(x)             isnan(x)
#define sk_isinf(x)             isinf(x)
#define sk_isfinite(x)          isfinite(x)
#define sk_isnormal(x)          isnormal(x)
#define sk_signbit(x)           signbit(x)
#define sk_isgreater(x, y)      isgreater(x, y)
#define sk_isgreaterequal(x, y) isgreaterequal(x, y)
#define sk_isless(x, y)         isless(x, y)
#define sk_islessequal(x, y)    islessequal(x, y)
#define sk_islessgreater(x, y)  islessgreater(x, y)
#define sk_isunordered(x, y)    isunordered(x, y)

#endif
