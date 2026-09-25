// .kkrieger WebAssembly port -- scalar emulation of the MMX subset that the
// procedural texture generator (genbitmap.cpp) relies on.
//
// Semantics are bit-exact with the Intel definitions, which matters: the whole
// point of .kkrieger is that the textures are *computed*, so any deviation
// shows up as a visibly different image rather than as a crash.
//
// Lane layout of __m64 mirrors the hardware: u16[0] is the least significant
// word, i.e. the one at the lowest address in little-endian memory.
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#ifndef __MMX_SCALAR_HPP__
#define __MMX_SCALAR_HPP__

#include <stdint.h>

union __m64
{
  uint64_t q;
  int64_t  sq;
  uint32_t u32[2];
  int32_t  s32[2];
  uint16_t u16[4];
  int16_t  s16[4];
  uint8_t  u8[8];
  int8_t   s8[8];
};

/****************************************************************************/
/***   saturation helpers                                                 ***/
/****************************************************************************/

static inline int16_t  mmxSatS16(int32_t v) { return (int16_t)(v >  32767 ?  32767 : (v < -32768 ? -32768 : v)); }
static inline uint16_t mmxSatU16(int32_t v) { return (uint16_t)(v > 65535 ?  65535 : (v <      0 ?      0 : v)); }

#define MMX_W4(expr)  { __m64 r__; for(int i__=0;i__<4;i__++) { expr; } return r__; }
#define MMX_D2(expr)  { __m64 r__; for(int i__=0;i__<2;i__++) { expr; } return r__; }

/****************************************************************************/
/***   set / misc                                                         ***/
/****************************************************************************/

static inline __m64 _mm_setzero_si64()                       { __m64 r; r.q = 0; return r; }
static inline __m64 _mm_set_pi32(int e1,int e0)              { __m64 r; r.s32[0]=e0; r.s32[1]=e1; return r; }
static inline __m64 _mm_set_pi16(short w3,short w2,short w1,short w0)
                                                             { __m64 r; r.s16[0]=w0; r.s16[1]=w1; r.s16[2]=w2; r.s16[3]=w3; return r; }
static inline __m64 _mm_set1_pi16(short w)                   { return _mm_set_pi16(w,w,w,w); }
static inline int   _mm_cvtsi64_si32(__m64 a)                { return a.s32[0]; }
static inline __m64 _mm_cvtsi32_si64(int a)                  { __m64 r; r.q=0; r.s32[0]=a; return r; }
static inline void  _mm_empty()                              { }   // no FPU state to unwind

/****************************************************************************/
/***   logical (whole 64 bit)                                             ***/
/****************************************************************************/

static inline __m64 _mm_and_si64 (__m64 a,__m64 b)           { __m64 r; r.q = a.q & b.q;  return r; }
static inline __m64 _mm_andnot_si64(__m64 a,__m64 b)         { __m64 r; r.q = (~a.q) & b.q; return r; }
static inline __m64 _mm_or_si64  (__m64 a,__m64 b)           { __m64 r; r.q = a.q | b.q;  return r; }
static inline __m64 _mm_xor_si64 (__m64 a,__m64 b)           { __m64 r; r.q = a.q ^ b.q;  return r; }
static inline __m64 _mm_slli_si64(__m64 a,int n)             { __m64 r; r.q = (n>63) ? 0 : (a.q << n); return r; }
static inline __m64 _mm_srli_si64(__m64 a,int n)             { __m64 r; r.q = (n>63) ? 0 : (a.q >> n); return r; }

/****************************************************************************/
/***   16 bit lanes                                                       ***/
/****************************************************************************/

static inline __m64 _mm_add_pi16 (__m64 a,__m64 b)  MMX_W4( r__.u16[i__] = (uint16_t)(a.u16[i__] + b.u16[i__]) )
static inline __m64 _mm_sub_pi16 (__m64 a,__m64 b)  MMX_W4( r__.u16[i__] = (uint16_t)(a.u16[i__] - b.u16[i__]) )
static inline __m64 _mm_adds_pi16(__m64 a,__m64 b)  MMX_W4( r__.s16[i__] = mmxSatS16((int32_t)a.s16[i__] + b.s16[i__]) )
static inline __m64 _mm_subs_pi16(__m64 a,__m64 b)  MMX_W4( r__.s16[i__] = mmxSatS16((int32_t)a.s16[i__] - b.s16[i__]) )
static inline __m64 _mm_adds_pu16(__m64 a,__m64 b)  MMX_W4( r__.u16[i__] = mmxSatU16((int32_t)a.u16[i__] + b.u16[i__]) )
static inline __m64 _mm_subs_pu16(__m64 a,__m64 b)  MMX_W4( r__.u16[i__] = mmxSatU16((int32_t)a.u16[i__] - b.u16[i__]) )

static inline __m64 _mm_mullo_pi16(__m64 a,__m64 b) MMX_W4( r__.u16[i__] = (uint16_t)(((int32_t)a.s16[i__] * (int32_t)b.s16[i__]) & 0xffff) )
static inline __m64 _mm_mulhi_pi16(__m64 a,__m64 b) MMX_W4( r__.s16[i__] = (int16_t)(((int32_t)a.s16[i__] * (int32_t)b.s16[i__]) >> 16) )
static inline __m64 _mm_mulhi_pu16(__m64 a,__m64 b) MMX_W4( r__.u16[i__] = (uint16_t)(((uint32_t)a.u16[i__] * (uint32_t)b.u16[i__]) >> 16) )

static inline __m64 _mm_slli_pi16(__m64 a,int n)    MMX_W4( r__.u16[i__] = (n>15) ? 0 : (uint16_t)(a.u16[i__] << n) )
static inline __m64 _mm_srli_pi16(__m64 a,int n)    MMX_W4( r__.u16[i__] = (n>15) ? 0 : (uint16_t)(a.u16[i__] >> n) )
static inline __m64 _mm_srai_pi16(__m64 a,int n)    MMX_W4( r__.s16[i__] = (int16_t)(a.s16[i__] >> ((n>15)?15:n)) )

static inline __m64 _mm_cmpgt_pi16(__m64 a,__m64 b) MMX_W4( r__.u16[i__] = (a.s16[i__] > b.s16[i__]) ? 0xffff : 0 )
static inline __m64 _mm_cmpeq_pi16(__m64 a,__m64 b) MMX_W4( r__.u16[i__] = (a.s16[i__] == b.s16[i__]) ? 0xffff : 0 )
static inline __m64 _mm_max_pi16 (__m64 a,__m64 b)  MMX_W4( r__.s16[i__] = a.s16[i__] > b.s16[i__] ? a.s16[i__] : b.s16[i__] )
static inline __m64 _mm_min_pi16 (__m64 a,__m64 b)  MMX_W4( r__.s16[i__] = a.s16[i__] < b.s16[i__] ? a.s16[i__] : b.s16[i__] )

/****************************************************************************/
/***   32 bit lanes                                                       ***/
/****************************************************************************/

static inline __m64 _mm_add_pi32 (__m64 a,__m64 b)  MMX_D2( r__.u32[i__] = a.u32[i__] + b.u32[i__] )
static inline __m64 _mm_sub_pi32 (__m64 a,__m64 b)  MMX_D2( r__.u32[i__] = a.u32[i__] - b.u32[i__] )
static inline __m64 _mm_slli_pi32(__m64 a,int n)    MMX_D2( r__.u32[i__] = (n>31) ? 0 : (a.u32[i__] << n) )
static inline __m64 _mm_srli_pi32(__m64 a,int n)    MMX_D2( r__.u32[i__] = (n>31) ? 0 : (a.u32[i__] >> n) )
static inline __m64 _mm_srai_pi32(__m64 a,int n)    MMX_D2( r__.s32[i__] = a.s32[i__] >> ((n>31)?31:n) )
static inline __m64 _mm_cmpgt_pi32(__m64 a,__m64 b) MMX_D2( r__.u32[i__] = (a.s32[i__] > b.s32[i__]) ? 0xffffffffu : 0 )

// 4x s16 * 4x s16 -> 2x s32, pairwise summed
static inline __m64 _mm_madd_pi16(__m64 a,__m64 b)
{
  __m64 r;
  r.s32[0] = (int32_t)a.s16[0]*b.s16[0] + (int32_t)a.s16[1]*b.s16[1];
  r.s32[1] = (int32_t)a.s16[2]*b.s16[2] + (int32_t)a.s16[3]*b.s16[3];
  return r;
}

/****************************************************************************/
/***   pack / unpack                                                      ***/
/****************************************************************************/

static inline __m64 _mm_packs_pi32(__m64 a,__m64 b)
{
  __m64 r;
  r.s16[0] = mmxSatS16(a.s32[0]);  r.s16[1] = mmxSatS16(a.s32[1]);
  r.s16[2] = mmxSatS16(b.s32[0]);  r.s16[3] = mmxSatS16(b.s32[1]);
  return r;
}

static inline __m64 _mm_packs_pi16(__m64 a,__m64 b)
{
  __m64 r;
  for(int i=0;i<4;i++)
  {
    int32_t va = a.s16[i], vb = b.s16[i];
    r.s8[i]   = (int8_t)(va >  127 ?  127 : (va < -128 ? -128 : va));
    r.s8[i+4] = (int8_t)(vb >  127 ?  127 : (vb < -128 ? -128 : vb));
  }
  return r;
}

static inline __m64 _mm_unpacklo_pi16(__m64 a,__m64 b)
{
  __m64 r;
  r.u16[0]=a.u16[0]; r.u16[1]=b.u16[0]; r.u16[2]=a.u16[1]; r.u16[3]=b.u16[1];
  return r;
}

static inline __m64 _mm_unpackhi_pi16(__m64 a,__m64 b)
{
  __m64 r;
  r.u16[0]=a.u16[2]; r.u16[1]=b.u16[2]; r.u16[2]=a.u16[3]; r.u16[3]=b.u16[3];
  return r;
}

static inline __m64 _mm_unpacklo_pi32(__m64 a,__m64 b)
{
  __m64 r; r.u32[0]=a.u32[0]; r.u32[1]=b.u32[0]; return r;
}

static inline __m64 _mm_unpackhi_pi32(__m64 a,__m64 b)
{
  __m64 r; r.u32[0]=a.u32[1]; r.u32[1]=b.u32[1]; return r;
}

#undef MMX_W4
#undef MMX_D2

#endif // __MMX_SCALAR_HPP__
