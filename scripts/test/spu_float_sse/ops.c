#ifndef VARIANT_V
#define SPU_FLOAT_SCALAR
#endif
#include "spu_helpers.h"
#ifdef VARIANT_V
#define PFX(n) v_##n
#else
#define PFX(n) s_##n
#endif
#define W(n) u128 PFX(n)(u128 a, u128 b, u128 c) { (void)c; return spu_##n(a, b); }
#define W3(n) u128 PFX(n)(u128 a, u128 b, u128 c) { return spu_##n(a, b, c); }
W(fa) W(fs) W(fm) W(fceq) W(fcgt) W3(fma) W3(fms) W3(fnms)
