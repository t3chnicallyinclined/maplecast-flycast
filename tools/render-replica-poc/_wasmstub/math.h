/* Freestanding-wasm math shim for the transpiled executor. fabs/fabsf/sqrtf lower to wasm
 * intrinsics; sin/cos are provided by wasm_tick.c (with a call counter so we can verify
 * whether the tick even reaches them). Keep minimal — extend only for symbols the executor
 * actually references. */
#ifndef _WASM_MATH_SHIM_H
#define _WASM_MATH_SHIM_H
static inline float  fabsf(float x)  { return __builtin_fabsf(x); }
static inline double fabs(double x)  { return __builtin_fabs(x); }
static inline float  sqrtf(float x)  { return __builtin_sqrtf(x); }
float sinf(float x);
float cosf(float x);
float fmaf(float x, float y, float z);
#endif
