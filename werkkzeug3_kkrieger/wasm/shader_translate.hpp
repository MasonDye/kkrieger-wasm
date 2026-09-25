// See wasm/shader_translate.cpp.
#ifndef __SHADER_TRANSLATE_HPP__
#define __SHADER_TRANSLATE_HPP__

#include "_types.hpp"

// Vertex attribute slots shared by the VAO layout and the shader binder.
enum
{
  KKATT_POS = 0, KKATT_NORMAL, KKATT_COLOR0, KKATT_UV0, KKATT_UV1,
  KKATT_TANGENT, KKATT_COLOR1, KKATT_BINORMAL, KKATT_MAX
};

// Sampler types, one per texture stage.
enum { KKSAMP_2D = 0, KKSAMP_CUBE = 1, KKSAMP_VOLUME = 2 };

struct kkShaderProgram;

kkShaderProgram *kkShaderGet(sInt setupId,const sU32 *vs,const sU32 *ps,const sU8 *samplerTypes);
void kkShaderForget(sInt setupId);
unsigned kkShaderProgramId(kkShaderProgram *p);
// alphaFunc: D3DCMP_* (8 == ALWAYS == no test), alphaRef 0..1
void kkShaderDump(kkShaderProgram *p,sInt sid,const sVector *vsc,sInt vsn,const sVector *psc,sInt psn);
void kkShaderUse(kkShaderProgram *p,const sVector *vsc,sInt vsn,const sVector *psc,sInt psn,sInt alphaFunc,sF32 alphaRef);
// emulated ZFUNC EQUAL for the program in use (kkShaderUse turns it off)
void kkShaderZEqual(kkShaderProgram *p,sInt on);
#define KK_ZSNAP_UNIT 7                 // texture unit holding the depth snapshot

#endif
