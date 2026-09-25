// D3D9 shader bytecode -> GLSL ES 3.00 programs, via MojoShader.
//
// sShaderCodeGen still emits genuine vs_1_1 / ps_1_1 token streams at runtime
// (it specialises the material11 templates per material). This file turns a
// (vertex shader, pixel shader, sampler types) triple into a linked WebGL2
// program and knows how to feed it the game's constant registers.
//
// Conventions of MojoShader's GLSL profiles that we rely on:
//   attributes  "vs_v<n>"           bound by (usage,index) to our fixed slots
//   constants   "vs_uniforms_vec4[]" packed: one vec4 per *used* register, in
//               the order of parseData->uniforms (index = register number)
//   samplers    "ps_s<n>"           texture unit n == D3D stage n
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#include "_types.hpp"
#include "wasm/shader_translate.hpp"
#include "wasm/render2004.hpp"
#include <stdio.h>
#include <string.h>
#include <GLES3/gl3.h>
#include "mojoshader.h"

/****************************************************************************/

#define KK_MAXREGS   96          // vs_1_1 has 96 float constants
#define KK_MAXSTAGES 16
#define KK_VARIANTS  4

struct kkShaderProgram
{
  sU32   Sig;                          // sampler type signature
  GLuint Prog;
  GLint  VSLoc, PSLoc;                 // vs_uniforms_vec4 / ps_uniforms_vec4
  sInt   VSCount, PSCount;             // packed vec4 count
  sU8    VSRegs[KK_MAXREGS];           // packed slot -> register
  sU8    PSRegs[KK_MAXREGS];
  sInt   SamplerCount;
  sU8    SamplerStage[KK_MAXSTAGES];   // sampler -> stage (unit)
  GLint  AlphaFuncLoc, AlphaRefLoc;    // emulated alpha test
  GLint  ZEqualLoc;                    // emulated ZFUNC EQUAL (depth snapshot on unit KK_ZSNAP_UNIT)
  GLint  SwizzleLoc;                   // debug: r/b swap
  GLint  BoostLoc;                     // debug: brightness multiplier
  GLint  ForceAlphaLoc;                // debug: force the specular alpha
  GLint  DebugViewLoc;                 // debug: light term views
  sInt   IsLight;                      // samples a cube map == per-pixel light pass
  char  *VSSrc, *PSSrc;                // debug: translated sources
  sBool  Failed;
};

struct kkSetupCache
{
  sInt Count;
  kkShaderProgram Variant[KK_VARIANTS];
};

static kkSetupCache *gCache;          // indexed by setup id
static sInt gCacheSize;
sInt kkSwizzleOutput = 0;                         // debug (C): swap r/b on every shader output
sF32 kkLightBoost = 1.0f;                         // debug (V): scale the lighting passes
sF32 kkForceLightAlpha = 0.0f;                    // debug (B): force the specular alpha the lights write
sInt kkLightDebugView = 0;                        // debug (N): show one term of the light shader

static sInt gLoggedErrors;

/****************************************************************************/

static sInt TokenCount(const sU32 *t)
{
  sInt n = 0;
  if(!t) return 0;
  while(t[n] != 0x0000ffff && n < 65536) n++;
  return n+1;
}

// (usage,index) -> vertex attribute slot; must agree with the VAO layout in
// _start_wasm.cpp (KKATT_*).
static sInt AttrSlot(MOJOSHADER_usage usage,sInt index)
{
  switch(usage)
  {
  case MOJOSHADER_USAGE_POSITION: return KKATT_POS;
  case MOJOSHADER_USAGE_NORMAL:   return KKATT_NORMAL;
  case MOJOSHADER_USAGE_COLOR:    return index==0 ? KKATT_COLOR0 : KKATT_COLOR1;
  case MOJOSHADER_USAGE_TEXCOORD: return index==0 ? KKATT_UV0 : KKATT_UV1;
  case MOJOSHADER_USAGE_TANGENT:  return KKATT_TANGENT;
  case MOJOSHADER_USAGE_BINORMAL: return KKATT_BINORMAL;
  default:                        return -1;
  }
}

// Fragment shader fix-ups:
//  * GLSL ES 3.00 has no default precision for sampler3D (the attenuation
//    volume); splice one in right after the #version line.
//  * GL ES has no alpha test; emulate D3D's with a discard at the end of
//    main(), driven by two uniforms.
//  * ZFUNC EQUAL is emulated (see kkZEqual in _start_wasm.cpp): the hardware
//    test is LEQUAL, and fragments clearly in front of the depth the z-fill
//    passes left behind (alpha-tested holes, where that depth belongs to
//    whatever is behind) are dropped here.
static const char *kAlphaTestTail =
  "\n  if(kkZEqual != 0 && gl_FragCoord.z < texelFetch(kkZSnap, ivec2(gl_FragCoord.xy), 0).r - 0.00002)\n"
  "  { if(kkZEqual == 1) discard; _gl_FragData_0 = vec4(1.0, 0.0, 1.0, 0.0); }   // 2: debug, show instead\n"
  "\n  { float kk_a = _gl_FragData_0.a; bool kk_pass =\n"
  "      (kkAlphaFunc == 8) || (kkAlphaFunc == 5 && kk_a > kkAlphaRef) || (kkAlphaFunc == 7 && kk_a >= kkAlphaRef) ||\n"
  "      (kkAlphaFunc == 2 && kk_a < kkAlphaRef) || (kkAlphaFunc == 4 && kk_a <= kkAlphaRef) ||\n"
  "      (kkAlphaFunc == 3 && abs(kk_a - kkAlphaRef) < 0.002) || (kkAlphaFunc == 6 && abs(kk_a - kkAlphaRef) >= 0.002);\n"
  "    if(!kk_pass) discard; }\n"
  "  if(kkSwizzle == 1) _gl_FragData_0 = _gl_FragData_0.bgra;   // debug (C)\n"
  "  _gl_FragData_0.rgb *= kkBoost;                             // debug (V)\n"
  "  if(kkForceAlpha > 0.0) _gl_FragData_0.a = kkForceAlpha;    // debug (B)\n";

// Debug views for the per-pixel lighting shaders (key N cycles):
//  1 attenuation volume   2 light-vector cube   3 normal map (biased)
//  4 diffuse dot          5 attenuation texcoord 6 light colour constant
static const char *kLightDebugTail =
  "\n  if(kkDebugView > 0) {\n"
  "    vec4 kn = texture(ps_s0, io_5_0.xy);\n"
  "    vec4 kl = texture(ps_s1, io_5_1.xyz);\n"
  "    vec4 ka = texture(ps_s3, io_5_3.xyz);\n"
  "    float kd = clamp(dot(kl.xyz*2.0-1.0, kn.xyz), 0.0, 1.0);\n"
  "    if(kkDebugView == 1) _gl_FragData_0 = vec4(ka.xyz, 0.0);\n"
  "    if(kkDebugView == 2) _gl_FragData_0 = vec4(kl.xyz, 0.0);\n"
  "    if(kkDebugView == 3) _gl_FragData_0 = vec4(kn.xyz*0.5+0.5, 0.0);\n"
  "    if(kkDebugView == 4) _gl_FragData_0 = vec4(vec3(kd), 0.0);\n"
  "    if(kkDebugView == 5) _gl_FragData_0 = vec4(fract(io_5_3.xyz), 0.0);\n"
  "    if(kkDebugView == 6) _gl_FragData_0 = vec4(ps_uniforms_vec4[0].xyz, 0.0);\n"
  "  }\n";

static char *WithSamplerPrecision(const char *src)
{
  const char *nl = strchr(src,'\n');
  if(!nl) nl = src;
  const char *extra = "\nprecision mediump sampler3D;\nuniform highp sampler2D kkZSnap;\nuniform mediump int kkZEqual;\nuniform mediump int kkAlphaFunc;\nuniform mediump float kkAlphaRef;\nuniform mediump int kkSwizzle;\nuniform mediump float kkBoost;\nuniform mediump float kkForceAlpha;\nuniform mediump int kkDebugView;\n";
  size_t head = nl - src;
  const char *lastBrace = strrchr(src,'}');
  size_t bodyLen = lastBrace ? (size_t)(lastBrace - nl) : strlen(nl);
  sBool lightShader = strstr(src,"samplerCube ps_s1") && strstr(src,"sampler3D ps_s3") && strstr(src,"sampler2D ps_s0")
                      && strstr(src,"ps_uniforms_vec4");
  char *out = new char[strlen(src) + strlen(extra) + strlen(kAlphaTestTail) + strlen(kLightDebugTail) + 80];
  memcpy(out,src,head);
  strcpy(out+head,extra);
  strncat(out,nl,bodyLen);                // everything up to the final '}'
  if(lastBrace)
  {
    strcat(out,kAlphaTestTail);
    if(lightShader) strcat(out,kLightDebugTail);
    strcat(out,lastBrace);
  }
  // D3D9 ran pixel shaders at fp24/fp32. GLSL ES mediump may really be fp16
  // (ANGLE on NVIDIA's GLES driver reports a 10 bit mantissa), which cuts a
  // tiled texture coordinate of 8.0 down to steps of 1/256 and makes lit
  // surfaces shimmer. WebGL 2 guarantees highp in fragment shaders.
  char *prec = strstr(out,"precision mediump float;");
  if(prec) memcpy(prec,"precision highp   float;",24);
  return out;
}

// Vertex shader fix-up: the engine renders the same geometry several times
// (z-fill, then base and light passes with ZFUNC=EQUAL). D3D guarantees that
// identical vertex programs produce identical positions; in GL that needs
// `invariant gl_Position`, otherwise the depth comparison misses and whole
// surfaces stay unshaded.
static char *WithInvariantPosition(const char *src)
{
  const char *nl = strchr(src,'\n');
  if(!nl) nl = src;
  const char *extra = "\ninvariant gl_Position;\n";
  size_t head = nl - src;
  char *out = new char[strlen(src) + strlen(extra) + 8];
  memcpy(out,src,head);
  strcpy(out+head,extra);
  strcat(out,nl);
  return out;
}

static GLuint CompileStage(GLenum type,const char *src0,const char *what,sInt sid)
{
  char *fixed = (type == GL_FRAGMENT_SHADER) ? WithSamplerPrecision(src0)
                                             : WithInvariantPosition(src0);
  const char *src = fixed ? fixed : src0;
  GLuint s = glCreateShader(type);
  glShaderSource(s,1,&src,0);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok)
  {
    char log[2048]; GLsizei n = 0;
    glGetShaderInfoLog(s,sizeof(log),&n,log);
    if(gLoggedErrors++ < 8)
      fprintf(stderr,"[kk] setup %d %s GLSL compile failed: %.*s\n%s\n",sid,what,(int)n,log,src);
    glDeleteShader(s);
    delete[] fixed;
    return 0;
  }
  delete[] fixed;
  return s;
}

// Copies the packing order out of a parse result.
static sInt PackRegs(const MOJOSHADER_parseData *pd,sU8 *regs)
{
  sInt n = 0;
  for(sInt i=0;i<pd->uniform_count;i++)
  {
    const MOJOSHADER_uniform &u = pd->uniforms[i];
    if(u.type != MOJOSHADER_UNIFORM_FLOAT) continue;
    sInt count = u.array_count ? u.array_count : 1;
    for(sInt k=0;k<count && n<KK_MAXREGS;k++)
      regs[n++] = (sU8)(u.index + k);
  }
  return n;
}

static sBool Build(kkShaderProgram *p,sInt sid,const sU32 *vs,const sU32 *ps,const sU8 *samplerTypes)
{
  MOJOSHADER_samplerMap smap[KK_MAXSTAGES];
  sInt smapCount = 0;
  for(sInt i=0;i<KK_MAXSTAGES;i++)
  {
    if(samplerTypes[i] == KKSAMP_2D) continue;
    smap[smapCount].index = i;
    smap[smapCount].type = samplerTypes[i]==KKSAMP_CUBE ? MOJOSHADER_SAMPLER_CUBE : MOJOSHADER_SAMPLER_VOLUME;
    smapCount++;
  }

  const MOJOSHADER_parseData *pv = MOJOSHADER_parse("glsles3",0,(const unsigned char *)vs,TokenCount(vs)*4,0,0,smap,smapCount,0,0,0);
  const MOJOSHADER_parseData *pp = MOJOSHADER_parse("glsles3",0,(const unsigned char *)ps,TokenCount(ps)*4,0,0,smap,smapCount,0,0,0);

  sBool ok = sTRUE;
  if(pv->error_count || pp->error_count)
  {
    if(gLoggedErrors++ < 8)
    {
      fprintf(stderr,"[kk] setup %d: shader translation failed\n",sid);
      for(sInt i=0;i<pv->error_count;i++) fprintf(stderr,"[kk]   VS @%d: %s\n",pv->errors[i].error_position,pv->errors[i].error);
      for(sInt i=0;i<pp->error_count;i++) fprintf(stderr,"[kk]   PS @%d: %s\n",pp->errors[i].error_position,pp->errors[i].error);
    }
    ok = sFALSE;
  }

  GLuint prog = 0;
  if(ok)
  {
    GLuint v = CompileStage(GL_VERTEX_SHADER,pv->output,"VS",sid);
    GLuint f = CompileStage(GL_FRAGMENT_SHADER,pp->output,"PS",sid);
    if(v && f)
    {
      prog = glCreateProgram();
      glAttachShader(prog,v);
      glAttachShader(prog,f);
      {
        static sInt logged;
        if(pp->output && strstr(pp->output,"samplerCube") && logged++ < 2)   // a per-pixel light pass
        {
          fprintf(stderr,"[kk] light setup %d attributes: %d\n",sid,pv->attribute_count);
          for(sInt i=0;i<pv->attribute_count;i++)
            fprintf(stderr,"[kk]   attr %s usage=%d index=%d -> slot %d\n",
                    pv->attributes[i].name,(int)pv->attributes[i].usage,pv->attributes[i].index,
                    AttrSlot(pv->attributes[i].usage,pv->attributes[i].index));
        }
      }
      for(sInt i=0;i<pv->attribute_count;i++)
      {
        sInt slot = AttrSlot(pv->attributes[i].usage,pv->attributes[i].index);
        if(slot >= 0)
          glBindAttribLocation(prog,slot,pv->attributes[i].name);
        else if(gLoggedErrors++ < 8)
          fprintf(stderr,"[kk] setup %d: unmapped attribute usage %d idx %d\n",sid,pv->attributes[i].usage,pv->attributes[i].index);
      }
      glLinkProgram(prog);
      GLint linked = 0;
      glGetProgramiv(prog,GL_LINK_STATUS,&linked);
      if(!linked)
      {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(prog,sizeof(log),&n,log);
        if(gLoggedErrors++ < 8)
          fprintf(stderr,"[kk] setup %d: link failed: %.*s\n",sid,(int)n,log);
        glDeleteProgram(prog);
        prog = 0;
      }
    }
    if(v) glDeleteShader(v);
    if(f) glDeleteShader(f);
    if(!prog) ok = sFALSE;
  }

  if(ok)
  {
    p->Prog = prog;
    p->VSLoc = glGetUniformLocation(prog,"vs_uniforms_vec4");
    p->PSLoc = glGetUniformLocation(prog,"ps_uniforms_vec4");
    p->VSCount = PackRegs(pv,p->VSRegs);
    p->PSCount = PackRegs(pp,p->PSRegs);
    p->AlphaFuncLoc = glGetUniformLocation(prog,"kkAlphaFunc");
    p->AlphaRefLoc = glGetUniformLocation(prog,"kkAlphaRef");
    p->ZEqualLoc = glGetUniformLocation(prog,"kkZEqual");
    p->SwizzleLoc = glGetUniformLocation(prog,"kkSwizzle");
    p->BoostLoc = glGetUniformLocation(prog,"kkBoost");
    p->ForceAlphaLoc = glGetUniformLocation(prog,"kkForceAlpha");
    p->DebugViewLoc = glGetUniformLocation(prog,"kkDebugView");
    p->IsLight = pp->output && strstr(pp->output,"samplerCube") ? 1 : 0;

    glUseProgram(prog);
    { GLint zl = glGetUniformLocation(prog,"kkZSnap"); if(zl >= 0) glUniform1i(zl,KK_ZSNAP_UNIT); }
    p->SamplerCount = 0;
    for(sInt i=0;i<pp->sampler_count && p->SamplerCount<KK_MAXSTAGES;i++)
    {
      GLint loc = glGetUniformLocation(prog,pp->samplers[i].name);
      if(loc >= 0)
      {
        glUniform1i(loc,pp->samplers[i].index);          // unit == stage
        p->SamplerStage[p->SamplerCount++] = (sU8)pp->samplers[i].index;
      }
    }
  }

  p->VSSrc = pv->output ? strdup(pv->output) : 0;
  p->PSSrc = pp->output ? strdup(pp->output) : 0;
  MOJOSHADER_freeParseData(pv);
  MOJOSHADER_freeParseData(pp);
  p->Failed = !ok;
  return ok;
}

/****************************************************************************/

// Hand-written GLSL programs: a setup whose vertex "token stream" is
// { KK04_MARKER | variant, end } is one of the Breakpoint 2004 lighting
// shaders from wasm/render2004.cpp. Constants are unpacked (register n is
// array element n), samplers are ps_s<n> on unit n, and the attributes have
// fixed names.
static sBool BuildGLSL(kkShaderProgram *p,sInt sid,sInt variant)
{
  static const struct { const char *Name; sInt Slot; } attrs[] =
  {
    { "a_pos",KKATT_POS },{ "a_nrm",KKATT_NORMAL },{ "a_tan",KKATT_TANGENT },{ "a_uv",KKATT_UV0 },
  };
  const char *vsrc = kk04ShaderSource(variant,0);
  const char *fsrc = kk04ShaderSource(variant,1);
  GLuint sh[2] = { 0,0 };
  for(sInt k=0;k<2;k++)
  {
    sh[k] = glCreateShader(k ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    const char *src = k ? fsrc : vsrc;
    glShaderSource(sh[k],1,&src,0);
    glCompileShader(sh[k]);
    GLint ok = 0;
    glGetShaderiv(sh[k],GL_COMPILE_STATUS,&ok);
    if(!ok)
    {
      char log[2048]; GLsizei n = 0;
      glGetShaderInfoLog(sh[k],sizeof(log),&n,log);
      fprintf(stderr,"[kk] 2004 shader %x %s compile failed: %.*s\n%s\n",variant,k?"PS":"VS",(int)n,log,src);
    }
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog,sh[0]);
  glAttachShader(prog,sh[1]);
  for(sInt i=0;i<(sInt)(sizeof(attrs)/sizeof(attrs[0]));i++)
    glBindAttribLocation(prog,attrs[i].Slot,attrs[i].Name);
  glLinkProgram(prog);
  glDeleteShader(sh[0]);
  glDeleteShader(sh[1]);
  GLint linked = 0;
  glGetProgramiv(prog,GL_LINK_STATUS,&linked);
  if(!linked)
  {
    char log[2048]; GLsizei n = 0;
    glGetProgramInfoLog(prog,sizeof(log),&n,log);
    fprintf(stderr,"[kk] 2004 shader %x (setup %d): link failed: %.*s\n",variant,sid,(int)n,log);
    glDeleteProgram(prog);
    p->Failed = sTRUE;
    return sFALSE;
  }

  p->Prog = prog;
  p->VSLoc = glGetUniformLocation(prog,"vs_uniforms_vec4");
  p->PSLoc = glGetUniformLocation(prog,"ps_uniforms_vec4");
  p->VSCount = KK04_VSREGS;
  p->PSCount = KK04_PSREGS;
  for(sInt i=0;i<KK04_VSREGS;i++) p->VSRegs[i] = (sU8)i;
  for(sInt i=0;i<KK04_PSREGS;i++) p->PSRegs[i] = (sU8)i;
  p->AlphaFuncLoc = p->AlphaRefLoc = p->SwizzleLoc = p->ForceAlphaLoc = p->DebugViewLoc = -1;
  p->ZEqualLoc = glGetUniformLocation(prog,"kkZEqual");
  p->BoostLoc = glGetUniformLocation(prog,"kkBoost");
  p->IsLight = 1;

  glUseProgram(prog);
  { GLint zl = glGetUniformLocation(prog,"kkZSnap"); if(zl >= 0) glUniform1i(zl,KK_ZSNAP_UNIT); }
  p->SamplerCount = 0;
  for(sInt i=0;i<4;i++)
  {
    char name[16];
    sprintf(name,"ps_s%d",i);
    GLint loc = glGetUniformLocation(prog,name);
    if(loc >= 0)
    {
      glUniform1i(loc,i);
      p->SamplerStage[p->SamplerCount++] = (sU8)i;
    }
  }
  p->VSSrc = strdup(vsrc);
  p->PSSrc = strdup(fsrc);
  p->Failed = sFALSE;
  return sTRUE;
}

/****************************************************************************/

kkShaderProgram *kkShaderGet(sInt sid,const sU32 *vs,const sU32 *ps,const sU8 *samplerTypes)
{
  if(!vs || !ps) return 0;

  if(sid >= gCacheSize)
  {
    sInt ns = sMax(sid+1,gCacheSize*2);
    if(ns < 64) ns = 64;
    kkSetupCache *nc = new kkSetupCache[ns];
    memset(nc,0,sizeof(kkSetupCache)*ns);
    if(gCache) memcpy(nc,gCache,sizeof(kkSetupCache)*gCacheSize);
    delete[] gCache;
    gCache = nc;
    gCacheSize = ns;
  }

  sU32 sig = 0;
  for(sInt i=0;i<KK_MAXSTAGES;i++)
    sig |= (sU32)(samplerTypes[i] & 3) << (2*i);

  kkSetupCache *c = &gCache[sid];
  for(sInt i=0;i<c->Count;i++)
    if(c->Variant[i].Sig == sig)
      return c->Variant[i].Failed ? 0 : &c->Variant[i];

  if(c->Count >= KK_VARIANTS)
    return 0;
  kkShaderProgram *p = &c->Variant[c->Count++];
  memset(p,0,sizeof(*p));
  p->Sig = sig;
  if((vs[0] & 0xffff0000) == KK04_MARKER)
    BuildGLSL(p,sid,vs[0] & 0xffff);
  else
    Build(p,sid,vs,ps,samplerTypes);
  return p->Failed ? 0 : p;
}

void kkShaderForget(sInt sid)
{
  if(sid < 0 || sid >= gCacheSize) return;
  kkSetupCache *c = &gCache[sid];
  for(sInt i=0;i<c->Count;i++)
    if(c->Variant[i].Prog) glDeleteProgram(c->Variant[i].Prog);
  memset(c,0,sizeof(*c));
}

void kkShaderZEqual(kkShaderProgram *p,sInt on)
{
  if(p && p->ZEqualLoc >= 0) glUniform1i(p->ZEqualLoc,on);
}

GLuint kkShaderProgramId(kkShaderProgram *p)
{
  return p ? p->Prog : 0;
}

void kkShaderUse(kkShaderProgram *p,const sVector *vsc,sInt vsn,const sVector *psc,sInt psn,sInt alphaFunc,sF32 alphaRef)
{
  static float tmp[KK_MAXREGS*4];
  glUseProgram(p->Prog);
  if(p->AlphaFuncLoc >= 0) glUniform1i(p->AlphaFuncLoc,alphaFunc);
  if(p->ZEqualLoc >= 0)    glUniform1i(p->ZEqualLoc,0);
  if(p->SwizzleLoc >= 0)   glUniform1i(p->SwizzleLoc,kkSwizzleOutput);
  if(p->BoostLoc >= 0)     glUniform1f(p->BoostLoc,p->IsLight ? kkLightBoost : 1.0f);
  if(p->ForceAlphaLoc >= 0) glUniform1f(p->ForceAlphaLoc,p->IsLight ? kkForceLightAlpha : 0.0f);
  if(p->DebugViewLoc >= 0)  glUniform1i(p->DebugViewLoc,kkLightDebugView);
  if(p->AlphaRefLoc >= 0)  glUniform1f(p->AlphaRefLoc,alphaRef);

  if(p->VSLoc >= 0 && p->VSCount)
  {
    for(sInt i=0;i<p->VSCount;i++)
    {
      sInt r = p->VSRegs[i];
      if(r < vsn) memcpy(tmp+i*4,&vsc[r],16); else memset(tmp+i*4,0,16);
    }
    glUniform4fv(p->VSLoc,p->VSCount,tmp);
  }
  if(p->PSLoc >= 0 && p->PSCount)
  {
    for(sInt i=0;i<p->PSCount;i++)
    {
      sInt r = p->PSRegs[i];
      if(r < psn) memcpy(tmp+i*4,&psc[r],16); else memset(tmp+i*4,0,16);
    }
    glUniform4fv(p->PSLoc,p->PSCount,tmp);
  }
}

// debug: print a program's GLSL (without the long #define preamble) and the
// constants it is about to get
void kkShaderDump(kkShaderProgram *p,sInt sid,const sVector *vsc,sInt vsn,const sVector *psc,sInt psn)
{
  if(!p) { fprintf(stderr,"[kk] dump setup %d: no program\n",sid); return; }
  for(sInt k=0;k<2;k++)
  {
    const char *src = k ? p->PSSrc : p->VSSrc;
    fprintf(stderr,"[kk] ===== setup %d %s =====\n",sid,k?"PS":"VS");
    const char *l = src;
    while(l && *l)
    {
      const char *e = strchr(l,'\n');
      sInt n = e ? (sInt)(e-l) : (sInt)strlen(l);
      if(n && strncmp(l,"#define texture",15) && strncmp(l,"#define ps_c",12) && strncmp(l,"#define vs_c",12))
        fprintf(stderr,"[kk] | %.*s\n",(int)n,l);
      l = e ? e+1 : 0;
    }
  }
  for(sInt i=0;i<p->VSCount;i++)
  {
    sInt r = p->VSRegs[i];
    if(r < vsn) fprintf(stderr,"[kk] vs c%d = %.3f %.3f %.3f %.3f\n",r,vsc[r].x,vsc[r].y,vsc[r].z,vsc[r].w);
  }
  for(sInt i=0;i<p->PSCount;i++)
  {
    sInt r = p->PSRegs[i];
    if(r < psn) fprintf(stderr,"[kk] ps c%d = %.3f %.3f %.3f %.3f\n",r,psc[r].x,psc[r].y,psc[r].z,psc[r].w);
    else fprintf(stderr,"[kk] ps c%d = (not set, n=%d)\n",r,psn);
  }
  fprintf(stderr,"[kk] ===== end =====\n");
}
