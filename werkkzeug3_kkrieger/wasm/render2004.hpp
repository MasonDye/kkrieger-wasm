// The lighting of the Breakpoint 2004 .kkrieger beta, see wasm/render2004.cpp.
#ifndef __RENDER2004_HPP__
#define __RENDER2004_HPP__

#include "_types.hpp"

struct sMaterialEnv;
class sMaterial11;

// a raw GLSL setup: vertex "token stream" { KK04_MARKER | variant, end }
#define KK04_MARKER   0x4b4b0000
#define KK04_VSREGS   22                // vs constants c0..c21
#define KK04_PSREGS   4                 // ps constants c0..c3

const char *kk04ShaderSource(sInt variant,sInt fragment);

// one of the (at most four) lights a 2004 frame is lit with
struct kk04Light
{
  sVector Pos;                          // world space
  sVector Color;                        // r,g,b,a in 0..1
  sF32 Range;
  sF32 Amplify;
};

enum
{
  KK04_DIFFUSE = 0,                     // light pass (2004 light mode 5)
  KK04_SPECULAR = 1,                    // specular pass (2004 light mode 6)
};

// Sets the four-light pass for one mesh job. `mtrl` is the job's ENGU_LIGHT
// material (normal map, diffuse colour, specular power), `lights` the frame's
// four light slots (unused ones have Amplify 0), `mask` the shadow mask
// render target.
void kk04SetLight(const sMaterialEnv &env,const sMaterial11 *mtrl,sInt mode,const kk04Light *lights,sInt mask);

// The shadow mask: a render target at half the view size.
sInt kk04MaskTexture(sInt xs,sInt ys);
// Adds one mask channel (0 = alpha/light 0, 1 = blue, 2 = green, 3 = red)
// wherever the stencil is 0.
void kk04MaskChannel(sInt channel);

#endif
