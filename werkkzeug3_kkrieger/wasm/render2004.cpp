// The lighting of the Breakpoint 2004 .kkrieger beta.
//
// The released beta does not light like the werkkzeug3 sources this port is
// built from (one additive pass per light, 1-d^2/r^2 attenuation volume,
// per-light stencil shadows). Reconstructed from the beta executable
// (Engine paint at VA 0x817134, material setup at 0x804b25) and an apitrace
// of it running under Wine:
//
//  * a frame is lit by at most four lights: importance = range / distance to
//    the camera (0 beyond 45 units, amplify faded between 35 and 45), the
//    newest weapon light always first; up to three shadow casters are moved
//    to the front
//  * shadows go to a half-size mask render target: cleared (per shadow count)
//    to 0xffffffff / 0x00ffffff / 0x00ffff00 / 0x00ff0000, z-filled with the
//    base passes (which write their colour, alpha 0), then per shadow light
//    the volumes stamp the stencil and a quad adds the light's channel
//    (a, b, g) where the stencil is 0
//  * every lit mesh gets ONE additive pass for all four lights: per-vertex
//    linear attenuation amplify*(1-d/range), the lights' directions blended
//    by (0.5+0.5*N.L)*attenuation*max(r,g,b) into a single tangent-space
//    vector for the normal map, colour = sum(colour_i*attenuation_i*mask_i)
//    (light 3 unmasked), times saturate(N.L)
//  * specular is a second additive pass after the texture pass with the same
//    blending on half vectors, (N.H)^8/16/32 by specular power < 8.5 / 16.5
//
// The shaders below follow the traced vs_1_1 / ps_1_3 code instruction by
// instruction; only the vertex inputs differ (the port keeps float normals
// and tangents, 2004 packed shorts).
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#include "_types.hpp"
#include "_start.hpp"
#include "_startdx.hpp"
#include "materials/material11.hpp"
#include "genoverlay.hpp"
#include "wasm/render2004.hpp"
#include <stdio.h>
#include <string.h>

/****************************************************************************/
/***                                                                      ***/
/***   Shaders                                                            ***/
/***                                                                      ***/
/****************************************************************************/

// variant bits
#define V_NRM       0x01                // normal map (else the constant normal)
#define V_MODEMASK  0x06                // 0 diffuse, 1..3 specular ^8 ^16 ^32
#define V_SPECMAP   0x08                // normal map alpha scales the specular

// vs constants
//   c0-c3   world-view-projection     c4      camera (object space)
//   c5-c8   light positions (object)  c9      slot weights
//   c10     attenuation (a,b) 0,1     c11     attenuation (a,b) 2,3
//   c12     light 3 colour            c14-15  normal map uv transform
//   c18-21  object -> shadow mask (projective)
// ps constants
//   diffuse:  c0-c2 light colour * material colour
//   specular: c0 = (light 3 weight, light 2 weight, light 1 weight, light 0 weight)
static const char kVS[] =
  "precision highp float;\n"
  "invariant gl_Position;\n"
  "uniform vec4 vs_uniforms_vec4[22];\n"
  "#define C(i) vs_uniforms_vec4[i]\n"
  "in vec4 a_pos;\n"
  "in vec3 a_nrm;\n"
  "in vec3 a_tan;\n"
  "in vec2 a_uv;\n"
  "out vec4 v_mask;\n"
  "out vec3 v_L;\n"
  "out vec4 v_att;\n"
  "out vec3 v_col3;\n"
  "out vec2 v_uvN;\n"
  "vec3 E;\n"
  "vec3 dir(vec3 lp,vec3 p,vec2 ab,out float att)\n"
  "{\n"
  "  vec3 l = lp - p;\n"
  "  float d = length(l);\n"
  "  l = l / d;\n"
  "  att = max(d*ab.x + ab.y,0.0);\n"
  "#if MODE != 0\n"
  "  l = normalize(l + E);\n"
  "#endif\n"
  "  return l;\n"
  "}\n"
  "void main()\n"
  "{\n"
  "  vec4 p = a_pos;\n"
  "  gl_Position = vec4(dot(p, C(0)), dot(p, C(1)), dot(p, C(2)), dot(p, C(3)));\n"
  "  vec3 N = a_nrm;\n"
  "  vec3 T = a_tan;\n"
  "  vec3 B = N.yzx*T.zxy - N.zxy*T.yzx;\n"
  "  vec4 uv = vec4(a_uv,0.0,1.0);\n"
  "  v_uvN = vec2(dot(uv,C(14)),dot(uv,C(15)));\n"
  "  v_mask = vec4(dot(p,C(18)),dot(p,C(19)),dot(p,C(20)),dot(p,C(21)));\n"
  "  E = normalize(C(4).xyz - p.xyz);\n"
  "  vec4 att;\n"
  "  vec3 l0 = dir(C(5).xyz,p.xyz,C(10).xy,att.x);\n"
  "  vec3 l1 = dir(C(6).xyz,p.xyz,C(10).zw,att.y);\n"
  "  vec3 l2 = dir(C(7).xyz,p.xyz,C(11).xy,att.z);\n"
  "  vec3 l3 = dir(C(8).xyz,p.xyz,C(11).zw,att.w);\n"
  "  vec4 w = vec4(dot(l0,N),dot(l1,N),dot(l2,N),dot(l3,N))*0.5 + 0.5;\n"
  "  w = w * att * C(9);\n"
  "  vec3 s = l0*w.x + l1*w.y + l2*w.z + l3*w.w;\n"
  "  float ss = dot(s,s);\n"
  "  s = ss > 0.0 ? s*inversesqrt(ss) : N;\n"
  "  v_L = vec3(dot(s,N),dot(s,B),dot(s,T));\n"
  "  v_att = clamp(att.wzyx,0.0,1.0);\n"                    // oD0: colour interpolators saturate
  "  v_col3 = clamp(att.w*C(12).xyz,0.0,1.0);\n"            // oD1
  "}\n";

static const char kPS[] =
  "precision highp float;\n"
  "uniform vec4 ps_uniforms_vec4[4];\n"
  "#define P(i) ps_uniforms_vec4[i]\n"
  "uniform highp sampler2D ps_s0;\n"                        // normal map
  "uniform highp sampler2D ps_s1;\n"                        // shadow mask
  "uniform highp sampler2D kkZSnap;\n"
  "uniform mediump int kkZEqual;\n"
  "uniform mediump float kkBoost;\n"
  "in vec4 v_mask;\n"
  "in vec3 v_L;\n"
  "in vec4 v_att;\n"
  "in vec3 v_col3;\n"
  "in vec2 v_uvN;\n"
  "out vec4 kk_FragColor;\n"
  "void main()\n"
  "{\n"
  "  vec4 m = textureProj(ps_s1,v_mask);\n"
  "#if NRM\n"
  "  vec4 nt = texture(ps_s0,v_uvN);\n"
  "  vec3 n = nt.xyz;\n"
  "#else\n"
  "  vec3 n = vec3(1.0,0.0,0.0);\n"                         // tangent space is (N,B,T)
  "#endif\n"
  "  float nl = clamp(dot(n,normalize(v_L)),0.0,1.0);\n"
  "#if MODE == 0\n"
  "  vec3 c = v_col3 + P(0).rgb*(m.a*v_att.w) + P(1).rgb*(m.b*v_att.z) + P(2).rgb*(m.g*v_att.y);\n"
  "  kk_FragColor = vec4(c*nl,0.0);\n"
  "#else\n"
  "  float s2 = nl*nl;\n"
  "  float s4 = s2*s2;\n"
  "  float s8 = s4*s4;\n"
  "#if MODE == 1\n"
  "  float sp = s8;\n"
  "#elif MODE == 2\n"
  "  float sp = s8*s8;\n"
  "#else\n"
  "  float s16 = s8*s8;\n"
  "  float sp = s16*s16;\n"
  "#endif\n"
  "  float wt = (m.a*v_att.w)*P(0).w + dot(m.rgb*v_att.xyz,P(0).xyz);\n"
  "#if SPECMAP\n"
  "  wt *= nt.w;\n"
  "#endif\n"
  "  kk_FragColor = vec4(vec3(sp*wt),0.0);\n"
  "#endif\n"
  "  kk_FragColor.rgb *= kkBoost;\n"
  "  if(kkZEqual != 0 && gl_FragCoord.z < texelFetch(kkZSnap,ivec2(gl_FragCoord.xy),0).r - 0.00002)\n"
  "    discard;\n"
  "}\n";

const char *kk04ShaderSource(sInt variant,sInt fragment)
{
  static char *cache[16][2];
  variant &= 15;
  fragment = fragment ? 1 : 0;
  if(!cache[variant][fragment])
  {
    const char *body = fragment ? kPS : kVS;
    char head[160];
    sprintf(head,"#version 300 es\n#define NRM %d\n#define MODE %d\n#define SPECMAP %d\n",
            (variant & V_NRM) ? 1 : 0,(variant & V_MODEMASK) >> 1,(variant & V_SPECMAP) ? 1 : 0);
    char *s = new char[strlen(head) + strlen(body) + 1];
    strcpy(s,head);
    strcat(s,body);
    cache[variant][fragment] = s;
  }
  return cache[variant][fragment];
}

/****************************************************************************/
/***                                                                      ***/
/***   Material setups                                                    ***/
/***                                                                      ***/
/****************************************************************************/

struct kk04Setup { sU32 Key; sInt Id; };
static kk04Setup gSetups[64];
static sInt gSetupCount;

// key: variant | cull << 4 | normal map sampler flags << 8
static sInt GetSetup(sInt variant,sU32 baseFlags,sU32 tflags)
{
  static const sU32 filters[8][3] =
  {
    { sD3DTEXF_POINT,sD3DTEXF_POINT,sD3DTEXF_NONE },
    { sD3DTEXF_LINEAR,sD3DTEXF_LINEAR,sD3DTEXF_NONE },
    { sD3DTEXF_LINEAR,sD3DTEXF_LINEAR,sD3DTEXF_LINEAR },
    { sD3DTEXF_LINEAR,sD3DTEXF_ANISOTROPIC,sD3DTEXF_LINEAR },
    { sD3DTEXF_POINT,sD3DTEXF_POINT,sD3DTEXF_POINT },
    { sD3DTEXF_LINEAR,sD3DTEXF_LINEAR,sD3DTEXF_POINT },
    { sD3DTEXF_POINT,sD3DTEXF_POINT,sD3DTEXF_NONE },
    { sD3DTEXF_POINT,sD3DTEXF_POINT,sD3DTEXF_NONE },
  };

  sU32 cull = (baseFlags & sMBF_DOUBLESIDED) ? sD3DCULL_NONE : (baseFlags & sMBF_INVERTCULL) ? sD3DCULL_CW : sD3DCULL_CCW;
  sU32 samp = tflags & (7 | sMTF_CLAMP);
  sU32 key = variant | (cull << 4) | (samp << 8);
  for(sInt i=0;i<gSetupCount;i++)
    if(gSetups[i].Key == key)
      return gSetups[i].Id;

  sU32 states[128],*d = states;
  *d++ = sD3DRS_ALPHATESTENABLE;      *d++ = 0;
  *d++ = sD3DRS_ZENABLE;              *d++ = sD3DZB_TRUE;
  *d++ = sD3DRS_ZWRITEENABLE;         *d++ = 0;
  // 2004 used LESSEQUAL (its alpha-tested materials alpha-tested every pass);
  // the port's EQUAL keeps these passes out of alpha-test holes instead
  *d++ = sD3DRS_ZFUNC;                *d++ = sD3DCMP_EQUAL;
  *d++ = sD3DRS_CULLMODE;             *d++ = cull;
  *d++ = sD3DRS_COLORWRITEENABLE;     *d++ = 15;
  *d++ = sD3DRS_SLOPESCALEDEPTHBIAS;  *d++ = 0;
  *d++ = sD3DRS_DEPTHBIAS;            *d++ = 0;
  *d++ = sD3DRS_FOGENABLE;            *d++ = 0;
  *d++ = sD3DRS_CLIPPING;             *d++ = 1;
  *d++ = sD3DRS_ALPHABLENDENABLE;     *d++ = 1;
  *d++ = sD3DRS_BLENDOP;              *d++ = sD3DBLENDOP_ADD;
  *d++ = sD3DRS_SRCBLEND;             *d++ = sD3DBLEND_ONE;
  *d++ = sD3DRS_DESTBLEND;            *d++ = sD3DBLEND_ONE;
  *d++ = sD3DRS_STENCILENABLE;        *d++ = 0;
  *d++ = sD3DRS_TWOSIDEDSTENCILMODE;  *d++ = 0;
  *d++ = sD3DRS_STENCILFUNC;          *d++ = sD3DCMP_ALWAYS;

  // stage 0: normal map
  *d++ = sD3DSAMP_MAGFILTER;          *d++ = filters[samp & 7][0];
  *d++ = sD3DSAMP_MINFILTER;          *d++ = filters[samp & 7][1];
  *d++ = sD3DSAMP_MIPFILTER;          *d++ = filters[samp & 7][2];
  *d++ = sD3DSAMP_ADDRESSU;           *d++ = (samp & sMTF_CLAMP) ? sD3DTADDRESS_CLAMP : sD3DTADDRESS_WRAP;
  *d++ = sD3DSAMP_ADDRESSV;           *d++ = (samp & sMTF_CLAMP) ? sD3DTADDRESS_CLAMP : sD3DTADDRESS_WRAP;
  // stage 1: shadow mask (bilinear, as in the trace)
  *d++ = sD3DSAMP_1+sD3DSAMP_MAGFILTER; *d++ = sD3DTEXF_LINEAR;
  *d++ = sD3DSAMP_1+sD3DSAMP_MINFILTER; *d++ = sD3DTEXF_LINEAR;
  *d++ = sD3DSAMP_1+sD3DSAMP_MIPFILTER; *d++ = sD3DTEXF_NONE;
  *d++ = sD3DSAMP_1+sD3DSAMP_ADDRESSU;  *d++ = sD3DTADDRESS_CLAMP;
  *d++ = sD3DSAMP_1+sD3DSAMP_ADDRESSV;  *d++ = sD3DTADDRESS_CLAMP;
  *d++ = ~0U;
  *d++ = ~0U;

  sU32 code[2] = { KK04_MARKER | (sU32)variant, 0x0000ffff };
  sInt id = sSystem->MtrlAddSetup(states,code,code);
  if(gSetupCount < 64)
  {
    gSetups[gSetupCount].Key = key;
    gSetups[gSetupCount].Id = id;
    gSetupCount++;
  }
  return id;
}

// the uv transform rows the material11 vertex shader would use for a stage
static void UVTransform(const sMaterial11 *m,sInt stage,sVector &r0,sVector &r1)
{
  r0.Init(1,0,0,0);
  r1.Init(0,1,0,0);
  switch((m->TFlags[stage] >> 12) & 15)
  {
  case 1: // scale
    r0.x = r1.y = m->TScale[stage];
    break;
  case 2: // srt 1
    {
      sMatrix srt;
      srt.InitSRT(m->SRT1);
      srt.Trans4();
      r0 = srt.i;
      r1 = srt.j;
    }
    break;
  case 3: // srt 2
    {
      sF32 fs,fc;
      sFSinCos(m->SRT2[2],fs,fc);
      r0.Init( m->SRT2[0]*fc,m->SRT2[1]*fs,0,m->SRT2[3]);
      r1.Init(-m->SRT2[0]*fs,m->SRT2[1]*fc,0,m->SRT2[4]);
    }
    break;
  }
}

void kk04SetLight(const sMaterialEnv &env,const sMaterial11 *mtrl,sInt mode,const kk04Light *lights,sInt mask)
{
  sInt nrm = mtrl->GetTex(1);
  sInt variant = (nrm != sINVALID) ? V_NRM : 0;
  if(mode == KK04_SPECULAR)
  {
    // 2004 rounded the specular power to 8, 16 or 32 (repeated squaring)
    sInt power = mtrl->SpecPower < 8.5f ? 1 : mtrl->SpecPower < 16.5f ? 2 : 3;
    variant |= power << 1;
    if(nrm != sINVALID && (mtrl->SpecialFlags & sMSF_SPECMAP))
      variant |= V_SPECMAP;
  }
  sInt setup = GetSetup(variant,mtrl->BaseFlags,mtrl->TFlags[1]);

  sVector vc[KK04_VSREGS];
  sVector pc[KK04_PSREGS];
  sSetMem(vc,0,sizeof(vc));
  sSetMem(pc,0,sizeof(pc));

  sMatrix inv,mvp;
  inv.Invert(env.ModelSpace);

  mvp.Mul4(env.ModelSpace,sSystem->LastViewProject);   // c0-c3
  mvp.Trans4();
  vc[0] = mvp.i; vc[1] = mvp.j; vc[2] = mvp.k; vc[3] = mvp.l;
  vc[4].Rotate34(inv,env.CameraSpace.l);                // c4

  sVector mcol;
  mcol.InitColor(mtrl->Color[0]);
  for(sInt i=0;i<4;i++)
  {
    const kk04Light &l = lights[i];
    vc[5+i].Rotate34(inv,l.Pos);                        // c5-c8
    // slot weight: diffuse max(r,g,b), specular the colour's alpha
    (&vc[9].x)[i] = (mode == KK04_SPECULAR) ? l.Color.w : sMax(l.Color.x,sMax(l.Color.y,l.Color.z));
    sF32 *ab = &vc[10 + (i >> 1)].x + (i & 1) * 2;      // c10-c11
    ab[0] = (l.Range > 0.001f) ? -l.Amplify / l.Range : 0.0f;
    ab[1] = l.Amplify;
  }
  vc[12].Mul4(lights[3].Color,mcol);                    // c12

  UVTransform(mtrl,1,vc[14],vc[15]);                    // c14-c15

  // c18-c21: object -> clip (D3D conventions) -> mask texture
  {
    sMatrix view,proj,bias,t0,t1;
    view = env.CameraSpace;
    view.TransR();
    env.MakeProjectionMatrix(proj);
    proj.l.x += 1.0f / sSystem->ViewportX;             // without the D3D9 half-pixel shift,
    proj.l.y -= 1.0f / sSystem->ViewportY;             // like the GPU copy (_start_wasm.cpp)
    bias.Init();
    bias.i.x = 0.5f;
    bias.j.y = -0.5f;
    bias.l.x = 0.5f;
    bias.l.y = 0.5f;
    t0.Mul4(env.ModelSpace,view);
    t1.Mul4(t0,proj);
    t0.Mul4(t1,bias);
    t0.Trans4();
    vc[18] = t0.i; vc[19] = t0.j; vc[20] = t0.k; vc[21] = t0.l;
  }

  if(mode == KK04_SPECULAR)
  {
    // (light 3 slot takes light 0's red: a quirk of the 2004 code, kept)
    sVector c0,c1,c2;
    c0.Mul4(lights[0].Color,mcol);
    c1.Mul4(lights[1].Color,mcol);
    c2.Mul4(lights[2].Color,mcol);
    pc[0].Init(c0.x,c2.w,c1.w,c0.w);
  }
  else
  {
    for(sInt i=0;i<3;i++)
      pc[i].Mul4(lights[i].Color,mcol);
  }

  sMaterialInstance inst;
  inst.NumTextures = 2;
  inst.Textures[0] = nrm;
  inst.Textures[1] = mask;
  inst.NumVSConstants = KK04_VSREGS;
  inst.VSConstants = vc;
  inst.NumPSConstants = KK04_PSREGS;
  inst.PSConstants = pc;

  sSystem->MtrlSetSetup(setup);
  sSystem->MtrlSetInstance(inst);
}

/****************************************************************************/
/***                                                                      ***/
/***   Shadow mask                                                        ***/
/***                                                                      ***/
/****************************************************************************/

sInt kk04MaskTexture(sInt xs,sInt ys)
{
  static sInt tex = sINVALID, txs, tys;
  if(tex != sINVALID && (txs != xs || tys != ys))
  {
    sSystem->RemTexture(tex);
    tex = sINVALID;
  }
  if(tex == sINVALID)
  {
    tex = sSystem->AddTexture(xs,ys,sTF_A8R8G8B8,0);
    txs = xs;
    tys = ys;
  }
  return tex;
}

void kk04MaskChannel(sInt channel)
{
  // what 2004 added per shadow light (table at VA 0x83f308)
  static const sU32 colors[4] = { 0xff000000,0x000000ff,0x0000ff00,0x00ff0000 };
  static sMaterial11 *mtrl[4];
  channel &= 3;
  if(!mtrl[channel])
  {
    sMaterial11 *m = new sMaterial11;
    m->ShaderLevel = sPS_11;
    m->BaseFlags = sMBF_ZOFF|sMBF_BLENDADD|sMBF_NOTEXTURE|sMBF_NONORMAL|sMBF_DOUBLESIDED|sMBF_STENCILTEST;
    m->Color[0] = colors[channel];
    m->Combiner[sMCS_COLOR0] = sMCOA_SET;
    m->AlphaCombiner = sMCA_COL0;
    m->Compile();
    mtrl[channel] = m;
  }
  GenOverlayManager->FXQuad(mtrl[channel]);
}
