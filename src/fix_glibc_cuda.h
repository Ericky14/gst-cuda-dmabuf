/**
 * Workaround for glibc 2.41+ vs CUDA 12.9 noexcept specification conflict
 * 
 * glibc 2.41 declares sinpi/cospi with noexcept(true) but CUDA declares without.
 * This header redefines the glibc macro to prevent the glibc declarations.
 */

#ifndef FIX_GLIBC_CUDA_H
#define FIX_GLIBC_CUDA_H

/* Prevent glibc from declaring sinpi/cospi/sinpif/cospif */
/* These are provided by CUDA's math_functions.h instead */
#define __GLIBC_USE_IEC_60559_FUNCS_EXT 0
#define __GLIBC_USE_IEC_60559_FUNCS_EXT_C23 0

#endif /* FIX_GLIBC_CUDA_H */
