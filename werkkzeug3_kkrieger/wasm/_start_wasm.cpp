// .kkrieger WebAssembly platform layer -- the emscripten/SDL2/WebGL2
// replacement for _start.cpp (Win32 + Direct3D 9 + DirectSound).
//
// Everything the game touches goes through sSystem_, and sSystem_ never hands
// a D3D object to the game: textures, geometry and materials are all integer
// handles. That is what makes this port tractable -- only this one file knows
// about the graphics API.
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#include "_types.hpp"
#include "_start.hpp"
#include "_startdx.hpp"
#include "wasm/shader_translate.hpp"
#include "kdoc.hpp"

#if !defined(KK_HEADLESS)
#include <SDL.h>
#endif
#include <emscripten/em_js.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/****************************************************************************/
/***                                                                      ***/
/***   Win32 API leftovers referenced from portable files                 ***/
/***                                                                      ***/
/****************************************************************************/

// _types.cpp: sSPrintF / sDPrintF route through user32's wvsprintfA. Its
// buffer is 1024 bytes.
extern "C" int wvsprintfA(char *buffer,const char *fmt,va_list args)
{
  return vsnprintf(buffer,1024,fmt,args);
}

// _types.cpp: sFormatString's %f / %e go through the MSVC CRT's _fcvt / _ecvt.
// Both return the digits of the value with the decimal point given separately.
static char kkCvtBuf[512];

extern "C" char *_fcvt(double value,int count,int *dec,int *sign)
{
  char tmp[512];
  *sign = value < 0 ? 1 : 0;
  if(value < 0) value = -value;
  if(count < 0) count = 0;
  if(count > 300) count = 300;
  snprintf(tmp,sizeof(tmp),"%.*f",count,value);

  char *src = tmp, *dst = kkCvtBuf;
  int intDigits = 0;
  while(*src && *src != '.')                        // integer part, "0" dropped
  {
    if(!(intDigits == 0 && *src == '0' && (src[1] == 0 || src[1] == '.')))
    {
      *dst++ = *src;
      intDigits++;
    }
    src++;
  }
  if(*src == '.') src++;
  while(*src) *dst++ = *src++;                      // fraction
  *dst = 0;
  *dec = intDigits;
  return kkCvtBuf;
}

extern "C" char *_ecvt(double value,int count,int *dec,int *sign)
{
  char tmp[512];
  *sign = value < 0 ? 1 : 0;
  if(value < 0) value = -value;
  if(count < 1) count = 1;
  if(count > 300) count = 300;
  snprintf(tmp,sizeof(tmp),"%.*e",count-1,value);

  char *src = tmp, *dst = kkCvtBuf;
  while(*src && *src != 'e')                        // mantissa without the point
  {
    if(*src != '.') *dst++ = *src;
    src++;
  }
  *dst = 0;
  *dec = (value == 0) ? 1 : (*src == 'e' ? atoi(src+1) + 1 : 1);
  return kkCvtBuf;
}

// _types.cpp: set by sFatal so debug output stops recursing.
sInt sFatality;

// kdoc.cpp: "time of year" event variable. FILETIME is 100ns ticks since
// 1601-01-01; the struct mirrors the one kdoc.cpp declares.
struct kkSYSTEMTIME
{
  sU16 wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds;
};

extern "C" void GetSystemTime(kkSYSTEMTIME *st)
{
  time_t t = time(0);
  struct tm tmv;
  gmtime_r(&t,&tmv);
  st->wYear = (sU16)(tmv.tm_year + 1900);
  st->wMonth = (sU16)(tmv.tm_mon + 1);
  st->wDayOfWeek = (sU16)tmv.tm_wday;
  st->wDay = (sU16)tmv.tm_mday;
  st->wHour = (sU16)tmv.tm_hour;
  st->wMinute = (sU16)tmv.tm_min;
  st->wSecond = (sU16)tmv.tm_sec;
  st->wMilliseconds = 0;
}

// days from civil, Howard Hinnant's algorithm (proleptic Gregorian)
static sS64 DaysFromCivil(sInt y,sInt m,sInt d)
{
  y -= m <= 2;
  const sS64 era = (y >= 0 ? y : y-399) / 400;
  const sInt yoe = (sInt)(y - era*400);
  const sInt doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
  const sInt doe = yoe*365 + yoe/4 - yoe/100 + doy;
  return era*146097 + doe - 719468;               // days since 1970-01-01
}

extern "C" sBool SystemTimeToFileTime(const kkSYSTEMTIME *st,sU64 *ft)
{
  sS64 days = DaysFromCivil(st->wYear,st->wMonth,st->wDay);
  sS64 secs = days*86400 + st->wHour*3600 + st->wMinute*60 + st->wSecond;
  // 1970-01-01 is 11644473600 s after 1601-01-01
  *ft = (sU64)(secs + 11644473600LL) * 10000000ULL + (sU64)st->wMilliseconds * 10000ULL;
  return sTRUE;
}

/****************************************************************************/
/***                                                                      ***/
/***   Globals                                                            ***/
/***                                                                      ***/
/****************************************************************************/

sSystem_ *sSystem;
class sBroker_ *sBroker;

sInt IntroTargetAspect = 0;
sBool IntroStereo3D = sFALSE;
sInt IntroLoop;

#if !defined(KK_HEADLESS)
static SDL_Window *gWindow;
static SDL_GLContext gGL;
static SDL_AudioDeviceID gAudioDev;
#else
static int gAudioDev;
#endif
static sChar gCmdLine[256] = "/kkrieger.kx";
static sInt gFontPageX, gFontPageY;      // exported player data, not the .k editor document
static sInt gStartTicks;
static sBool gInitDone;

// sound
static sSoundHandler gSoundHandler;
static void *gSoundUser;
static sInt gSoundAlign;
static sInt gSamplesPlayed;

// input state (the matching sSystem_ members only exist in non-intro builds)
static sInt gMouseX, gMouseY;
static sU32 gKeyQual;

extern sBool sAppHandler(sInt code,sDInt value);

// per-frame diagnostics: counters printed every 120 frames, plus a full call
// trace of one early frame
static sInt gFrame, gTraceLeft;
extern sInt kkExecTrace;                                  // kdoc.cpp: one-frame op trace
extern sInt kkPaintAllSectors;                            // engine.cpp: portal visibility off
static sInt cViewport, cClear, cSetup, cInstT, cInstP, cDraw, cDrawEmpty, cGeoEnd;
#define KKTRACE(...) do { if(gTraceLeft>0) { gTraceLeft--; fprintf(stderr,"[kk] T " __VA_ARGS__); } } while(0)

#define GLCHECK() glCheck(__LINE__)
static void glCheck(int line)
{
  GLenum e = glGetError();
  if(e != GL_NO_ERROR)
    printf("[kk] GL error 0x%04x at _start_wasm.cpp:%d\n",(unsigned)e,line);
}
// in trace mode, sample the centre of the current framebuffer
// debug (F9): keep a copy of every post-processing stage as a data URL in
// window.__kkShots, for wasm/cdp.js to save
EM_JS(void, kkSaveShot, (const char *name, const unsigned char *px, int w, int h), {
  try {
    var c = document.createElement('canvas'); c.width = w; c.height = h;
    var x = c.getContext('2d'); var img = x.createImageData(w, h);
    for (var y = 0; y < h; y++)                      // GL rows are bottom-up
      img.data.set(HEAPU8.subarray(px + (h - 1 - y) * w * 4, px + (h - y) * w * 4), y * w * 4);
    for (var i = 3; i < img.data.length; i += 4) img.data[i] = 255;
    x.putImageData(img, 0, 0);
    (window.__kkShots = window.__kkShots || []).push([UTF8ToString(name), c.toDataURL('image/png')]);
  } catch (e) {}
});
static sInt kkShotCount;

// debug: operators listed in window.__kkDumpOps save their generated bitmap
EM_JS(int, kkShouldDumpOp, (int id), {
  return (window.__kkDumpOps && window.__kkDumpOps.indexOf(id) >= 0) ? 1 : 0;
});
// debug: setups listed in window.__kkDumpSetups dump their program during an F9 trace
EM_JS(int, kkShouldDumpSetup, (int id), {
  return (window.__kkDumpSetups && window.__kkDumpSetups.indexOf(id) >= 0) ? 1 : 0;
});
// debug: game code asks whether window[name] is set (flags set from cdp.js / the console)
EM_JS(int, kkJsFlag, (const char *name), { return window[UTF8ToString(name)] ? 1 : 0; });
// resolution picked on the page (shell.html puts [w,h] into Module.kkRes)
EM_JS(int, kkPageRes, (int which), { var r = Module.kkRes; return (r && r.length == 2) ? (r[which] | 0) : 0; });
sInt kkForcedResX, kkForcedResY;                          // 0 = the game's own resolution switch
EM_JS(int, kkJsInt, (const char *name), { var v = window[UTF8ToString(name)]; return (typeof v === 'number') ? v : -1; });
// debug: take (and clear) a number array window[name]; returns how many were copied
EM_JS(int, kkJsVec, (const char *name, float *out, int n), {
  var k = UTF8ToString(name), v = window[k];
  if (!Array.isArray(v)) return 0;
  window[k] = null;
  var m = Math.min(n, v.length);
  for (var i = 0; i < m; i++) HEAPF32[(out >> 2) + i] = +v[i];
  return m;
});
// debug: with window.__kkTracePickup set, the game traces the frames right after an item pickup
EM_JS(int, kkTracePickupWanted, (), { return window.__kkTracePickup ? 1 : 0; });
static sInt gTraceInFrames;
void kkTraceSoon(sInt frames)
{
  if(kkTracePickupWanted()) gTraceInFrames = frames;
}
void kkDumpBitmap(sInt opid,const sU16 *px,sInt w,sInt h)
{
  if(!kkShouldDumpOp(opid)) return;
  sU8 *buf = new sU8[w*h*4], *ab = new sU8[w*h*4];
  for(sInt y=0;y<h;y++)                          // kkSaveShot flips rows (GL order)
    for(sInt x=0;x<w;x++)
    {
      const sU16 *s = px + ((h-1-y)*w + x)*4;     // lanes b,g,r,a, 15 bit
      sU8 *d = buf + (y*w+x)*4, *a = ab + (y*w+x)*4;
      d[0] = sRange<sInt>(s[2]>>7,255,0); d[1] = sRange<sInt>(s[1]>>7,255,0); d[2] = sRange<sInt>(s[0]>>7,255,0); d[3] = 255;
      a[0] = a[1] = a[2] = sRange<sInt>(s[3]>>7,255,0); a[3] = 255;
    }
  sChar name[64];
  sprintf(name,"op%d_rgb",opid);   kkSaveShot(name,buf,w,h);
  sprintf(name,"op%d_alpha",opid); kkSaveShot(name,ab,w,h);
  delete[] buf; delete[] ab;
}
static void kkCaptureStage(const char *what)
{
  GLint vp[4]; glGetIntegerv(GL_VIEWPORT,vp);
  if(vp[2] <= 0 || vp[3] <= 0 || kkShotCount >= 400) return;
  sU8 *buf = new sU8[vp[2]*vp[3]*4];
  glReadPixels(vp[0],vp[1],vp[2],vp[3],GL_RGBA,GL_UNSIGNED_BYTE,buf);
  sChar name[128];
  sprintf(name,"%02d_%s",kkShotCount++,what);
  kkSaveShot(name,buf,vp[2],vp[3]);
  delete[] buf;
}
// debug: window[name] set? (and clear it: one-shot requests from cdp.js)
EM_JS(int, kkTakeFlag, (const char *name), { var k = UTF8ToString(name); var v = window[k] ? 1 : 0; window[k] = 0; return v; });
// debug: the 2004 renderer saves its stages (render target rows are D3D
// order, so those images come out upside down)
void kk04Capture(const char *what)
{
  kkCaptureStage(what);
}

static void kkProbe(const char *what)
{
  if(gTraceLeft <= 0) return;
  GLint vp[4]; glGetIntegerv(GL_VIEWPORT,vp);
  sInt lit = 0, maxv = 0, sum = 0;
  for(sInt y=0;y<6;y++) for(sInt x=0;x<10;x++)
  {
    sU8 px[4];
    glReadPixels(vp[0] + (vp[2]*(2*x+1))/20, vp[1] + (vp[3]*(2*y+1))/12, 1,1, GL_RGBA,GL_UNSIGNED_BYTE,px);
    sInt v = sMax(px[0],sMax(px[1],px[2]));
    if(v > 8) lit++;
    if(v > maxv) maxv = v;
    sum += v;
  }
  fprintf(stderr,"[kk] T   probe %s: lit %d/60 max=%d avg=%d (vp %d,%d %dx%d)\n",what,lit,maxv,sum/60,vp[0],vp[1],vp[2],vp[3]);
}

// in trace mode, report GL errors after every logged call
#define KKTRACEERR(what) do { if(gTraceLeft>0) { GLenum e__ = glGetError(); if(e__) fprintf(stderr,"[kk] T   ^^^ GL error 0x%x after %s\n",(unsigned)e__,what); } } while(0)

// Quads arrive as 4 vertices each with no index buffer; D3D drew them through
// a static index buffer, so do we.
static GLuint gQuadIb;
static sInt gQuadIbQuads;
static void EnsureQuadIb(sInt quads)
{
  if(quads <= gQuadIbQuads) return;
  sInt n = sMax(quads,1024);
  sU16 *ib = new sU16[n*6];
  for(sInt q=0;q<n;q++)
  {
    ib[q*6+0] = (sU16)(q*4+0); ib[q*6+1] = (sU16)(q*4+1); ib[q*6+2] = (sU16)(q*4+2);
    ib[q*6+3] = (sU16)(q*4+0); ib[q*6+4] = (sU16)(q*4+2); ib[q*6+5] = (sU16)(q*4+3);
  }
  if(!gQuadIb) glGenBuffers(1,&gQuadIb);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gQuadIb);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,n*6*2,ib,GL_STATIC_DRAW);
  delete[] ib;
  gQuadIbQuads = n;
}

/****************************************************************************/
/***                                                                      ***/
/***   Vertex formats                                                     ***/
/***                                                                      ***/
/****************************************************************************/

// Attribute slots, matched by the generated GLSL.
#define ATT_POS      KKATT_POS
#define ATT_NORMAL   KKATT_NORMAL
#define ATT_COLOR0   KKATT_COLOR0
#define ATT_UV0      KKATT_UV0
#define ATT_UV1      KKATT_UV1
#define ATT_TANGENT  KKATT_TANGENT
#define ATT_COLOR1   KKATT_COLOR1
#define ATT_BINORMAL KKATT_BINORMAL
#define ATT_MAX      KKATT_MAX

struct sFVFAttr { sU8 Attr, Size, Type, Norm; sU8 Offset; };
// Type: 0 = GL_FLOAT, 1 = GL_UNSIGNED_BYTE (bgra colour), 2 = packed normal
struct sFVFDesc { sInt Stride; sInt Count; sFVFAttr A[8]; };

static const sFVFDesc FVFTable[8] =
{
  { 0, 0, {} },                                                     // 0: unused
  { 32, 3, {{ATT_POS,3,0,0,0},{ATT_NORMAL,3,0,0,12},                // sFVF_STANDARD
             {ATT_UV0,2,0,0,24}} },
  { 32, 4, {{ATT_POS,3,0,0,0},{ATT_COLOR0,4,1,1,12},                // sFVF_DOUBLE
             {ATT_UV0,2,0,0,16},{ATT_UV1,2,0,0,24}} },
  { 64, 7, {{ATT_POS,3,0,0,0},{ATT_NORMAL,3,0,0,12},                // sFVF_TSPACE
             {ATT_COLOR0,4,1,1,24},{ATT_COLOR1,4,1,1,28},
             {ATT_UV0,2,0,0,32},{ATT_TANGENT,3,0,0,40},
             {ATT_BINORMAL,3,0,0,52}} },
  { 16, 2, {{ATT_POS,3,0,0,0},{ATT_COLOR0,4,1,1,12}} },             // sFVF_COMPACT
  { 16, 1, {{ATT_POS,4,0,0,0}} },                                   // sFVF_XYZW
  { 32, 5, {{ATT_POS,3,0,0,0},{ATT_NORMAL,4,1,1,12},                // sFVF_TSPACE3
             {ATT_TANGENT,4,1,1,16},{ATT_COLOR0,4,1,1,20},
             {ATT_UV0,2,0,0,24}} },
  { 48, 5, {{ATT_POS,3,0,0,0},{ATT_NORMAL,3,0,0,12},                // sFVF_TSPACE3BIG
             {ATT_TANGENT,3,0,0,24},{ATT_COLOR0,4,1,1,36},
             {ATT_UV0,2,0,0,40}} },
};

/****************************************************************************/
/***                                                                      ***/
/***   Per-object GL state (kept out of the shared headers)               ***/
/***                                                                      ***/
/****************************************************************************/

struct sGLTex   { GLuint Name; GLenum Target; GLuint Fbo; GLuint DepthRb; sU8 Avg[4]; sBool Mips; };   // Mips: a full mip chain was uploaded
struct sGLGeo   { GLuint Vao, Vb, Ib; sInt VbBytes, IbBytes; sU8 *VStage, *IStage; sInt VStageSize, IStageSize; sBool VertexValid, IndexValid; };
struct sGLSetup { GLuint Prog; sBool Tried; };

static sGLTex   gTex[MAX_TEXTURE];

// debug: save a render-target texture (all of it) through its FBO
static void kkCaptureTexture(sInt tex,const char *what)
{
  if(tex<=0 || tex>=MAX_TEXTURE || !gTex[tex].Fbo || kkShotCount >= 400) return;
  sInt w = sSystem->Textures[tex].XSize, h = sSystem->Textures[tex].YSize;
  GLint oldfb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&oldfb);
  glBindFramebuffer(GL_FRAMEBUFFER,gTex[tex].Fbo);
  sU8 *buf = new sU8[w*h*4];
  glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,buf);
  // alpha as a second image: grey = alpha
  sU8 *ab = new sU8[w*h*4];
  for(sInt i=0;i<w*h;i++) { ab[i*4]=ab[i*4+1]=ab[i*4+2]=buf[i*4+3]; ab[i*4+3]=255; }
  glBindFramebuffer(GL_FRAMEBUFFER,oldfb);
  sChar name[128];
  sprintf(name,"%02d_%s_tex%d",kkShotCount++,what,tex);
  kkSaveShot(name,buf,w,h);
  sprintf(name,"%02d_%s_tex%d_alpha",kkShotCount++,what,tex);
  kkSaveShot(name,ab,w,h);
  delete[] buf; delete[] ab;
}

static sGLGeo   gGeo[MAX_GEOHANDLE];
static sGLSetup gSetup[MAX_SETUPS];

static GLenum CmpGL(sU32 v);
static GLenum StencilCmpGL(sU32 v);
static GLenum StencilOpGL(sU32 v);
static GLenum BlendGL(sU32 v);
static void ApplyStencil(const sU32 *st);

// Rendering into a texture: D3D's render targets have row 0 at the top, GL's
// at the bottom. We flip the projection's y (and the cull winding) while a
// render target is bound so the textures come out D3D-oriented and every
// sampling path in the game keeps working unchanged.
static sBool gRTActive;
sInt kkInvertStencil = 0;                                 // debug (F5): invert the shadow stencil test
sInt kkShowShadowVolumes = 0;                             // debug (F4): draw the shadow volumes
sInt kkLightTestOff = 0;                                  // debug (F3): 1 = no scissor, 2 = no depth, 3 = no stencil
sInt kkStencilMarkVolumes = 0;                            // debug (F2): shadow volumes stamp the stencil
static sU32 gCullMode;
static sInt kkAlphaTestOff = 0;                  // debug (K): ignore ALPHATESTENABLE
static sBool gScissorOn;                                  // (defined early: GeoDraw needs it)
static sU32 gAppliedStates[0x100];                        // what GL actually has (~0 = unknown)
static sInt cStateSet, cStateSkip;                        // per-frame statistics
sInt kkOnlyMtrl = 0;                             // debug (U): paint only this mesh material index
sInt kkDumpJobs = 0;                             // debug (G): log one frame of base paint jobs
sInt kkNoFrustumCull = 0;                        // debug (H): engine draws every job
static sInt kkCullDebug = 0;                     // debug (J): 1 culling off, 2 inverted winding
static void ApplyCull()
{
  // D3D's front face is clockwise in its y-down screen space, which is
  // counter-clockwise in GL's y-up NDC. Rendering into textures flips y.
  // Winding: the engine's front faces come out clockwise in GL window space
  // on screen; rendering into a texture flips y (see gRTActive), which flips
  // the winding with it.
  glFrontFace(gRTActive ? GL_CCW : GL_CW);
  if(kkCullDebug == 2) glFrontFace(gRTActive ? GL_CW : GL_CCW);   // debug (J): inverted winding
  if(gCullMode == sD3DCULL_NONE || gCullMode == 0 || kkCullDebug == 1) { glDisable(GL_CULL_FACE); return; }
  glEnable(GL_CULL_FACE);
  glCullFace(gCullMode == sD3DCULL_CW ? GL_FRONT : GL_BACK);   // D3DCULL_CW culls the front faces
}

// Stencil, including D3D's two-sided mode (STENCIL* for front faces,
// CCW_STENCIL* for back faces).
static void ApplyStencil(const sU32 *st)
{
  sU32 mask = st[sD3DRS_STENCILMASK] ? st[sD3DRS_STENCILMASK] : 0xff;
  GLint ref = (GLint)st[sD3DRS_STENCILREF];
  extern sInt kkInvertStencil, kkStencilMarkVolumes;
  sU32 fn = st[sD3DRS_STENCILFUNC];
  if(kkInvertStencil && fn == sD3DCMP_EQUAL) fn = sD3DCMP_NOTEQUAL;   // debug (F5)

  // debug (F2): make every rasterised shadow-volume fragment set the stencil,
  // no matter how the depth test goes - if the picture still does not change,
  // the volumes are not reaching the rasteriser at all
  if(kkStencilMarkVolumes && fn == sD3DCMP_ALWAYS
     && (st[sD3DRS_STENCILZFAIL] != sD3DSTENCILOP_KEEP || st[sD3DRS_STENCILPASS] != sD3DSTENCILOP_KEEP))
  {
    glStencilFunc(GL_ALWAYS,1,0xff);
    glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);
    return;
  }
  if(st[sD3DRS_TWOSIDEDSTENCILMODE])
  {
    glStencilFuncSeparate(GL_FRONT,StencilCmpGL(fn),ref,mask);
    glStencilOpSeparate(GL_FRONT,StencilOpGL(st[sD3DRS_STENCILFAIL]),StencilOpGL(st[sD3DRS_STENCILZFAIL]),StencilOpGL(st[sD3DRS_STENCILPASS]));
    glStencilFuncSeparate(GL_BACK,StencilCmpGL(st[sD3DRS_CCW_STENCILFUNC]),ref,mask);
    glStencilOpSeparate(GL_BACK,StencilOpGL(st[sD3DRS_CCW_STENCILFAIL]),StencilOpGL(st[sD3DRS_CCW_STENCILZFAIL]),StencilOpGL(st[sD3DRS_CCW_STENCILPASS]));
  }
  else
  {
    glStencilFunc(StencilCmpGL(fn),ref,mask);
    glStencilOp(StencilOpGL(st[sD3DRS_STENCILFAIL]),StencilOpGL(st[sD3DRS_STENCILZFAIL]),StencilOpGL(st[sD3DRS_STENCILPASS]));
  }
}

// built-in textures the material system addresses as -2 / -3
static GLuint gNormalCube;
static GLuint gAttenuationVolume;

// current render state, mirrored so we do not spam the driver
static sMaterialEnv gEnv;
static sMatrix gViewProject;
static sBool gHaveEnv;

/****************************************************************************/
/***                                                                      ***/
/***   Placeholder shader                                                 ***/
/***                                                                      ***/
/****************************************************************************/
//
// TODO(shader): sShaderCodeGen still produces real Direct3D 9 vs.1.1/ps.1.1
// bytecode (see materials/material11.vsh). Translating it to GLSL ES is the
// next milestone -- MojoShader is the intended backend. Until then every
// material renders through this stand-in so geometry is visible.

static const char *kPlaceholderVS =
  "#version 300 es\n"
  "layout(location=0) in vec4 aPos;\n"
  "layout(location=1) in vec3 aNormal;\n"
  "layout(location=2) in vec4 aColor0;\n"
  "layout(location=3) in vec2 aUV0;\n"
  "uniform mat4 uViewProject;\n"
  "out vec2 vUV;\n"
  "out vec3 vNormal;\n"
  "out vec4 vColor;\n"
  "void main(){\n"
  "  gl_Position = uViewProject * vec4(aPos.xyz,1.0);\n"
  "  vUV = aUV0; vNormal = aNormal; vColor = aColor0;\n"
  "}\n";

static const char *kPlaceholderPS =
  "#version 300 es\n"
  "precision highp float;\n"
  "in vec2 vUV;\n"
  "in vec3 vNormal;\n"
  "in vec4 vColor;\n"
  "uniform sampler2D uTex0;\n"
  "uniform int uHasTex;\n"
  "out vec4 oColor;\n"
  "void main(){\n"
  "  vec4 c = (uHasTex != 0) ? texture(uTex0,vUV) : vec4(0.7,0.7,0.75,1.0);\n"
  "  float l = 0.55 + 0.45*abs(normalize(vNormal + vec3(0.0,0.0,1e-6)).z);\n"
  "  oColor = vec4(c.rgb * l, c.a);\n"
  "}\n";

static GLuint gPlaceholderProg;
static GLint  gLocViewProject, gLocTex0, gLocHasTex;

static GLuint CompileShader(GLenum type,const char *src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s,1,&src,0);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok)
  {
    char log[2048]; GLsizei n = 0;
    glGetShaderInfoLog(s,sizeof(log),&n,log);
    printf("[kk] shader compile failed: %.*s\n",(int)n,log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint LinkProgram(const char *vs,const char *ps)
{
  GLuint v = CompileShader(GL_VERTEX_SHADER,vs);
  GLuint f = CompileShader(GL_FRAGMENT_SHADER,ps);
  if(!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p,v); glAttachShader(p,f);
  glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok)
  {
    char log[2048]; GLsizei n = 0;
    glGetProgramInfoLog(p,sizeof(log),&n,log);
    printf("[kk] program link failed: %.*s\n",(int)n,log);
    glDeleteProgram(p); p = 0;
  }
  glDeleteShader(v); glDeleteShader(f);
  return p;
}

/****************************************************************************/
/***                                                                      ***/
/***   init / exit / debug                                                ***/
/***                                                                      ***/
/****************************************************************************/

void sSetConfig(sU32 flags,sInt xs,sInt ys)
{
  if(!sSystem)
  {
    sSystem = new sSystem_;
    sSetMem(((sU8 *)sSystem)+4,0,sizeof(sSystem_)-4);
  }
  sSystem->ConfigFlags = flags;
  sSystem->ConfigX = xs;
  sSystem->ConfigY = ys;
}

void sSystem_::Log(sChar *s)                { printf("[kk] %s",s ? s : ""); }
void sSystem_::Abort(sChar *msg)            { printf("[kk] FATAL: %s\n",msg ? msg : ""); emscripten_force_exit(1); for(;;){} }
void sSystem_::Exit()                       { WAborting = 1; }
void sSystem_::Tag()                        {}
sInt sSystem_::MemoryUsed()                 { return 0; }
void sSystem_::CheckMem()                   {}
sChar *sSystem_::GetCmdLine()               { return gCmdLine; }

void sSystem_::Init(sU32 flags,sInt xs,sInt ys)
{
  ConfigFlags = flags;
  if(xs>0) ConfigX = xs;
  if(ys>0) ConfigY = ys;
}

/****************************************************************************/
/***                                                                      ***/
/***   time / input                                                       ***/
/***                                                                      ***/
/****************************************************************************/

static sInt kkTicks()                       { return (sInt)emscripten_get_now(); }
sInt sSystem_::GetTime()                    { return kkTicks() - gStartTicks; }
sInt sSystem_::GetTimeOfDay()               { return kkTicks()/1000; }
void sSystem_::PerfKalib()                  {}
sU32 sSystem_::PerfTime()                   { return (sU32)(emscripten_get_now()*1000.0); }
void sSystem_::GetPerf(sPerfInfo &i,sInt)   { sSetMem(&i,0,sizeof(i)); }

sU32 sSystem_::GetKeyboardShiftState()      { return gKeyQual; }
sBool sSystem_::GetAbortKey()               { return sFALSE; }
void sSystem_::ClearAbortKey()              {}

sBool sSystem_::GetWinMouse(sInt &x,sInt &y)    { x = gMouseX; y = gMouseY; return sTRUE; }
sBool sSystem_::GetWinMouseAbs(sInt &x,sInt &y) { x = gMouseX; y = gMouseY; return sTRUE; }
void sSystem_::SetWinMouse(sInt,sInt)           {}
void sSystem_::SetWinTitle(sChar *)             {}
void sSystem_::MoveWindow(sInt,sInt)            {}
void sSystem_::SetWinMode(sInt)                 {}
sInt sSystem_::GetWinMode()                     { return 0; }
void sSystem_::HideWinMouse(sBool)              {}
void sSystem_::GetKeyName(sChar *b,sInt s,sU32) { if(s>0) b[0]=0; }
sBool sSystem_::FileRequester(sChar *,sInt,sU32){ return sFALSE; }

// Mouse deltas are accumulated by the event pump and drained here; that is
// what the game's look control expects (sIDT_TABLET == relative device).
static sInt gMouseDX, gMouseDY;

void sSystem_::GetInput(sInt id,sInputData &data)
{
  sSetMem(&data,0,sizeof(data));
  if(id != 0)
    return;
  data.Type = sIDT_TABLET;
  data.AnalogCount = 2;
  data.DigitalCount = 3;
  // the game keeps its own "last position" and differentiates, so report an
  // accumulating counter rather than per-call deltas
  data.Analog[0] = gMouseDX;
  data.Analog[1] = gMouseDY;
  data.Digital = MouseButtons;
}

/****************************************************************************/
/***                                                                      ***/
/***   file                                                               ***/
/***                                                                      ***/
/****************************************************************************/

sU8 *sSystem_::LoadFile(const sChar *name,sInt &size)
{
  size = 0;
  FILE *f = fopen(name,"rb");
  if(!f) { printf("[kk] LoadFile failed: %s\n",name); return 0; }
  fseek(f,0,SEEK_END);
  long len = ftell(f);
  fseek(f,0,SEEK_SET);
  sU8 *mem = new sU8[len ? len : 1];
  if(fread(mem,1,len,f) != (size_t)len) { fclose(f); delete[] mem; return 0; }
  fclose(f);
  size = (sInt)len;
  return mem;
}

sU8 *sSystem_::LoadFile(const sChar *name)      { sInt s; return LoadFile(name,s); }

sChar *sSystem_::LoadText(const sChar *name)
{
  sInt size;
  sU8 *d = LoadFile(name,size);
  if(!d) return 0;
  sChar *t = new sChar[size+1];
  sCopyMem(t,d,size);
  t[size] = 0;
  delete[] d;
  return t;
}

sBool sSystem_::SaveFile(const sChar *name,const sU8 *data,sInt size)
{
  FILE *f = fopen(name,"wb");
  if(!f) return sFALSE;
  sBool ok = fwrite(data,1,size,f) == (size_t)size;
  fclose(f);
  return ok;
}

sU64 sSystem_::GetFileStamp(const sChar *)                          { return 0; }
sU8 *sSystem_::LoadFileIfNewerThan(const sChar *n,const sChar *,sInt &s) { return LoadFile(n,s); }
sDirEntry *sSystem_::LoadDir(const sChar *)                         { return 0; }
sBool sSystem_::MakeDir(const sChar *)                              { return sFALSE; }
sBool sSystem_::CheckDir(const sChar *)                             { return sFALSE; }
sBool sSystem_::CheckFile(const sChar *n)                           { FILE *f=fopen(n,"rb"); if(f){fclose(f);return sTRUE;} return sFALSE; }
sBool sSystem_::RenameFile(const sChar *,const sChar *)             { return sFALSE; }
sBool sSystem_::DeleteFile(const sChar *)                           { return sFALSE; }
void sSystem_::GetCurrentDir(sChar *b,sInt s)                       { if(s>1) { b[0]='/'; b[1]=0; } }
sU32 sSystem_::GetDriveMask()                                       { return 0; }

sBitmap *sSystem_::LoadBitmap(const sU8 *,sInt)                     { return 0; }
sBool sSystem_::LoadBitmapCore(const sU8 *,sInt,sInt &,sInt &,sU8 *&){ return sFALSE; }

/****************************************************************************/
/***                                                                      ***/
/***   host font (used by Bitmap_Text)                                    ***/
/***                                                                      ***/
/****************************************************************************/
//
// The original rasterises glyphs with GDI into a 32 bit page; here a 2D
// canvas does the same job. The game reads the low byte of each pixel as
// glyph coverage (see Bitmap_Text), so coverage is written to all channels.

EM_JS(int, kkFontBegin, (int pagex, int pagey, const char *name, int xs, int ys, int style), {
  if (typeof document === 'undefined') return ys;
  var cv = document.createElement('canvas');
  cv.width = pagex; cv.height = pagey;
  var ctx = cv.getContext('2d', { willReadFrequently: true });
  var family = UTF8ToString(name) || 'sans-serif';
  var px = Math.max(1, ys);
  ctx.font = ((style & 1) ? 'italic ' : '') + ((style & 2) ? 'bold ' : '') + px + 'px "' + family + '", Arial, sans-serif';
  ctx.textBaseline = 'top';
  ctx.fillStyle = '#fff';
  ctx.clearRect(0, 0, pagex, pagey);
  Module.__kkFont = { cv: cv, ctx: ctx, pagex: pagex, pagey: pagey, px: px };
  var m = ctx.measureText('Hg');
  var h = (m.actualBoundingBoxAscent + m.actualBoundingBoxDescent) || px;
  return Math.max(px, Math.round(h));
});

EM_JS(int, kkFontWidth, (const char *s, int count), {
  if (typeof document === 'undefined') return count * 8;
  var f = Module.__kkFont; if (!f) return count * 8;
  var str = UTF8ToString(s, count);
  return Math.round(f.ctx.measureText(str).width);
});

EM_JS(void, kkFontPrint, (int x, int y, const char *s, int count), {
  if (typeof document === 'undefined') return;
  var f = Module.__kkFont; if (!f) return;
  f.ctx.fillText(UTF8ToString(s, count), x, y);
});

EM_JS(void, kkFontEnd, (unsigned char *dest), {
  if (typeof document === 'undefined') return;
  var f = Module.__kkFont; if (!f) return;
  var img = f.ctx.getImageData(0, 0, f.pagex, f.pagey).data;
  var n = f.pagex * f.pagey;
  for (var i = 0; i < n; i++) {
    var a = img[i*4+3];                       // coverage
    HEAPU8[dest + i*4 + 0] = a; HEAPU8[dest + i*4 + 1] = a;
    HEAPU8[dest + i*4 + 2] = a; HEAPU8[dest + i*4 + 3] = a;
  }
  Module.__kkFont = null;
});

sInt sSystem_::FontBegin(sInt pagex,sInt pagey,const sChar *name,sInt xs,sInt ys,sInt style)
{
  delete[] FontMem;
  FontMem = new sU32[pagex*pagey];
  sSetMem(FontMem,0,pagex*pagey*4);
  gFontPageX = pagex; gFontPageY = pagey;
  sInt h = kkFontBegin(pagex,pagey,name ? name : "",xs,ys,style);
  static sInt logged; if(logged++ < 12)
    fprintf(stderr,"[kk] font begin page=%dx%d '%s' %dx%d style=%d -> h=%d\n",pagex,pagey,name?name:"",xs,ys,style,h);
  return h;
}

sInt sSystem_::FontWidth(const sChar *s,sInt count)
{
  if(count<0) count = s ? sGetStringLen(s) : 0;
  return kkFontWidth(s,count);
}

void sSystem_::FontCharWidth(sInt ch,sInt *w)
{
  sChar c[2]; c[0] = (sChar)ch; c[1] = 0;
  w[0] = 0; w[1] = kkFontWidth(c,1); w[2] = 0;       // pre, advance, post
}

void sSystem_::FontPrint(sInt x,sInt y,const sChar *s,sInt count)
{
  if(count<0) count = s ? sGetStringLen(s) : 0;
  kkFontPrint(x,y,s,count);
}

void sSystem_::FontPrint(sInt x,sInt y,const sU16 *s,sInt count)
{
  // narrow the string; the game only uses latin text
  sChar tmp[512]; sInt n = 0;
  while(s && s[n] && n < 511 && (count < 0 || n < count)) { tmp[n] = (sChar)s[n]; n++; }
  tmp[n] = 0;
  kkFontPrint(x,y,tmp,n);
}

void sSystem_::FontEnd()
{
  // FontBitmap() is read right before FontEnd() by Bitmap_Text; resolve the
  // page into FontMem now so it is there. (FontEnd frees it afterwards.)
  delete[] FontMem; FontMem = 0;
}

// Bitmap_Text reads FontBitmap() between the last FontPrint and FontEnd, so
// the page is resolved lazily on that call.
sU32 *sSystem_::FontBitmap()
{
  if(FontMem)
  {
    kkFontEnd((unsigned char *)FontMem);
    static sInt logged;
    if(logged++ < 6)
    {
      sInt n = gFontPageX*gFontPageY, lit = 0;
      for(sInt i=0;i<n;i++) if(FontMem[i] & 0xff) lit++;
      fprintf(stderr,"[kk] font page resolved: %d/%d pixels covered\n",lit,n);
    }
  }
  return FontMem;
}

/****************************************************************************/
/***                                                                      ***/
/***   sound                                                              ***/
/***                                                                      ***/
/****************************************************************************/

// Sound effects. _start.cpp keeps them in DirectSound buffers; here they are
// mixed in software on top of the music, with the same bookkeeping (a handle
// owns `buffers` voices used round-robin, SamplePlay returns a use counter
// that Sample3DParam checks against reuse) and the same volume model: linear
// gain, pan in hundredths of a dB, DirectSound's 3d rolloff
// (minDist/dist)^rolloff capped at maxDist. 3d panning is a plain left/right
// balance from the listener's orientation; there is no doppler.
struct kkSfxVoice
{
  sInt Pos;                                   // next frame to mix, -1 = idle
  sInt PlayTime;                              // GetTime() at play, 0 once stopped
  sF32 Volume,Pan;
  sBool Mode3D;                               // positioned by Sample3DParam
  sVector Pos3D;
  sF32 MinDist,MaxDist;
  sF32 GainL,GainR;
};
struct kkSfx
{
  sS16 *Data;                                 // stereo frames, mono for 3d samples
  sInt Len;
  sBool Is3D;
  sInt Count,LRU,Uses,LenMs;
  kkSfxVoice Voice[sMAXSAMPLEBUFFER];
};
static kkSfx gSfx[sMAXSAMPLEHANDLE];
static sVector gListenerPos,gListenerRight;
static sF32 gListenerRolloff = 1.0f;
static sInt gSfxTime;

static void kkSfxGain(kkSfxVoice *v)
{
  sF32 l = v->Volume, r = v->Volume;
  if(v->Mode3D)
  {
    sVector d;
    d.Sub3(v->Pos3D,gListenerPos);
    sF32 dist = d.Abs3();
    if(dist > v->MinDist && v->MinDist > 0)
    {
      sF32 att = sFPow(v->MinDist/sMin(dist,sMax(v->MaxDist,v->MinDist)),gListenerRolloff);
      l *= att; r *= att;
    }
    sF32 side = dist > 1e-4f ? d.Dot3(gListenerRight)/dist : 0.0f;
    if(side > 0) l *= 1.0f - 0.7f*side;       // source to the right: quieter left
    if(side < 0) r *= 1.0f + 0.7f*side;
  }
  else if(v->Pan > 0)
    l *= sFPow(10.0f,-v->Pan/20.0f);
  else if(v->Pan < 0)
    r *= sFPow(10.0f,v->Pan/20.0f);
  v->GainL = l;
  v->GainR = r;
}

#if !defined(KK_HEADLESS)
static void kkSfxMix(sS16 *out,sInt samples)
{
  for(sInt h=0;h<sMAXSAMPLEHANDLE;h++)
  {
    kkSfx *s = &gSfx[h];
    for(sInt b=0;b<s->Count;b++)
    {
      kkSfxVoice *v = &s->Voice[b];
      if(v->Pos < 0) continue;
      sInt n = sMin(samples,s->Len - v->Pos);
      sInt gl = sInt(v->GainL*65536.0f), gr = sInt(v->GainR*65536.0f);
      sS16 *o = out;
      if(s->Is3D)
      {
        const sS16 *src = s->Data + v->Pos;
        for(sInt i=0;i<n;i++,o+=2)
        {
          o[0] = sRange<sInt>(o[0] + ((src[i]*gl)>>16),32767,-32768);
          o[1] = sRange<sInt>(o[1] + ((src[i]*gr)>>16),32767,-32768);
        }
      }
      else
      {
        const sS16 *src = s->Data + v->Pos*2;
        for(sInt i=0;i<n;i++,o+=2)
        {
          o[0] = sRange<sInt>(o[0] + ((src[i*2+0]*gl)>>16),32767,-32768);
          o[1] = sRange<sInt>(o[1] + ((src[i*2+1]*gr)>>16),32767,-32768);
        }
      }
      v->Pos += n;
      if(v->Pos >= s->Len)
        v->Pos = -1;
    }
  }
}

static void AudioCallback(void *,Uint8 *stream,int len)
{
  sS16 *out = (sS16 *)stream;
  sInt samples = len/4;                       // 16 bit stereo
  if(gSoundHandler)
  {
    gSoundHandler(out,samples,gSoundUser);
    gSamplesPlayed += samples;
  }
  else
    sSetMem(out,0,len);
  kkSfxMix(out,samples);

  // is anything actually coming out? (the callback only runs once the page's
  // audio context is allowed to start, which needs a real user gesture)
  { static sInt calls, peak, logged;
    calls++;
    for(sInt i=0;i<samples*2;i++)
    { sInt v = out[i] < 0 ? -out[i] : out[i]; if(v > peak) peak = v; }
    if((calls % 100) == 0 && logged++ < 6)
    { fprintf(stderr,"[kk] audio: %d callbacks, peak %d/32767\n",calls,peak); peak = 0; } }
}
#endif

void sSystem_::SetSoundHandler(sSoundHandler hnd,sInt align,void *user)
{
#if !defined(KK_HEADLESS)
  if(gAudioDev) SDL_LockAudioDevice(gAudioDev);
#endif
  gSoundHandler = hnd;
  gSoundUser = user;
  gSoundAlign = align;
  gSamplesPlayed = 0;
#if !defined(KK_HEADLESS)
  if(gAudioDev)
  {
    SDL_UnlockAudioDevice(gAudioDev);
    SDL_PauseAudioDevice(gAudioDev,hnd ? 0 : 1);
  }
#endif
}

sInt sSystem_::GetCurrentSample()               { return gSamplesPlayed; }

// debug: with window.__kkDumpSamples set, every sound effect is kept as a WAV
// (cdp.js `shots:` saves it next to the stage images)
EM_JS(void, kkSaveWav, (const char *name, const short *pcm, int frames, int channels), {
  if (!window.__kkDumpSamples) return;
  var bytes = frames * channels * 2, buf = new ArrayBuffer(44 + bytes), d = new DataView(buf);
  function str(o, t) { for (var i = 0; i < t.length; i++) d.setUint8(o + i, t.charCodeAt(i)); }
  str(0, 'RIFF'); d.setUint32(4, 36 + bytes, true); str(8, 'WAVEfmt '); d.setUint32(16, 16, true);
  d.setUint16(20, 1, true); d.setUint16(22, channels, true); d.setUint32(24, 44100, true);
  d.setUint32(28, 44100 * channels * 2, true); d.setUint16(32, channels * 2, true); d.setUint16(34, 16, true);
  str(36, 'data'); d.setUint32(40, bytes, true);
  new Uint8Array(buf, 44).set(HEAPU8.subarray(pcm, pcm + bytes));
  var bin = '', u = new Uint8Array(buf);
  for (var i = 0; i < u.length; i += 0x8000) bin += String.fromCharCode.apply(null, u.subarray(i, i + 0x8000));
  (window.__kkShots = window.__kkShots || []).push([UTF8ToString(name) + '.wav', 'data:audio/wav;base64,' + btoa(bin)]);
});
static sInt kkSfxPlays;

static void kkSfxLock(sBool lock)
{
#if !defined(KK_HEADLESS)
  if(gAudioDev) { if(lock) SDL_LockAudioDevice(gAudioDev); else SDL_UnlockAudioDevice(gAudioDev); }
#endif
}

sInt sSystem_::SampleAdd(sS16 *data,sInt size,sInt buffers,sInt handle,sBool is3d)
{
  sVERIFY(buffers>=1);
  if(handle==sINVALID)
  {
    for(sInt i=0;i<sMAXSAMPLEHANDLE && handle==sINVALID;i++)
      if(gSfx[i].Count==0)
        handle = i;
  }
  else if(handle<0 || handle>=sMAXSAMPLEHANDLE || gSfx[handle].Count!=0)
    handle = sINVALID;
  if(handle==sINVALID)
    return sINVALID;

  sS16 *copy = new sS16[is3d ? size : size*2];
  if(is3d)                                    // 3d buffers are mono (downmix like _start.cpp)
    for(sInt i=0;i<size;i++)
      copy[i] = (data[i*2+0] + data[i*2+1]) >> 1;
  else
    sCopyMem(copy,data,size*4);

  kkSfxLock(sTRUE);
  kkSfx *s = &gSfx[handle];
  sSetMem(s,0,sizeof(*s));
  s->Data = copy;
  s->Len = size;
  s->Is3D = is3d;
  s->Count = sMin(buffers,sMAXSAMPLEBUFFER);
  s->LenMs = sMulDiv(size,1000,44100) + 10;
  for(sInt i=0;i<s->Count;i++)
    s->Voice[i].Pos = -1;
  kkSfxLock(sFALSE);
  { static sInt n; if(n++ < 40) fprintf(stderr,"[kk] sample %d: %d frames %s, %d buffers\n",handle,size,is3d ? "3d" : "stereo",buffers); }
  { sChar name[32]; sprintf(name,"sample%02d",handle); kkSaveWav(name,copy,size,is3d ? 1 : 2); }
  return handle;
}

void sSystem_::SampleRem(sInt handle)
{
  sVERIFY(handle>=0 && handle<sMAXSAMPLEHANDLE);
  kkSfxLock(sTRUE);
  delete[] gSfx[handle].Data;
  sSetMem(&gSfx[handle],0,sizeof(kkSfx));
  kkSfxLock(sFALSE);
}

void sSystem_::SampleRemAll()
{
  for(sInt i=0;i<sMAXSAMPLEHANDLE;i++)
    SampleRem(i);
}

sInt sSystem_::SamplePlay(sInt handle,sF32 volume,sF32 pan,sInt freq)
{
  sVERIFY(handle>=0 && handle<sMAXSAMPLEHANDLE);
  kkSfx *s = &gSfx[handle];
  if(s->Count<=0)
    return -1;

  kkSfxLock(sTRUE);
  s->LRU = (s->LRU+1)%s->Count;
  kkSfxVoice *v = &s->Voice[s->LRU];
  v->Pos = 0;
  v->Volume = sRange(volume,1.0f,0.0f);
  v->Pan = sRange<sF32>(pan,100.0f,-100.0f);
  v->Mode3D = sFALSE;                         // DS3DMODE_DISABLE until positioned
  v->PlayTime = GetTime() | 1;
  kkSfxGain(v);
  kkSfxLock(sFALSE);
  if(kkSfxPlays++ < 60) fprintf(stderr,"[kk] play sample %d vol %.2f\n",handle,volume);
  return ++s->Uses;
}

void sSystem_::Sample3DParam(sInt handle,sInt bufnum,const sVector &pos,const sVector &vel,sF32 minDist,sF32 maxDist)
{
  sVERIFY(handle>=0 && handle<sMAXSAMPLEHANDLE);
  kkSfx *s = &gSfx[handle];
  if(!s->Is3D || s->Count<=0 || s->Uses - bufnum >= s->Count)   // buffer reused since
    return;
  kkSfxVoice *v = &s->Voice[bufnum % s->Count];
  if(!v->PlayTime)
    return;
  kkSfxLock(sTRUE);
  v->Mode3D = sTRUE;
  v->Pos3D = pos;
  v->MinDist = minDist;
  v->MaxDist = maxDist;
  kkSfxGain(v);
  kkSfxLock(sFALSE);
}

void sSystem_::Sample3DListener(const sVector &pos,const sVector &vel,const sVector &up,const sVector &fwd,sF32 doppler,sF32 rolloff)
{
  gListenerPos = pos;
  gListenerRight.Cross3(up,fwd);              // left-handed: x = y cross z
  gListenerRight.UnitSafe3();
  gListenerRolloff = rolloff;
}

void sSystem_::Sample3DCommit()
{
  kkSfxLock(sTRUE);
  for(sInt i=0;i<sMAXSAMPLEHANDLE;i++)
  {
    kkSfx *s = &gSfx[i];
    for(sInt j=0;j<s->Count;j++)
    {
      kkSfxVoice *v = &s->Voice[j];
      if(v->PlayTime && v->PlayTime + s->LenMs < gSfxTime)   // "hanging" buffer: stop positioning it
      {
        v->PlayTime = 0;
        v->Mode3D = sFALSE;
      }
      if(v->Pos >= 0 && v->Mode3D)
        kkSfxGain(v);                         // the listener moved
    }
  }
  kkSfxLock(sFALSE);
  gSfxTime = GetTime();
}

/****************************************************************************/
/***                                                                      ***/
/***   textures                                                           ***/
/***                                                                      ***/
/****************************************************************************/

static void TexFormatGL(sInt format,GLenum &ifmt,GLenum &fmt,GLenum &type)
{
  switch(format)
  {
  case sTF_A8:          ifmt = GL_R8;      fmt = GL_RED;  type = GL_UNSIGNED_BYTE; break;
  case sTF_A1R5G5B5:    ifmt = GL_RGB5_A1; fmt = GL_RGBA; type = GL_UNSIGNED_SHORT_5_5_5_1; break;
  case sTF_R16F:        ifmt = GL_R16F;    fmt = GL_RED;  type = GL_HALF_FLOAT; break;
  case sTF_A2R10G10B10: ifmt = GL_RGB10_A2;fmt = GL_RGBA; type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
  case sTF_Q8W8V8U8:    ifmt = GL_RGBA8_SNORM; fmt = GL_RGBA; type = GL_BYTE; break;
  default:              ifmt = GL_RGBA8;   fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
  }
}

static sInt AllocTexHandle()
{
  for(sInt i=1;i<MAX_TEXTURE;i++)
    if(!(sSystem->Textures[i].Flags & sTIF_ALLOCATED))
      return i;
  sSystem->Abort("out of texture handles");
  return 0;
}

sInt sSystem_::AddTexture(sInt xs,sInt ys,sInt format,sU16 *data,sInt mipcount,sInt)
{
  sInt h = AllocTexHandle();
  sHardTex *t = &Textures[h];
  t->RefCount = 1;
  t->XSize = xs;
  t->YSize = ys;
  t->MipLevels = mipcount ? mipcount : 1;
  t->Format = format;
  t->Flags = sTIF_ALLOCATED;

  sGLTex *g = &gTex[h];
  glGenTextures(1,&g->Name);
  g->Target = GL_TEXTURE_2D;
  g->Fbo = 0;
  g->DepthRb = 0;
  g->Mips = sFALSE;

  GLenum ifmt,fmt,type;
  TexFormatGL(format,ifmt,fmt,type);
  glBindTexture(GL_TEXTURE_2D,g->Name);
  glTexImage2D(GL_TEXTURE_2D,0,ifmt,xs,ys,0,fmt,type,0);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);

  if(data)
    UpdateTexture(h,data,0);
  return h;
}

sInt sSystem_::AddTexture(const sTexInfo &info)
{
  sInt h = AddTexture(info.XSize,info.YSize,info.Format ? info.Format : sTF_A8R8G8B8,0);
  Textures[h].Flags |= (info.Flags & sTIF_RENDERTARGET);
  return h;
}

void sSystem_::AddRefTexture(sInt tex)
{
  if(tex>0 && tex<MAX_TEXTURE) Textures[tex].RefCount++;
}

void sSystem_::RemTexture(sInt handle)
{
  if(handle<=0 || handle>=MAX_TEXTURE) return;
  sHardTex *t = &Textures[handle];
  if(--t->RefCount > 0) return;
  sGLTex *g = &gTex[handle];
  if(g->Fbo)     glDeleteFramebuffers(1,&g->Fbo);
  if(g->DepthRb) glDeleteRenderbuffers(1,&g->DepthRb);
  if(g->Name)    glDeleteTextures(1,&g->Name);
  sSetMem(g,0,sizeof(*g));
  t->Flags = 0;
}

void sSystem_::GetTextureSize(sInt tex,sInt &w,sInt &h)
{
  w = h = 0;
  if(tex>0 && tex<MAX_TEXTURE) { w = Textures[tex].XSize; h = Textures[tex].YSize; }
}

// The generator works in 4 x signed 16 bit (0..0x7fff) per texel, matching the
// sU64 pixels of genbitmap. Convert down to 8 bit RGBA here.
void sSystem_::UpdateTexture(sInt handle,sU16 *data,sInt)
{
  if(handle<=0 || handle>=MAX_TEXTURE || !data) return;
  sHardTex *t = &Textures[handle];
  sGLTex *g = &gTex[handle];

  GLenum ifmt,fmt,type;
  TexFormatGL(t->Format,ifmt,fmt,type);

  sInt xs = t->XSize, ys = t->YSize;
  sU8 *level = new sU8[xs*ys*4];

  if(t->Format == sTF_Q8W8V8U8)
  {
    // signed normal map: lanes are b,g,r,a around 0x4000, D3D stores
    // (U,V,W,Q) = (x,y,z,a) which is the same byte order as GL's RGBA8_SNORM
    for(sInt i=0;i<xs*ys;i++)
    {
      sVector v;
      v.x = data[i*4+2] - 0x4000;
      v.y = data[i*4+1] - 0x4000;
      v.z = data[i*4+0] - 0x4000;
      v.UnitSafe3();
      level[i*4+0] = (sU8)(sInt(v.x*127.0f) & 0xff);
      level[i*4+1] = (sU8)(sInt(v.y*127.0f) & 0xff);
      level[i*4+2] = (sU8)(sInt(v.z*127.0f) & 0xff);
      level[i*4+3] = (sU8)((((sInt)data[i*4+3]-0x4000)>>7) & 0xff);
    }
  }
  else
  {
    for(sInt i=0;i<xs*ys;i++)                     // genbitmap lane order is b,g,r,a
    {
      level[i*4+0] = (sU8)(sRange<sInt>(data[i*4+2]>>7,255,0));
      level[i*4+1] = (sU8)(sRange<sInt>(data[i*4+1]>>7,255,0));
      level[i*4+2] = (sU8)(sRange<sInt>(data[i*4+0]>>7,255,0));
      level[i*4+3] = (sU8)(sRange<sInt>(data[i*4+3]>>7,255,0));
    }
  }

  {
    sU32 sum[4] = {0,0,0,0};
    for(sInt i=0;i<xs*ys;i++) for(sInt k=0;k<4;k++) sum[k] += level[i*4+k];
    for(sInt k=0;k<4;k++) g->Avg[k] = (sU8)(sum[k]/(xs*ys));
  }
  glBindTexture(GL_TEXTURE_2D,g->Name);
  glPixelStorei(GL_UNPACK_ALIGNMENT,1);
  glTexImage2D(GL_TEXTURE_2D,0,ifmt,xs,ys,0,fmt,type,level);

  // mip chain: box filter in software (glGenerateMipmap does not work for
  // the signed format the normal maps use), exactly like the d3d player did
  if(!(t->Flags & sTIF_RENDERTARGET))
  {
    sInt mip = 0;
    sBool sign = (t->Format == sTF_Q8W8V8U8);
    while(xs > 1 || ys > 1)
    {
      sInt nxs = xs>1 ? xs>>1 : 1, nys = ys>1 ? ys>>1 : 1;
      sInt sx = xs>1 ? 2 : 1, sy = ys>1 ? 2 : 1;
      sU8 *dst = new sU8[nxs*nys*4];
      for(sInt y=0;y<nys;y++)
      {
        for(sInt x=0;x<nxs;x++)
        {
          const sU8 *a = level + ((y*sy)*xs + x*sx)*4;
          const sU8 *b = a + (sx>1 ? 4 : 0);
          const sU8 *c = a + (sy>1 ? xs*4 : 0);
          const sU8 *d = c + (sx>1 ? 4 : 0);
          for(sInt k=0;k<4;k++)
          {
            sInt v;
            if(sign)
              v = (((sS8)a[k]) + ((sS8)b[k]) + ((sS8)c[k]) + ((sS8)d[k]) + 2) >> 2;
            else
              v = (a[k] + b[k] + c[k] + d[k] + 2) >> 2;
            dst[(y*nxs+x)*4+k] = (sU8)(v & 0xff);
          }
        }
      }
      delete[] level;
      level = dst;
      xs = nxs; ys = nys;
      mip++;
      glTexImage2D(GL_TEXTURE_2D,mip,ifmt,xs,ys,0,fmt,type,level);
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,mip);
    g->Mips = sTRUE;
  }
  else
  {
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    g->Mips = sFALSE;
  }

  delete[] level;
}
void sSystem_::UpdateTexture(sInt,sBitmap *)                 {}
void sSystem_::ReadTexture(sInt,sU16 *)                      {}
void sSystem_::FlushTexture(sInt)                            {}
sBool sSystem_::StreamTextureStart(sInt,sInt,sBitmapLock &)  { return sFALSE; }
void sSystem_::StreamTextureEnd()                            {}

/****************************************************************************/
/***                                                                      ***/
/***   geometry                                                           ***/
/***                                                                      ***/
/****************************************************************************/

sInt sSystem_::GeoAdd(sInt fvf,sInt prim)
{
  for(sInt i=1;i<MAX_GEOHANDLE;i++)
  {
    if(GeoHandle[i].Mode == 0)
    {
      sGeoHandle *gh = &GeoHandle[i];
      sSetMem(gh,0,sizeof(*gh));
      gh->Mode = prim | 0x8000;               // non-zero marks "in use"
      gh->FVF = fvf;
      gh->VertexSize = FVFTable[fvf].Stride;
      sSetMem(&gGeo[i],0,sizeof(sGLGeo));
      return i;
    }
  }
  Abort("out of geometry handles");
  return 0;
}

void sSystem_::GeoRem(sInt handle)
{
  if(handle<=0 || handle>=MAX_GEOHANDLE) return;
  sGLGeo *g = &gGeo[handle];
  if(g->Vao) glDeleteVertexArrays(1,&g->Vao);
  if(g->Vb)  glDeleteBuffers(1,&g->Vb);
  if(g->Ib)  glDeleteBuffers(1,&g->Ib);
  delete[] g->VStage;
  delete[] g->IStage;
  sSetMem(g,0,sizeof(*g));
  sSetMem(&GeoHandle[handle],0,sizeof(sGeoHandle));
}

void sSystem_::GeoFlush()                                    {}

void sSystem_::GeoFlush(sInt handle,sInt what)
{
  // D3D: reset the discard counters so the next GeoDraw() asks for fresh data
  if(handle<=0 || handle>=MAX_GEOHANDLE) return;
  if(what & sGEO_VERTEX) gGeo[handle].VertexValid = sFALSE;
  if(what & sGEO_INDEX)  gGeo[handle].IndexValid = sFALSE;
}

// Only the parts named in upd are (re)filled; the shadow volume path uploads
// vertices and indices in two separate Begin/End pairs.
void sSystem_::GeoBegin(sInt handle,sInt vc,sInt ic,sF32 **fp,void **ip,sInt upd)
{
  sGeoHandle *gh = &GeoHandle[handle];
  sGLGeo *g = &gGeo[handle];
  sInt idxSize = (gh->Mode & sGEO_IND32B) ? 4 : 2;

  if(upd & sGEO_VERTEX)
  {
    sInt vbytes = vc * gh->VertexSize;
    if(vbytes > g->VStageSize)
    {
      delete[] g->VStage;
      g->VStage = new sU8[vbytes ? vbytes : 1];
      g->VStageSize = vbytes;
    }
    gh->Vertex.Count = vc;
    gh->Locked |= sGEO_VERTEX;
    if(fp) *fp = (sF32 *)g->VStage;
  }
  if(upd & sGEO_INDEX)
  {
    sInt ibytes = ic * idxSize;
    if(ibytes > g->IStageSize)
    {
      delete[] g->IStage;
      g->IStage = new sU8[ibytes ? ibytes : 1];
      g->IStageSize = ibytes;
    }
    gh->Index.Count = ic;
    gh->Locked |= sGEO_INDEX;
    if(ip) *ip = g->IStage;
  }
}

void sSystem_::GeoEnd(sInt handle,sInt vc,sInt ic)
{
  sGeoHandle *gh = &GeoHandle[handle];
  sGLGeo *g = &gGeo[handle];
  sInt idxSize = (gh->Mode & sGEO_IND32B) ? 4 : 2;

  if(vc != -1) gh->Vertex.Count = vc;
  if(ic != -1) gh->Index.Count = ic;

  if(!g->Vao) glGenVertexArrays(1,&g->Vao);
  glBindVertexArray(g->Vao);

  if(gh->Locked & sGEO_VERTEX)
  {
    const sFVFDesc &d = FVFTable[gh->FVF];
    sInt n = gh->Vertex.Count;

    // D3DCOLOR elements are stored B,G,R,A; D3D's vertex fetch presented them
    // as R,G,B,A to the shader. WebGL cannot swizzle on fetch, so swap here.
    for(sInt i=0;i<d.Count;i++)
    {
      if(d.A[i].Type != 1) continue;
      sU8 *p = g->VStage + d.A[i].Offset;
      for(sInt v=0;v<n;v++,p+=d.Stride)
      {
        sU8 t = p[0]; p[0] = p[2]; p[2] = t;
      }
    }

    if(!g->Vb) glGenBuffers(1,&g->Vb);
    glBindBuffer(GL_ARRAY_BUFFER,g->Vb);
    glBufferData(GL_ARRAY_BUFFER,n*gh->VertexSize,g->VStage,GL_DYNAMIC_DRAW);
    for(sInt a=0;a<ATT_MAX;a++) glDisableVertexAttribArray(a);
    for(sInt i=0;i<d.Count;i++)
    {
      const sFVFAttr &at = d.A[i];
      glEnableVertexAttribArray(at.Attr);
      if(at.Type == 0)
        glVertexAttribPointer(at.Attr,at.Size,GL_FLOAT,GL_FALSE,d.Stride,(void *)(sDInt)at.Offset);
      else
        glVertexAttribPointer(at.Attr,at.Size,GL_UNSIGNED_BYTE,GL_TRUE,d.Stride,(void *)(sDInt)at.Offset);
    }
    g->VertexValid = sTRUE;
  }

  if(gh->Locked & sGEO_INDEX)
  {
    sInt n = gh->Index.Count;
    if(n)
    {
      if(!g->Ib) glGenBuffers(1,&g->Ib);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g->Ib);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,n*idxSize,g->IStage,GL_DYNAMIC_DRAW);
    }
    g->IndexValid = sTRUE;
  }

  glBindVertexArray(0);
  gh->Locked = 0;
  cGeoEnd++;
}

// ZFUNC EQUAL. The engine z-fills and then shades with EQUAL, relying on D3D
// giving different vertex programs identical depth; GL does not (a true
// GL_EQUAL leaves patches of walls unshaded), so the hardware test is LEQUAL.
// LEQUAL alone lets the shading passes of an alpha-tested z-fill (the spike
// strips on the wooden doors) paint through its holes onto whatever lies
// behind, as a translucent film. So before the first EQUAL draw after depth
// was written, the depth buffer is copied into a texture, and those draws
// drop fragments clearly in front of it (shader_translate.cpp).
static GLuint gZSnapTex, gZSnapFbo;
static sInt gZSnapW, gZSnapH;
static GLuint gZSnapSrc = ~0u;
static sBool gZSnapDirty = sTRUE;
static sBool gZSnapBroken[2];                             // blit refused: [0] screen, [1] render targets
static kkShaderProgram *gCurProg;                         // program of the current instance (0 = placeholder)

static sBool kkZSnapPrepare()
{
  sInt rt = gRTActive ? 1 : 0;
  if(gZSnapBroken[rt]) return sFALSE;
  GLuint src = rt ? gTex[sSystem->CurrentViewport.RenderTarget].Fbo : 0;
  sInt w = rt ? sSystem->Textures[sSystem->CurrentViewport.RenderTarget].XSize : sSystem->ConfigX;
  sInt h = rt ? sSystem->Textures[sSystem->CurrentViewport.RenderTarget].YSize : sSystem->ConfigY;
  if(!gZSnapDirty && src == gZSnapSrc && w == gZSnapW && h == gZSnapH)
    return sTRUE;

  if(!gZSnapFbo)
  {
    glGenFramebuffers(1,&gZSnapFbo);
    glGenTextures(1,&gZSnapTex);
  }
  glActiveTexture(GL_TEXTURE0 + KK_ZSNAP_UNIT);
  glBindTexture(GL_TEXTURE_2D,gZSnapTex);
  if(w != gZSnapW || h != gZSnapH)
  {
    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH24_STENCIL8,w,h,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER,gZSnapFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,gZSnapTex,0);
    gZSnapW = w; gZSnapH = h;
  }
  glActiveTexture(GL_TEXTURE0);

  while(glGetError() != GL_NO_ERROR) {}
  glBindFramebuffer(GL_READ_FRAMEBUFFER,src);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gZSnapFbo);
  if(gScissorOn) glDisable(GL_SCISSOR_TEST);                // blits are scissored
  glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
  GLenum err = glGetError();
  if(gScissorOn) glEnable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER,src);
  if(err != GL_NO_ERROR)
  {
    gZSnapBroken[rt] = sTRUE;
    fprintf(stderr,"[kk] depth snapshot blit failed (0x%x): EQUAL stays LEQUAL on %s\n",(unsigned)err,rt ? "render targets" : "the screen");
    return sFALSE;
  }
  gZSnapSrc = src;
  gZSnapDirty = sFALSE;
  return sTRUE;
}

sInt sSystem_::GeoDraw(sInt &handle)
{
  sGeoHandle *gh = &GeoHandle[handle];
  sGLGeo *gg = &gGeo[handle];
  sInt update = 0;
  if(gh->Vertex.Count == 0)         update = sGEO_VERTEX | sGEO_INDEX;
  else
  {
    if(!gg->VertexValid)            update |= sGEO_VERTEX;
    if(!gg->IndexValid && gh->Index.Count > 0) update |= sGEO_INDEX;
  }
  if(update)
  {
    cDrawEmpty++;
    return update;
  }
  cDraw++;
  KKTRACE("draw h=%d mode=%x fvf=%d vc=%d ic=%d setup=%d | sten=%d func=%d ref=%d ops=%d/%d/%d two=%d ccw=%d/%d/%d cw=%x zw=%d zf=%d cull=%d blend=%d/%d/%d op=%d at=%d/%d/%d\n",
          handle,gh->Mode,gh->FVF,gh->Vertex.Count,gh->Index.Count,CurrentSetupId,
          CurrentStates[sD3DRS_STENCILENABLE],CurrentStates[sD3DRS_STENCILFUNC],CurrentStates[sD3DRS_STENCILREF],
          CurrentStates[sD3DRS_STENCILFAIL],CurrentStates[sD3DRS_STENCILZFAIL],CurrentStates[sD3DRS_STENCILPASS],
          CurrentStates[sD3DRS_TWOSIDEDSTENCILMODE],
          CurrentStates[sD3DRS_CCW_STENCILFAIL],CurrentStates[sD3DRS_CCW_STENCILZFAIL],CurrentStates[sD3DRS_CCW_STENCILPASS],
          CurrentStates[sD3DRS_COLORWRITEENABLE],CurrentStates[sD3DRS_ZWRITEENABLE],CurrentStates[sD3DRS_ZFUNC],(sInt)gCullMode,
          CurrentStates[sD3DRS_ALPHABLENDENABLE],CurrentStates[sD3DRS_SRCBLEND],CurrentStates[sD3DRS_DESTBLEND],CurrentStates[sD3DRS_BLENDOP],
          CurrentStates[sD3DRS_ALPHATESTENABLE],CurrentStates[sD3DRS_ALPHAFUNC],CurrentStates[sD3DRS_ALPHAREF]);

  sGLGeo *g = &gGeo[handle];
  sInt prim = gh->Mode & 7;
  GLenum mode = GL_TRIANGLES;
  switch(prim)
  {
  case sGEO_LINE:   mode = GL_LINES;     break;
  case sGEO_TRI:    mode = GL_TRIANGLES; break;
  case sGEO_QUAD:   mode = GL_TRIANGLES; break;   // index buffer already expands quads
  default:          mode = GL_TRIANGLES; break;
  }

  // debug (F4): show the shadow volumes themselves (they normally only write
  // stencil), to tell a broken volume apart from a broken stencil
  sBool showSV = kkShowShadowVolumes && CurrentStates[sD3DRS_STENCILENABLE]
                 && CurrentStates[sD3DRS_COLORWRITEENABLE] == 0;
  if(showSV)
  {
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE,GL_ONE);
    glStencilFunc(GL_ALWAYS,0,0xff);
  }

  // XXX experiment: for the light passes, drop every test that could be
  // rejecting the fragments
  // debug (F2): let the shadow volumes stamp the stencil with every test out
  // of the way, to separate "not rasterised" from "rejected"
  sBool svPass = kkStencilMarkVolumes && CurrentStates[sD3DRS_COLORWRITEENABLE] == 0
                 && CurrentStates[sD3DRS_STENCILENABLE]
                 && CurrentStates[sD3DRS_STENCILFUNC] == sD3DCMP_ALWAYS;
  if(svPass)
  {
    glDepthFunc(GL_ALWAYS);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glStencilMask(0xff);
  }

  // debug (F3): drop one test at a time for the lighting passes
  sBool lightPass = kkLightTestOff && CurrentStates[sD3DRS_STENCILFUNC] == sD3DCMP_EQUAL
                    && CurrentStates[sD3DRS_ALPHABLENDENABLE]
                    && CurrentStates[sD3DRS_SRCBLEND] == sD3DBLEND_ONE
                    && CurrentStates[sD3DRS_DESTBLEND] == sD3DBLEND_ONE;
  if(lightPass)
  {
    if(kkLightTestOff == 1) glDisable(GL_SCISSOR_TEST);
    if(kkLightTestOff == 2) glDepthFunc(GL_ALWAYS);
    if(kkLightTestOff == 3) glDisable(GL_STENCIL_TEST);
  }

  // emulated ZFUNC EQUAL (see kkZSnapPrepare)
  static sInt zEqualOff, zEqualShow, zEqualPoll;            // debug: window.__kkNoZEqual = plain LEQUAL,
  if((zEqualPoll++ & 1023) == 0)                            // __kkShowZEqual = paint what it drops magenta
  { zEqualOff = kkJsFlag("__kkNoZEqual"); zEqualShow = kkJsFlag("__kkShowZEqual"); }
  sBool zEqual = gCurProg && !zEqualOff && CurrentStates[sD3DRS_ZENABLE] && CurrentStates[sD3DRS_ZFUNC] == sD3DCMP_EQUAL
                 && kkZSnapPrepare();
  if(zEqual)
  {
    glActiveTexture(GL_TEXTURE0 + KK_ZSNAP_UNIT);
    glBindTexture(GL_TEXTURE_2D,gZSnapTex);
    glActiveTexture(GL_TEXTURE0);
    kkShaderZEqual(gCurProg,zEqualShow ? 2 : 1);
  }

  glBindVertexArray(g->Vao);
  if(gh->Index.Count)
  {
    GLenum it = (gh->Mode & sGEO_IND32B) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,g->Ib);
    glDrawElements(mode,gh->Index.Count,it,0);
  }
  else if(prim == sGEO_QUAD)
  {
    sInt quads = gh->Vertex.Count/4;
    EnsureQuadIb(quads);                          // binds the element buffer into this VAO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gQuadIb);
    glDrawElements(GL_TRIANGLES,quads*6,GL_UNSIGNED_SHORT,0);
  }
  else
    glDrawArrays(mode,0,gh->Vertex.Count);
  if(zEqual)
    kkShaderZEqual(gCurProg,0);
  if(CurrentStates[sD3DRS_ZENABLE] && CurrentStates[sD3DRS_ZWRITEENABLE])
    gZSnapDirty = sTRUE;                                    // the snapshot is stale now
  if(lightPass)
  {
    if(kkLightTestOff == 1 && gScissorOn) glEnable(GL_SCISSOR_TEST);
    if(kkLightTestOff == 2) glDepthFunc(CmpGL(CurrentStates[sD3DRS_ZFUNC]));
    if(kkLightTestOff == 3 && CurrentStates[sD3DRS_STENCILENABLE]) glEnable(GL_STENCIL_TEST);
  }
  if(svPass)
  {
    glDepthFunc(CmpGL(CurrentStates[sD3DRS_ZFUNC]));
    if(gScissorOn) glEnable(GL_SCISSOR_TEST);
    ApplyCull();
  }
  if(showSV)                                      // put the real states back
  {
    sU32 cw = CurrentStates[sD3DRS_COLORWRITEENABLE];
    glColorMask((cw&1)?GL_TRUE:GL_FALSE,(cw&2)?GL_TRUE:GL_FALSE,(cw&4)?GL_TRUE:GL_FALSE,(cw&8)?GL_TRUE:GL_FALSE);
    CurrentStates[sD3DRS_ALPHABLENDENABLE] ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    glBlendFunc(BlendGL(CurrentStates[sD3DRS_SRCBLEND] ? CurrentStates[sD3DRS_SRCBLEND] : sD3DBLEND_ONE),
                BlendGL(CurrentStates[sD3DRS_DESTBLEND]));
    ApplyStencil(CurrentStates);
  }
  KKTRACEERR("draw");
  if(gTraceLeft>0 && (gh->FVF==2 || (gh->FVF==6 && (gh->Mode&0xff)==sGEO_QUAD)))   // post-processing stages, particles
  {
    sChar what[64];
    sprintf(what,"rt%d_setup%d",gRTActive ? CurrentViewport.RenderTarget : -1,CurrentSetupId);
    kkCaptureStage(what);
  }
  kkProbe("after draw");
  glBindVertexArray(0);
  return 0;
}

/****************************************************************************/
/***                                                                      ***/
/***   render states                                                      ***/
/***                                                                      ***/
/****************************************************************************/

static GLenum BlendGL(sU32 v)
{
  switch(v)
  {
  case sD3DBLEND_ZERO:            return GL_ZERO;
  case sD3DBLEND_ONE:             return GL_ONE;
  case sD3DBLEND_SRCCOLOR:        return GL_SRC_COLOR;
  case sD3DBLEND_INVSRCCOLOR:     return GL_ONE_MINUS_SRC_COLOR;
  case sD3DBLEND_SRCALPHA:        return GL_SRC_ALPHA;
  case sD3DBLEND_INVSRCALPHA:     return GL_ONE_MINUS_SRC_ALPHA;
  case sD3DBLEND_DESTALPHA:       return GL_DST_ALPHA;
  case sD3DBLEND_INVDESTALPHA:    return GL_ONE_MINUS_DST_ALPHA;
  case sD3DBLEND_DESTCOLOR:       return GL_DST_COLOR;
  case sD3DBLEND_INVDESTCOLOR:    return GL_ONE_MINUS_DST_COLOR;
  case sD3DBLEND_SRCALPHASAT:     return GL_SRC_ALPHA_SATURATE;
  default:                        return GL_ONE;
  }
}

sInt kkZEqualBias = 0;                                    // debug (F8): nudge z-equal passes forward

// D3D biases in normalised depth, GL in units of the smallest resolvable
// difference (a 24 bit buffer here). Passes that shade on top of the z-fill
// ask for ZFUNC EQUAL, which is mapped to LEQUAL; nudging them towards the
// viewer covers the cases where the shading pass ends up a hair behind.
static void ApplyDepthBias()
{
  sF32 bias  = *(const sF32 *)&sSystem->CurrentStates[sD3DRS_DEPTHBIAS];
  sF32 slope = *(const sF32 *)&sSystem->CurrentStates[sD3DRS_SLOPESCALEDEPTHBIAS];
  sF32 units = bias * 16777216.0f;
  if(kkZEqualBias && sSystem->CurrentStates[sD3DRS_ZFUNC] == sD3DCMP_EQUAL)
    units -= 32.0f;
  if(units == 0.0f && slope == 0.0f)
    glDisable(GL_POLYGON_OFFSET_FILL);
  else
  {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(slope,units);
  }
}

static GLenum BlendOpGL(sU32 v)
{
  switch(v)
  {
  case sD3DBLENDOP_SUBTRACT:    return GL_FUNC_SUBTRACT;
  case sD3DBLENDOP_REVSUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
  case sD3DBLENDOP_MIN:         return GL_MIN;
  case sD3DBLENDOP_MAX:         return GL_MAX;
  default:                      return GL_FUNC_ADD;
  }
}

static GLenum CmpGL(sU32 v)
{
  switch(v)
  {
  case sD3DCMP_NEVER:         return GL_NEVER;
  case sD3DCMP_LESS:          return GL_LESS;
  // D3D guarantees that the depth a vertex program produces is identical
  // across passes, so the engine can run its lighting passes with ZFUNC
  // EQUAL on top of a z-fill pass. GL makes no such promise between two
  // different programs, and the mismatch leaves whole surfaces unshaded, so
  // accept anything not behind the z-fill instead.
  case sD3DCMP_EQUAL:         return GL_LEQUAL;
  case sD3DCMP_LESSEQUAL:     return GL_LEQUAL;
  case sD3DCMP_GREATER:       return GL_GREATER;
  case sD3DCMP_NOTEQUAL:      return GL_NOTEQUAL;
  case sD3DCMP_GREATEREQUAL:  return GL_GEQUAL;
  default:                    return GL_ALWAYS;
  }
}

// The stencil keeps D3D's EQUAL: CmpGL's EQUAL -> LEQUAL stand-in is for the
// depth test only. For the stencil, "0 <= anything" passed everywhere, so the
// "lit where no shadow volume left a count" test never rejected anything and
// stencil shadows were missing altogether.
static GLenum StencilCmpGL(sU32 v)
{
  return v == sD3DCMP_EQUAL ? GL_EQUAL : CmpGL(v);
}

static GLenum StencilOpGL(sU32 v)
{
  switch(v)
  {
  case sD3DSTENCILOP_KEEP:    return GL_KEEP;
  case sD3DSTENCILOP_ZERO:    return GL_ZERO;
  case sD3DSTENCILOP_REPLACE: return GL_REPLACE;
  case sD3DSTENCILOP_INCRSAT: return GL_INCR;
  case sD3DSTENCILOP_DECRSAT: return GL_DECR;
  case sD3DSTENCILOP_INVERT:  return GL_INVERT;
  case sD3DSTENCILOP_INCR:    return GL_INCR_WRAP;
  case sD3DSTENCILOP_DECR:    return GL_DECR_WRAP;
  default:                    return GL_KEEP;
  }
}

void sSystem_::SetState(sU32 token,sU32 value)
{
  if(token < 0x100)                                       // render state
  {
    CurrentStates[token] = value;
    // material state blocks are absolute and mostly identical from draw to
    // draw; every GL call below is a call out of wasm, so drop the no-ops
    if(gAppliedStates[token] == value)
      { cStateSkip++; return; }
    gAppliedStates[token] = value;
    cStateSet++;
    switch(token)
    {
    case sD3DRS_ZENABLE:        value ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST); break;
    case sD3DRS_ZWRITEENABLE:   glDepthMask(value ? GL_TRUE : GL_FALSE); break;
    case sD3DRS_ZFUNC:
      glDepthFunc(CmpGL(value));
      ApplyDepthBias();                                   // z-equal passes may want a nudge
      break;
    case sD3DRS_ALPHABLENDENABLE: value ? glEnable(GL_BLEND) : glDisable(GL_BLEND); break;
    case sD3DRS_SRCBLEND:
    case sD3DRS_DESTBLEND:
      glBlendFunc(BlendGL(CurrentStates[sD3DRS_SRCBLEND] ? CurrentStates[sD3DRS_SRCBLEND] : sD3DBLEND_ONE),
                  BlendGL(CurrentStates[sD3DRS_DESTBLEND]));
      break;
    case sD3DRS_BLENDOP:        glBlendEquation(BlendOpGL(value)); break;
    case sD3DRS_CULLMODE:
      gCullMode = value;
      ApplyCull();
      break;
    case sD3DRS_COLORWRITEENABLE:
      glColorMask((value&1)?GL_TRUE:GL_FALSE,(value&2)?GL_TRUE:GL_FALSE,
                  (value&4)?GL_TRUE:GL_FALSE,(value&8)?GL_TRUE:GL_FALSE);
      break;
    case sD3DRS_STENCILENABLE:  value ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST); break;
    case sD3DRS_STENCILFUNC:
    case sD3DRS_STENCILREF:
    case sD3DRS_STENCILMASK:
    case sD3DRS_STENCILFAIL:
    case sD3DRS_STENCILZFAIL:
    case sD3DRS_STENCILPASS:
    case sD3DRS_TWOSIDEDSTENCILMODE:
    case sD3DRS_CCW_STENCILFUNC:
    case sD3DRS_CCW_STENCILFAIL:
    case sD3DRS_CCW_STENCILZFAIL:
    case sD3DRS_CCW_STENCILPASS:
      ApplyStencil(CurrentStates);
      break;
    case sD3DRS_STENCILWRITEMASK: glStencilMask(value); break;
    case sD3DRS_DEPTHBIAS:
    case sD3DRS_SLOPESCALEDEPTHBIAS:
      ApplyDepthBias();
      break;
    default: break;                                       // fixed function leftovers
    }
  }
  else if(token >= 0x200 && token < 0x300)                // sampler state
  {
    CurrentStates[token] = value;                         // applied at bind time
  }
}

void sSystem_::SetStates(sU32 *stream)
{
  if(!stream) return;
  // state blocks are absolute: a material that blends normally does not spell
  // out BLENDOP, so reset it here instead of inheriting the last subtract
  if(CurrentStates[sD3DRS_BLENDOP] != sD3DBLENDOP_ADD)
  {
    CurrentStates[sD3DRS_BLENDOP] = sD3DBLENDOP_ADD;
    gAppliedStates[sD3DRS_BLENDOP] = sD3DBLENDOP_ADD;
    glBlendEquation(GL_FUNC_ADD);
  }
  while(stream[0] != ~0U)
  {
    SetState(stream[0],stream[1]);
    stream += 2;
  }
}

// The current viewport in D3D coordinates (the rectangle Clear() is limited
// to; D3D clears the viewport, GL clears everything unless scissored).
static sInt gVpRect[4];

// D3D rectangles are top-down inside the target. On screen that has to be
// flipped into GL's bottom-up window space; render targets are already drawn
// flipped (see gRTActive), so there the mapping is the identity.
static void ScissorD3D(sInt x0,sInt y0,sInt x1,sInt y1)
{
  if(gRTActive)
    glScissor(x0,y0,x1-x0,y1-y0);
  else
    glScissor(x0,sSystem->ConfigY-y1,x1-x0,y1-y0);
}

void sSystem_::SetScissor(const sFRect *r)
{
  if(!r) { glDisable(GL_SCISSOR_TEST); gScissorOn = sFALSE; return; }

  // the rectangle is in clip space (-1..1, y up); the d3d layer turned it
  // into a viewport-relative pixel rect, top-down
  sInt vx0 = gVpRect[0], vy0 = gVpRect[1];
  sF32 vw = (sF32)(gVpRect[2] - gVpRect[0]), vh = (sF32)(gVpRect[3] - gVpRect[1]);
  sInt x0 = vx0 + (sInt)(vw * (1.0f + r->x0) * 0.5f);
  sInt x1 = vx0 + (sInt)(vw * (1.0f + r->x1) * 0.5f);
  sInt y0 = vy0 + (sInt)(vh * (1.0f - r->y1) * 0.5f);      // top
  sInt y1 = vy0 + (sInt)(vh * (1.0f - r->y0) * 0.5f);      // bottom

  glEnable(GL_SCISSOR_TEST);
  gScissorOn = sTRUE;
  ScissorD3D(x0,y0,x1,y1);
}

/****************************************************************************/
/***                                                                      ***/
/***   viewport / clear / present                                         ***/
/***                                                                      ***/
/****************************************************************************/

void sSystem_::SetViewport(const sViewport &vp)
{
  CurrentViewport = vp;
  cViewport++;
  KKTRACE("viewport rt=%d win=%d,%d-%d,%d\n",vp.RenderTarget,vp.Window.x0,vp.Window.y0,vp.Window.x1,vp.Window.y1);
  if(vp.RenderTarget >= 0 && vp.RenderTarget < MAX_TEXTURE)
  {
    sGLTex *g = &gTex[vp.RenderTarget];
    sHardTex *t = &Textures[vp.RenderTarget];
    if(!g->Fbo)
    {
      glGenFramebuffers(1,&g->Fbo);
      glBindFramebuffer(GL_FRAMEBUFFER,g->Fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,g->Name,0);
      glGenRenderbuffers(1,&g->DepthRb);
      glBindRenderbuffer(GL_RENDERBUFFER,g->DepthRb);
      glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,t->XSize,t->YSize);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,g->DepthRb);
      {
        GLint sbits = 0, dbits = 0;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE,&sbits);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,&dbits);
        fprintf(stderr,"[kk] rendertarget %d: %dx%d depth=%d stencil=%d status=0x%x\n",
                vp.RenderTarget,t->XSize,t->YSize,(int)dbits,(int)sbits,
                (unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER));
      }
    }
    else
      glBindFramebuffer(GL_FRAMEBUFFER,g->Fbo);
    // D3D viewports are sub-rectangles of the target; the game renders into
    // an 800x400 corner of a 1024x512 texture and samples accordingly.
    sInt x0 = vp.Window.x0, y0 = vp.Window.y0;
    sInt x1 = vp.Window.x1 ? vp.Window.x1 : t->XSize;
    sInt y1 = vp.Window.y1 ? vp.Window.y1 : t->YSize;
    ViewportX = x1 - x0;
    ViewportY = y1 - y0;
    gVpRect[0] = x0; gVpRect[1] = y0; gVpRect[2] = x1; gVpRect[3] = y1;
    // y is flipped while rendering to textures (see gRTActive), so the D3D
    // top-left rectangle maps to the same texel rows in GL
    glViewport(x0,y0,ViewportX,ViewportY);
    gRTActive = sTRUE;
  }
  else
  {
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    ViewportX = vp.Window.XSize() ? vp.Window.XSize() : ConfigX;
    ViewportY = vp.Window.YSize() ? vp.Window.YSize() : ConfigY;
    gVpRect[0] = vp.Window.x0; gVpRect[1] = vp.Window.y0;
    gVpRect[2] = gVpRect[0] + ViewportX; gVpRect[3] = gVpRect[1] + ViewportY;
    glViewport(gVpRect[0],ConfigY - gVpRect[3],ViewportX,ViewportY);
    gRTActive = sFALSE;
  }
  ApplyCull();
  glDisable(GL_SCISSOR_TEST);
  gScissorOn = sFALSE;
}

void sSystem_::Clear(sU32 flags,sU32 color)
{
  cClear++;
  KKTRACE("clear flags=%x color=%08x\n",flags,color);
  GLbitfield m = 0;
  if(flags & sVCF_COLOR)
  {
    m |= GL_COLOR_BUFFER_BIT;
    glClearColor(((color>>16)&0xff)/255.0f,((color>>8)&0xff)/255.0f,
                 (color&0xff)/255.0f,((color>>24)&0xff)/255.0f);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  }
  if(flags & sVCF_Z)       { m |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); gZSnapDirty = sTRUE; }
  if(flags & sVCF_STENCIL) { m |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xff); }
  if(m)
  {
    // D3D clears the viewport rectangle (further limited by an active scissor
    // rect), GL clears the whole buffer - so scissor it down
    if(!gScissorOn)
    {
      glEnable(GL_SCISSOR_TEST);
      ScissorD3D(gVpRect[0],gVpRect[1],gVpRect[2],gVpRect[3]);
    }
    glClear(m);
    if(!gScissorOn)
      glDisable(GL_SCISSOR_TEST);
  }

  // D3D's Clear leaves the render states alone; put the masks back
  sU32 cw = CurrentStates[sD3DRS_COLORWRITEENABLE];
  glColorMask((cw&1)?GL_TRUE:GL_FALSE,(cw&2)?GL_TRUE:GL_FALSE,(cw&4)?GL_TRUE:GL_FALSE,(cw&8)?GL_TRUE:GL_FALSE);
  glDepthMask(CurrentStates[sD3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
  glStencilMask(CurrentStates[sD3DRS_STENCILWRITEMASK]);
  kkProbe("after clear");
}

void sSystem_::GrabScreen(sInt,const sRect &,sInt,sBool) {}
void sSystem_::Begin2D(sU32 **buffer,sInt &stride)       { if(buffer) *buffer = 0; stride = 0; }
void sSystem_::End2D()                                   {}
void sSystem_::SetGamma(sF32)                            {}

sBool sSystem_::GetScreenInfo(sInt i,sScreenInfo &info)
{
  sSetMem(&info,0,sizeof(info));
  if(i != 0) return sFALSE;
  info.XSize = ConfigX;
  info.YSize = ConfigY;
  info.FullScreen = sFALSE;
  info.LowQuality = sFALSE;
  info.ShaderLevel = sPS_11;               // material11 path -- see genmaterial.cpp
  info.PixelRatio = 1.0f;
  return sTRUE;
}

sInt sSystem_::GetScreenCount()                          { return 1; }
sBool sSystem_::GetFullscreen()                          { return sFALSE; }
void sSystem_::Reset(sU32,sInt,sInt,sInt,sInt)           {}

void sSystem_::GetTransform(sInt mode,sMatrix &mat)
{
  switch(mode)
  {
  case sGT_VIEW:
    mat = LastCamera;
    break;

  case sGT_MODELVIEW:                             // camera axes in model space:
    mat = LastMatrix;                             // billboards are built from
    mat.TransR();                                 // mat.i / mat.j, so this has
    mat.MulA(mat,LastCamera);                     // to match _start.cpp exactly
    break;

  case sGT_MODEL:
    mat = LastMatrix;
    break;

  case sGT_PROJECT:
    mat = LastProjection;
    break;

  default:
    mat.Init();
    break;
  }
}

/****************************************************************************/
/***                                                                      ***/
/***   materials                                                          ***/
/***                                                                      ***/
/****************************************************************************/

static sU32 *CopyTokens(const sU32 *src)
{
  if(!src) return 0;
  sInt n = 0;
  while(src[n] != 0x0000ffff && n < 65536) n++;
  n++;
  sU32 *d = new sU32[n];
  sCopyMem(d,src,n*4);
  return d;
}

static sU32 *CopyStates(const sU32 *src)
{
  if(!src) return 0;
  sInt n = 0;
  while(src[n] != ~0U && n < 4096) n += 2;
  n += 2;
  sU32 *d = new sU32[n];
  sCopyMem(d,src,n*4);
  return d;
}

sInt sSystem_::MtrlAddSetup(const sU32 *states,const sU32 *vs,const sU32 *ps)
{
  for(sInt i=1;i<MAX_SETUPS;i++)
  {
    sMaterialSetup *su = &Setups[i];
    if(su->RefCount == 0 && su->States == 0)
    {
      su->RefCount = 1;
      su->States = CopyStates(states);
      su->VSCode = CopyTokens(vs);
      su->PSCode = CopyTokens(ps);
      gSetup[i].Prog = 0;
      gSetup[i].Tried = sFALSE;
      return i;
    }
  }
  Abort("out of material setups");
  return sINVALID;
}

void sSystem_::MtrlAddRefSetup(sInt sid)
{
  if(sid>0 && sid<MAX_SETUPS) Setups[sid].AddRef();
}

void sSystem_::MtrlRemSetup(sInt sid)
{
  if(sid<=0 || sid>=MAX_SETUPS) return;
  sMaterialSetup *su = &Setups[sid];
  if(--su->RefCount > 0) return;
  delete[] su->States; su->States = 0;
  delete[] su->VSCode; su->VSCode = 0;
  delete[] su->PSCode; su->PSCode = 0;
  if(gSetup[sid].Prog) glDeleteProgram(gSetup[sid].Prog);
  gSetup[sid].Prog = 0;
  gSetup[sid].Tried = sFALSE;
  kkShaderForget(sid);
}

void sSystem_::MtrlClearCaches()
{
  sSetMem(gAppliedStates,0xff,sizeof(gAppliedStates));    // nothing known about GL any more
  CurrentSetupId = sINVALID;
  LastDecl = LastVB = LastIB = -1;
  MtrlReset = sTRUE;
}

void sSystem_::MtrlSetSetup(sInt sid)
{
  if(sid<=0 || sid>=MAX_SETUPS) return;
  CurrentSetupId = sid;
  SetStates(Setups[sid].States);
  cSetup++;
  KKTRACE("setup %d\n",sid);
  // the program is picked in MtrlSetInstance, once the sampler types are known
}

static GLenum MagFilterGL(sU32 f)
{
  return f == sD3DTEXF_POINT ? GL_NEAREST : GL_LINEAR;
}

static GLenum MinFilterGL(sU32 minf,sU32 mipf,sBool hasMips)
{
  sBool point = (minf == sD3DTEXF_POINT);
  if(!hasMips || mipf == sD3DTEXF_NONE)
    return point ? GL_NEAREST : GL_LINEAR;
  if(mipf == sD3DTEXF_POINT)
    return point ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
  return point ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
}

static GLenum AddressGL(sU32 a)
{
  switch(a)
  {
  case sD3DTADDRESS_CLAMP:
  case sD3DTADDRESS_BORDER:     return GL_CLAMP_TO_EDGE;
  case sD3DTADDRESS_MIRROR:
  case sD3DTADDRESS_MIRRORONCE: return GL_MIRRORED_REPEAT;
  default:                      return GL_REPEAT;
  }
}

void sSystem_::MtrlSetInstance(const sMaterialInstance &inst)
{
  // bind textures, remembering each stage's sampler type
  sU8 samplerTypes[16];
  sInt hasTex = 0;
  for(sInt i=0;i<16;i++) samplerTypes[i] = KKSAMP_2D;

  for(sInt i=0;i<inst.NumTextures && i<MAX_TEXSTAGE && i<16;i++)
  {
    sInt tex = inst.Textures[i];
    GLenum target = GL_TEXTURE_2D;
    GLuint name = 0;
    sBool hasMips = sFALSE;

    if(tex>0 && tex<MAX_TEXTURE && gTex[tex].Name)
    {
      name = gTex[tex].Name;
      // IPP render targets are plain textures drawn into through an FBO and
      // never get a mip chain: asking for mipmapped filtering makes them
      // incomplete, and GL then samples black (every render target copied to
      // the screen came out black)
      hasMips = gTex[tex].Mips;
      if(i==0) hasTex = 1;
    }
    else if(tex == -2)  { name = gNormalCube;        target = GL_TEXTURE_CUBE_MAP; samplerTypes[i] = KKSAMP_CUBE; }
    else if(tex == -3)  { name = gAttenuationVolume; target = GL_TEXTURE_3D;       samplerTypes[i] = KKSAMP_VOLUME; }

#if defined(__EMSCRIPTEN__)
    // sampling the texture we are rendering into is a feedback loop: WebGL
    // drops the draw entirely, so find out where the game does this
    if(tex > 0 && tex == CurrentViewport.RenderTarget)
    {
      static sU32 seen[32]; static sInt seenCount = 0;
      sU32 id = (sU32)((CurrentSetupId<<8) | i);
      sBool isNew = sTRUE;
      for(sInt si=0;si<seenCount;si++) if(seen[si] == id) isNew = sFALSE;
      if(isNew && seenCount < 32)
      {
        seen[seenCount++] = id;
        fprintf(stderr,"[kk] feedback: setup=%d stage=%d tex=%d == rendertarget\n",CurrentSetupId,i,tex);
      }
    }
#endif
    glActiveTexture(GL_TEXTURE0+i);
    if(!name) { glBindTexture(GL_TEXTURE_2D,0); continue; }
    glBindTexture(target,name);

    // per-stage sampler state, from the material's state block
    const sU32 *ss = &CurrentStates[0x200 + i*16];
    glTexParameteri(target,GL_TEXTURE_MAG_FILTER,MagFilterGL(ss[5]));
    glTexParameteri(target,GL_TEXTURE_MIN_FILTER,MinFilterGL(ss[6],ss[7],hasMips));
    glTexParameteri(target,GL_TEXTURE_WRAP_S,AddressGL(ss[1]));
    glTexParameteri(target,GL_TEXTURE_WRAP_T,AddressGL(ss[2]));
    if(target == GL_TEXTURE_3D)
      glTexParameteri(target,GL_TEXTURE_WRAP_R,AddressGL(ss[3]));
  }

  // pick (or build) the translated program for this setup + sampler types
  kkShaderProgram *prog = 0;
  if(CurrentSetupId > 0 && CurrentSetupId < MAX_SETUPS)
  {
    sMaterialSetup *su = &Setups[CurrentSetupId];
    prog = kkShaderGet(CurrentSetupId,su->VSCode,su->PSCode,samplerTypes);
  }

  KKTRACE("instance prog=%u ntex=%d tex=%d,%d,%d,%d vs=%d ps=%d\n",kkShaderProgramId(prog),inst.NumTextures,
          inst.NumTextures>0?inst.Textures[0]:-9,inst.NumTextures>1?inst.Textures[1]:-9,inst.NumTextures>2?inst.Textures[2]:-9,inst.NumTextures>3?inst.Textures[3]:-9,
          inst.NumVSConstants,inst.NumPSConstants);
  KKTRACEERR("bind textures");
  if(gTraceLeft>0 && prog && kkShouldDumpSetup(CurrentSetupId))
    kkShaderDump(prog,CurrentSetupId,inst.VSConstants,inst.NumVSConstants,inst.PSConstants,inst.NumPSConstants);
  if(gTraceLeft>0 && inst.NumTextures>=2 && inst.Textures[0]>0 && inst.Textures[1]>0
     && inst.Textures[0]<MAX_TEXTURE && inst.Textures[1]<MAX_TEXTURE && gTex[inst.Textures[0]].Fbo && gTex[inst.Textures[1]].Fbo)
  {
    // debug: both inputs of a merge / mask
    sChar what[32]; sprintf(what,"in_setup%d",CurrentSetupId);
    kkCaptureTexture(inst.Textures[0],what);
    kkCaptureTexture(inst.Textures[1],what);
  }
  if(gTraceLeft>0 && (CurrentStates[sD3DRS_SRCBLEND]==sD3DBLEND_DESTCOLOR || (samplerTypes[1]==KKSAMP_CUBE && CurrentStates[sD3DRS_ALPHABLENDENABLE])))
  {
    // debug: the texture phase (fb = 2*src*fb) and the per-pixel light passes
    static sInt dumps, ldumps;
    sBool isLight = samplerTypes[1]==KKSAMP_CUBE;
    if(isLight ? ldumps++ < 1 : dumps++ < 1)
    {
      kkShaderDump(prog,CurrentSetupId,inst.VSConstants,inst.NumVSConstants,inst.PSConstants,inst.NumPSConstants);
      for(sInt i=0;i<inst.NumTextures && i<4;i++)
      {
        sInt tex = inst.Textures[i];
        if(tex>0 && tex<MAX_TEXTURE)
          fprintf(stderr,"[kk] tex%d = %d fmt=%d %dx%d avg rgba=%d,%d,%d,%d\n",i,tex,Textures[tex].Format,
                  Textures[tex].XSize,Textures[tex].YSize,gTex[tex].Avg[0],gTex[tex].Avg[1],gTex[tex].Avg[2],gTex[tex].Avg[3]);
        else
          fprintf(stderr,"[kk] tex%d = %d\n",i,tex);
      }
      for(sInt i=0;i<4;i++)
      {
        const sU32 *ss = &CurrentStates[0x200 + i*16];
        fprintf(stderr,"[kk] stage%d addr=%d/%d mag=%d min=%d mip=%d\n",i,ss[1],ss[2],ss[5],ss[6],ss[7]);
      }
    }
  }
  gCurProg = prog;
  if(prog)
  {
    cInstT++;
    kkShaderUse(prog,inst.VSConstants,inst.NumVSConstants,inst.PSConstants,inst.NumPSConstants,
                CurrentStates[sD3DRS_ALPHATESTENABLE] && !kkAlphaTestOff ? (sInt)CurrentStates[sD3DRS_ALPHAFUNC] : 8,
                CurrentStates[sD3DRS_ALPHAREF] / 255.0f);
  }
  else
  {
    cInstP++;
    // untranslatable or fixed-function setup: stand-in shader
    glUseProgram(gPlaceholderProg);
    glUniformMatrix4fv(gLocViewProject,1,GL_FALSE,(const GLfloat *)&gViewProject);
    glUniform1i(gLocTex0,0);
    glUniform1i(gLocHasTex,hasTex);
  }
  KKTRACEERR("instance");
}

// Cube map of unit direction vectors (the "normalizer" ps.1.1 shaders sample
// instead of normalising per pixel) and a spherical attenuation volume, both
// transcribed from _start.cpp.
static void MakeBuiltinTextures()
{
  const sInt size = 64;
  static const sVector faces[6][2] =
  {
    {{ 0, 0,-1},{ 0, 1, 0}},
    {{ 0, 0, 1},{ 0, 1, 0}},
    {{ 1, 0, 0},{ 0, 0,-1}},
    {{ 1, 0, 0},{ 0, 0, 1}},
    {{ 1, 0, 0},{ 0, 1, 0}},
    {{-1, 0, 0},{ 0, 1, 0}}
  };
  sU8 *buf = new sU8[size*size*4];

  glGenTextures(1,&gNormalCube);
  glBindTexture(GL_TEXTURE_CUBE_MAP,gNormalCube);
  for(sInt i=0;i<6;i++)
  {
    sU8 *p = buf;
    for(sInt y=0;y<size;y++)
    {
      for(sInt x=0;x<size;x++)
      {
        sVector v;
        v.Cross3(faces[i][0],faces[i][1]);
        v.Scale3((size-1)*0.5f);
        v.AddScale3(faces[i][0],(x-(size-1)*0.5f));
        v.AddScale3(faces[i][1],(-y+(size-1)*0.5f));
        v.Unit3();
        p[0] = (sU8)sFtol(128.0f+v.x*127);      // stored R,G,B,A for GL (D3D kept B,G,R,A)
        p[1] = (sU8)sFtol(128.0f+v.y*127);
        p[2] = (sU8)sFtol(128.0f+v.z*127);
        p[3] = 0;
        p += 4;
      }
    }
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,GL_RGBA8,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,buf);
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  delete[] buf;

  const sInt vsize = 32;
  const sF32 scale = 2.0f / (vsize - 2.0f);
  const sF32 mid = vsize / 2.0f;
  sU8 *vol = new sU8[vsize*vsize*vsize*4];
  sU8 *p = vol;
  for(sInt z=0;z<vsize;z++)
  {
    sF32 vz = (z - mid) * scale;
    for(sInt y=0;y<vsize;y++)
    {
      sF32 vy = (y - mid) * scale;
      for(sInt x=0;x<vsize;x++)
      {
        sF32 vx = (x - mid) * scale;
        sF32 attn = sMax(1.0f - (vx*vx+vy*vy+vz*vz),0.0f);
        p[0] = p[1] = p[2] = p[3] = (sU8)sFtol(attn * 255);
        p += 4;
      }
    }
  }
  glGenTextures(1,&gAttenuationVolume);
  glBindTexture(GL_TEXTURE_3D,gAttenuationVolume);
  glTexImage3D(GL_TEXTURE_3D,0,GL_RGBA8,vsize,vsize,vsize,0,GL_RGBA,GL_UNSIGNED_BYTE,vol);
  glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
  delete[] vol;
}

static void MakeGLProjectionMatrix(const sMaterialEnv *env,sMatrix &pmat);

void sSystem_::SetViewProject(const sMaterialEnv *env)
{
  if(!env) return;
  gEnv = *env;
  gHaveEnv = sTRUE;

  sMatrix proj;
  MakeGLProjectionMatrix(env,proj);

  sMatrix view = env->CameraSpace;
  view.TransR();                                  // world -> camera

  LastCamera = view;
  LastMatrix = env->ModelSpace;
  LastProjection = proj;

  // LastViewProject is world -> clip: the materials build their own
  // world-view-projection as ModelSpace * LastViewProject, so the model
  // matrix must NOT be folded in here.
  LastViewProject.Mul4(view,proj);

  // the stand-in program for untranslatable setups draws with a complete
  // model-view-projection instead
  sMatrix mv;
  mv.Mul4(env->ModelSpace,view);
  gViewProject.Mul4(mv,proj);
}


/****************************************************************************/
/***                                                                      ***/
/***   portable helpers that happen to live in _start.cpp                 ***/
/***                                                                      ***/
/****************************************************************************/

void sTexInfo::InitRT(sInt xs,sInt ys)
{
  XSize = xs;
  YSize = ys;
  Flags = sTIF_RENDERTARGET;
  Format = 0;
}

void sTexInfo::Init(sBitmap *bm,sInt format,sU32 flags)
{
  XSize = 0; YSize = 0; Bitmap = bm; Format = format; Flags = flags;
}

void sMaterialEnv::Init()
{
  sSetMem(this,0,sizeof(*this));
  ModelSpace.Init();
  CameraSpace.Init();
  NearClip = 0.125f;
  FarClip = 4096.0f;
  ZoomX = 1.0f;
  ZoomY = 1.0f;
  CenterX = 0.0f;
  CenterY = 0.0f;
  FogStart = 0;
  FogEnd = FarClip;
  FogColor = 0xff808080;
}

void sMaterialEnv::MakeProjectionMatrix(sMatrix &pmat) const
{
  sF32 q;
  sF32 shiftX = 1.0f / sSystem->ViewportX;
  sF32 shiftY = 1.0f / sSystem->ViewportY;

  switch(Orthogonal)
  {
  case sMEO_PIXELS:                                       // 0 .. screen_max
    pmat.i.Init(2.0f/sSystem->ViewportX,0,0,0);
    pmat.j.Init(0,-2.0f/sSystem->ViewportY,0,0);
    pmat.k.Init(0,0,1,0);
    pmat.l.Init(-1 - (2*CenterX+1)*shiftX,1 + (2*CenterY+1)*shiftY,0,1);
    break;

  case sMEO_NORMALISED:
    pmat.i.Init(ZoomX ,0     ,0            ,0);
    pmat.j.Init(0     ,ZoomY ,0            ,0);
    pmat.k.Init(0     ,0     ,1.0f/FarClip ,0);
    pmat.l.Init(-shiftX,shiftY,0           ,1);
    break;

  case sMEO_PERSPECTIVE:
  default:
    q = 1.0f;
    pmat.i.Init(ZoomX  ,0      ,0          ,0);
    pmat.j.Init(0      ,ZoomY  ,0          ,0);
    pmat.k.Init(CenterX,CenterY,q          ,1);
    pmat.l.Init(-shiftX,shiftY ,-q*NearClip,0);
    break;
  }

}

// The projection above is the one the engine also uses for frustum culling and
// portal visibility, so it has to stay in D3D conventions. Only the copy that
// goes to the GPU gets adapted to GL.
static void MakeGLProjectionMatrix(const sMaterialEnv *env,sMatrix &pmat)
{
  env->MakeProjectionMatrix(pmat);

  // undo D3D9's half-pixel shift (-1/ViewportX, +1/ViewportY in every mode):
  // D3D9 samples pixel centres at integer coordinates, GL at +0.5, so in GL
  // the shift put every full-screen copy half a texel off - bilinear then
  // averaged 2x2 texels, and the ~10 copies of the post-processing chain
  // blurred the whole picture
  pmat.l.x += 1.0f / sSystem->ViewportX;
  pmat.l.y -= 1.0f / sSystem->ViewportY;

  // GL clip space: z in [-w,w] instead of D3D's [0,w]
  pmat.i.z = 2*pmat.i.z - pmat.i.w;
  pmat.j.z = 2*pmat.j.z - pmat.j.w;
  pmat.k.z = 2*pmat.k.z - pmat.k.w;
  pmat.l.z = 2*pmat.l.z - pmat.l.w;

  // render targets are rendered upside down so they end up D3D-oriented
  if(gRTActive)
  {
    pmat.i.y = -pmat.i.y;
    pmat.j.y = -pmat.j.y;
    pmat.k.y = -pmat.k.y;
    pmat.l.y = -pmat.l.y;
  }
}

sMaterial::~sMaterial() {}

void sMaterialSetup::Cleanup()
{
  delete[] States; States = 0;
  delete[] VSCode; VSCode = 0;
  delete[] PSCode; PSCode = 0;
}

sMaterialInstance sMaterialInstance::Null = { 0 };

void sViewport::Init(sInt screen)
{
  Screen = screen;
  RenderTarget = -1;
  Window.Init(0,0,sSystem->Screen[screen].XSize,sSystem->Screen[screen].YSize);
}

void sViewport::InitTex(sInt handle)
{
  Screen = -1;
  RenderTarget = handle;
  Window.x0 = 0;
  Window.y0 = 0;
  sSystem->GetTextureSize(handle,Window.x1,Window.y1);
}

/****************************************************************************/

sSimpleMaterial::sSimpleMaterial(sInt tex,sU32 flags,sU32 flags2,sU32 color)
{
  Tex = tex;
  Color = color;
  Setup = sINVALID;
  if(tex != sINVALID)
    sSystem->AddRefTexture(tex);
  (void)flags; (void)flags2;
}

sSimpleMaterial::~sSimpleMaterial()
{
  if(Tex != sINVALID) sSystem->RemTexture(Tex);
  if(Setup != sINVALID) sSystem->MtrlRemSetup(Setup);
}

void sSimpleMaterial::SetTex(sInt tex)
{
  if(tex != Tex)
  {
    if(Tex != sINVALID) sSystem->RemTexture(Tex);
    Tex = tex;
    if(Tex != sINVALID) sSystem->AddRefTexture(Tex);
  }
}

void sSimpleMaterial::Set(const sMaterialEnv &env)
{
  sSystem->SetViewProject(&env);
  sMaterialInstance inst;
  sSetMem(&inst,0,sizeof(inst));
  inst.NumTextures = 1;
  inst.Textures[0] = Tex;
  sSystem->MtrlSetInstance(inst);
}

/****************************************************************************/
/***                                                                      ***/
/***   loading progress                                                   ***/
/***                                                                      ***/
/****************************************************************************/

EM_JS(int, kkPageHidden, (), {
  return (typeof document !== 'undefined' && document.hidden) ? 1 : 0;
});

void sSystem_::Progress(sInt done,sInt max)
{
  // A hidden tab clamps timers to a second, and every yield below costs one
  // of those, so generating in the background would take minutes. Yield far
  // less often while nobody is looking.
  static double last;
  double now = emscripten_get_now();
  double interval = kkPageHidden() ? 1000.0 : 60.0;
  if(now < last + interval && done != max)
    return;
  last = now;

  glBindFramebuffer(GL_FRAMEBUFFER,0);
  glViewport(0,0,ConfigX,ConfigY);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.05f,0.05f,0.07f,1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // A scissor-rect progress bar: no shader needed, so this works before any
  // material exists.
  sInt w = ConfigX - 40, h = 24;
  sInt y = ConfigY/2 - h/2;
  glEnable(GL_SCISSOR_TEST);
  glScissor(20,y,w,h);
  glClearColor(0.15f,0.15f,0.18f,1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  sInt fill = max>0 ? (sInt)((sS64)w*done/max) : 0;
  glScissor(20,y,fill,h);
  glClearColor(0.90f,0.55f,0.10f,1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);

  emscripten_sleep(0);          // hand the frame to the browser (ASYNCIFY)
}

void sSystem_::WaitForKey() {}

/****************************************************************************/
/***                                                                      ***/
/***   main loop                                                          ***/
/***                                                                      ***/
/****************************************************************************/

#if !defined(KK_HEADLESS)
static void PumpEvents()
{
  SDL_Event e;
  while(SDL_PollEvent(&e))
  {
    switch(e.type)
    {
    case SDL_QUIT:
      sSystem->Exit();
      break;

    case SDL_MOUSEMOTION:
      gMouseDX += e.motion.xrel;
      gMouseDY += e.motion.yrel;
      gMouseX = e.motion.x;
      gMouseY = e.motion.y;
      break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      {
        // pointer lock needs a user gesture, so (re-)request it on a click
        // inside the canvas rather than at startup
        if(e.type == SDL_MOUSEBUTTONDOWN && !SDL_GetRelativeMouseMode())
          SDL_SetRelativeMouseMode(SDL_TRUE);
        sU32 bit = 0;
        if(e.button.button == SDL_BUTTON_LEFT)   bit = 1;
        if(e.button.button == SDL_BUTTON_RIGHT)  bit = 2;
        if(e.button.button == SDL_BUTTON_MIDDLE) bit = 4;
        if(e.type == SDL_MOUSEBUTTONDOWN) sSystem->MouseButtons |= bit;
        else                              sSystem->MouseButtons &= ~bit;
        // the d3d layer also posted buttons as keys (WM_LBUTTONDOWN ->
        // sKEY_MOUSEL); the game fires on those, not on MouseButtons
        sU32 key = bit==1 ? sKEY_MOUSEL : bit==2 ? sKEY_MOUSER : bit==4 ? sKEY_MOUSEM : 0;
        if(key && sSystem->KeyIndex < MAX_KEYBUFFER)
          sSystem->KeyBuffer[sSystem->KeyIndex++] = key | (e.type == SDL_MOUSEBUTTONUP ? sKEYQ_BREAK : 0);
      }
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
      {
        sU32 key = 0;
        if(e.key.keysym.sym == SDLK_F9 && e.type == SDL_KEYDOWN) { gTraceLeft = 6000; break; }  // debug: trace the next frame
        if(e.key.keysym.sym == SDLK_F10 && e.type == SDL_KEYDOWN) { kkExecTrace = 1; break; }   // debug: log the next frame's op execution
        if(e.key.keysym.sym == SDLK_u && e.type == SDL_KEYDOWN)                                // debug: draw one material index only
        { extern sInt kkOnlyMtrl; kkOnlyMtrl = (kkOnlyMtrl+1)%17; fprintf(stderr,"[kk] only mtrl: %d\n",kkOnlyMtrl); break; }
        if(e.key.keysym.sym == SDLK_g && e.type == SDL_KEYDOWN)                                // debug: dump next frame's base jobs
        { extern sInt kkDumpJobs; kkDumpJobs = 1; break; }
        if(e.key.keysym.sym == SDLK_h && e.type == SDL_KEYDOWN)                                // debug: engine frustum culling off
        { extern sInt kkNoFrustumCull; kkNoFrustumCull = !kkNoFrustumCull; fprintf(stderr,"[kk] frustum cull off: %d\n",kkNoFrustumCull); break; }
        if(e.key.keysym.sym == SDLK_j && e.type == SDL_KEYDOWN)                                // debug: culling off / inverted
        { kkCullDebug = (kkCullDebug+1)%3; ApplyCull(); fprintf(stderr,"[kk] cull debug: %d\n",kkCullDebug); break; }
        if(e.key.keysym.sym == SDLK_k && e.type == SDL_KEYDOWN)                                // debug: alpha test off
        { kkAlphaTestOff = !kkAlphaTestOff; fprintf(stderr,"[kk] alpha test off: %d\n",kkAlphaTestOff); break; }
        if(e.key.keysym.sym == SDLK_n && e.type == SDL_KEYDOWN)                                // debug: light shader term views
        { extern sInt kkLightDebugView; kkLightDebugView = (kkLightDebugView+1)%7;
          fprintf(stderr,"[kk] light debug view: %d\n",kkLightDebugView); break; }
        if(e.key.keysym.sym == SDLK_b && e.type == SDL_KEYDOWN)                                // debug: force the specular alpha the light passes write
        { extern sF32 kkForceLightAlpha; kkForceLightAlpha = kkForceLightAlpha > 0.0f ? 0.0f : 0.5f;
          fprintf(stderr,"[kk] force light alpha: %.2f\n",kkForceLightAlpha); break; }
        if(e.key.keysym.sym == SDLK_v && e.type == SDL_KEYDOWN)                                // debug: brighten the lighting passes
        { extern sF32 kkLightBoost; kkLightBoost = kkLightBoost >= 4.0f ? 1.0f : kkLightBoost*2.0f;
          fprintf(stderr,"[kk] light boost: %.1f\n",kkLightBoost); break; }
        if((e.key.keysym.sym == SDLK_F1 || e.key.keysym.sym == SDLK_c) && e.type == SDL_KEYDOWN) // debug: swap red/blue on every shader output
        { extern sInt kkSwizzleOutput; kkSwizzleOutput = !kkSwizzleOutput;
          fprintf(stderr,"[kk] output r/b swapped: %d\n",kkSwizzleOutput); break; }
        if(e.key.keysym.sym == SDLK_F2 && e.type == SDL_KEYDOWN)                               // debug: stamp shadow volumes into the stencil
        { kkStencilMarkVolumes = !kkStencilMarkVolumes; sSystem->MtrlClearCaches();
          fprintf(stderr,"[kk] stencil mark volumes: %d\n",kkStencilMarkVolumes); break; }
        if(e.key.keysym.sym == SDLK_F3 && e.type == SDL_KEYDOWN)                               // debug: which test kills the light passes?
        { kkLightTestOff = (kkLightTestOff+1)%4; fprintf(stderr,"[kk] light test off: %d\n",kkLightTestOff); break; }
        if(e.key.keysym.sym == SDLK_F4 && e.type == SDL_KEYDOWN)                               // debug: show shadow volumes
        { kkShowShadowVolumes = !kkShowShadowVolumes; fprintf(stderr,"[kk] show shadow volumes: %d\n",kkShowShadowVolumes); break; }
        if(e.key.keysym.sym == SDLK_F5 && e.type == SDL_KEYDOWN)                               // debug: invert the shadow stencil test
        { kkInvertStencil = !kkInvertStencil; sSystem->MtrlClearCaches(); fprintf(stderr,"[kk] invert stencil: %d\n",kkInvertStencil); break; }
        if(e.key.keysym.sym == SDLK_F6 && e.type == SDL_KEYDOWN)                               // debug: all passes / base only / lighting only
        { extern sInt kkUsageFilter; kkUsageFilter = (kkUsageFilter+1)%3; fprintf(stderr,"[kk] pass filter: %d\n",kkUsageFilter); break; }
        if(e.key.keysym.sym == SDLK_F7 && e.type == SDL_KEYDOWN)                               // debug: shadows / no shadows / no lights
        { extern sInt kkCycleShadows(); fprintf(stderr,"[kk] shadow mode: %d\n",kkCycleShadows()); break; }
        if(e.key.keysym.sym == SDLK_F8 && e.type == SDL_KEYDOWN)                               // debug: z-equal depth nudge on/off
        { kkZEqualBias = !kkZEqualBias; fprintf(stderr,"[kk] z-equal bias: %d\n",kkZEqualBias); break; }
        if(e.key.keysym.sym == SDLK_F11 && e.type == SDL_KEYDOWN)                              // debug: portal visibility on/off
        { kkPaintAllSectors = !kkPaintAllSectors; fprintf(stderr,"[kk] paint all sectors: %d\n",kkPaintAllSectors); break; }
        switch(e.key.keysym.sym)
        {
        case SDLK_ESCAPE: key = sKEY_ESCAPE; break;
        case SDLK_RETURN: key = sKEY_ENTER;  break;
        case SDLK_SPACE:  key = sKEY_SPACE;  break;
        case SDLK_TAB:    key = sKEY_TAB;    break;
        case SDLK_UP:     key = sKEY_UP;     break;
        case SDLK_DOWN:   key = sKEY_DOWN;   break;
        case SDLK_LEFT:   key = sKEY_LEFT;   break;
        case SDLK_RIGHT:  key = sKEY_RIGHT;  break;
        default:
          if(e.key.keysym.sym >= 32 && e.key.keysym.sym < 127)
            key = (sU32)e.key.keysym.sym;
          break;
        }
        if(!key) break;
        if(e.type == SDL_KEYUP) key |= sKEYQ_BREAK;
        { static sInt klog; if(klog++ < 24) fprintf(stderr,"[kk] key %s sym=%d -> %08x\n",e.type==SDL_KEYDOWN?"down":"up  ",(int)e.key.keysym.sym,(unsigned)key); }
        if(sSystem->KeyIndex < MAX_KEYBUFFER)
          sSystem->KeyBuffer[sSystem->KeyIndex++] = key;
      }
      break;
    }
  }
}

#endif // !KK_HEADLESS

static void RunFrame();
static void MainLoop()
{
  if(!gInitDone)
    return;
#if !defined(KK_HEADLESS)
  PumpEvents();
#endif
  RunFrame();
}

// The back buffer has an alpha channel because the lighting passes keep the
// specular term in destination alpha. D3D presented it as opaque; a WebGL
// canvas is composited with its alpha, so whatever the passes left there
// (0, the texture phase's 0.5, specular) made the page darken or discolour
// the picture depending on the browser's compositor. Force alpha to 1
// before the frame is handed over.
static sInt kkAlphaLog = 3;
static void kkOpaqueBackbuffer()
{
  glBindFramebuffer(GL_FRAMEBUFFER,0);
  if(kkAlphaLog > 0 && gTraceLeft > 0)
  {
    // debug: what alpha the frame ended with (centre and a corner)
    sU8 a[4], b[4];
    glReadPixels(sSystem->ConfigX/2,sSystem->ConfigY/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,a);
    glReadPixels(sSystem->ConfigX/4,sSystem->ConfigY/4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,b);
    if(gTraceLeft > 0) { kkAlphaLog--; fprintf(stderr,"[kk] frame end rgba centre=%d,%d,%d,%d quarter=%d,%d,%d,%d\n",a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3]); }
  }
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
  glClearColor(0,0,0,1);
  glClear(GL_COLOR_BUFFER_BIT);
  sU32 cw = sSystem->CurrentStates[sD3DRS_COLORWRITEENABLE];
  glColorMask((cw&1)?GL_TRUE:GL_FALSE,(cw&2)?GL_TRUE:GL_FALSE,(cw&4)?GL_TRUE:GL_FALSE,(cw&8)?GL_TRUE:GL_FALSE);
  if(gScissorOn) glEnable(GL_SCISSOR_TEST);
}


static void RunFrame()
{

  for(sInt i=0;i<sSystem->KeyIndex;i++)
    sAppHandler(sAPPCODE_KEY,sSystem->KeyBuffer[i]);
  sSystem->KeyIndex = 0;

  if(sSystem->WAborting)
  {
    sAppHandler(sAPPCODE_EXIT,0);
    emscripten_cancel_main_loop();
    // "exit .kkrieger": the page takes over (shell.html goes back to its
    // start screen); without that hook the last frame would just stay up
    EM_ASM({ if(Module.kkExit) setTimeout(Module.kkExit, 0); });
    return;
  }

  sViewport vp;
  vp.Init();
  sSystem->SetViewport(vp);
  if(gTraceInFrames > 0 && --gTraceInFrames == 0) gTraceLeft = 6000;
  KKTRACE("---- frame %d ----\n",gFrame);
  sAppHandler(sAPPCODE_PAINT,0);
  kkExecTrace = 0;
  kkProbe("frame end");
  kkOpaqueBackbuffer();
  gFrame++;
  if(gFrame % 120 == 0)
  {
    GLenum e = glGetError();
    fprintf(stderr,"[kk] frame %d: viewport=%d clear=%d setup=%d inst(translated=%d placeholder=%d) draw=%d empty=%d geoend=%d glerr=0x%x\n",
            gFrame,cViewport,cClear,cSetup,cInstT,cInstP,cDraw,cDrawEmpty,cGeoEnd,(unsigned)e);
    fprintf(stderr,"[kk]   states applied=%d skipped=%d\n",cStateSet,cStateSkip);
    cViewport=cClear=cSetup=cInstT=cInstP=cDraw=cDrawEmpty=cGeoEnd=0;
    cStateSet=cStateSkip=0;
  }
}

// kkriegergame.cpp switches resolution from the options menu (Switches
// [KGS_RESOLUTION]) by writing ConfigX/Y and calling this - resize the canvas
// to match, so viewports, render targets and the master viewport stay in sync.
void sSystem_::InitScreens()
{
  WScreenCount = 1;
  Screen[0].XSize = ConfigX;
  Screen[0].YSize = ConfigY;
  ViewportX = ConfigX;
  ViewportY = ConfigY;
#if !defined(KK_HEADLESS)
  if(gWindow)
    SDL_SetWindowSize(gWindow,ConfigX,ConfigY);
  glViewport(0,0,ConfigX,ConfigY);
#endif
  fprintf(stderr,"[kk] screen: %dx%d\n",ConfigX,ConfigY);
}

void sSystem_::InitX()
{
  gStartTicks = kkTicks();
  sSetRndSeed(gStartTicks & 0x7fffffff);
  sInitTypes();

  WScreenCount = 1;
  Screen[0].XSize = ConfigX;
  Screen[0].YSize = ConfigY;
  ViewportX = ConfigX;
  ViewportY = ConfigY;
  CpuMask = 0;
  GpuMask = 0;
  CmdShaderLevel = sPS_11;

  sSetMem(GeoBuffer,0,sizeof(GeoBuffer));
  sSetMem(GeoHandle,0,sizeof(GeoHandle));
  for(sInt i=0;i<MAX_TEXTURE;i++)
    Textures[i].Flags = 0;
  sSetMem(Setups,0,sizeof(Setups));
  MtrlClearCaches();
  CurrentStates[sD3DRS_COLORWRITEENABLE] = 0xf;      // D3D defaults
  CurrentStates[sD3DRS_BLENDOP] = sD3DBLENDOP_ADD;
  CurrentStates[sD3DRS_ZWRITEENABLE] = 1;
  CurrentStates[sD3DRS_STENCILWRITEMASK] = 0xffffffff;
  CurrentStates[sD3DRS_STENCILMASK] = 0xffffffff;

  gPlaceholderProg = LinkProgram(kPlaceholderVS,kPlaceholderPS);
  if(gPlaceholderProg)
  {
    gLocViewProject = glGetUniformLocation(gPlaceholderProg,"uViewProject");
    gLocTex0        = glGetUniformLocation(gPlaceholderProg,"uTex0");
    gLocHasTex      = glGetUniformLocation(gPlaceholderProg,"uHasTex");
  }
  gViewProject.Init();
  MakeBuiltinTextures();

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);

#if !defined(KK_HEADLESS)
  {
    int dw=0,dh=0; SDL_GL_GetDrawableSize(gWindow,&dw,&dh);
    printf("[kk] config %dx%d, drawable %dx%d\n",ConfigX,ConfigY,dw,dh);
  }
#endif
  printf("[kk] init done, running sAPPCODE_INIT (procedural generation)\n");
  double t0 = emscripten_get_now();
  sAppHandler(sAPPCODE_INIT,0);
  printf("[kk] generation finished in %.1f s\n",(emscripten_get_now()-t0)/1000.0);

  gInitDone = sTRUE;
#if !defined(KK_HEADLESS)
  emscripten_set_main_loop(MainLoop,0,0);
#else
  // Drive the game by wall-clock time: the intro hands over to the menu on
  // its own, Enter is pressed periodically until the level root (2) is up,
  // then a few hundred more frames are run.
  extern KDoc *Document;
  sInt frames = 0, lastEnter = 0, inLevelFrames = 0;
  while(frames < 20000 && !WAborting)
  {
    sInt t = GetTime();
    if(Document && Document->CurrentRoot == 2)
    {
      if(++inLevelFrames > 300) break;
    }
    else if(t > 12000 && t - lastEnter > 3000)
    {
      lastEnter = t;
      KeyBuffer[KeyIndex++] = sKEY_ENTER;
      KeyBuffer[KeyIndex++] = sKEY_ENTER | sKEYQ_BREAK;
      fprintf(stderr,"[kk] headless: Enter at %d ms (root %d)\n",t,Document?Document->CurrentRoot:-1);
    }
    RunFrame();
    frames++;
    if(t > 120000) break;
  }
  fprintf(stderr,"[kk] headless: done after %d frames, root %d, %d level frames\n",frames,Document?Document->CurrentRoot:-1,inLevelFrames);
  emscripten_force_exit(0);
#endif
}

/****************************************************************************/

#if defined(KK_HEADLESS)
int main(int argc,char **argv)
{
  if(argc > 1)
    sCopyString(gCmdLine,argv[1],sizeof(gCmdLine));
  sSetConfig(sSF_DIRECT3D,1024,576);
  sAppHandler(sAPPCODE_CONFIG,0);
  sSystem->ConfigX = 1024; sSystem->ConfigY = 576;
  sSystem->InitX();
  return 0;
}
#else
int main(int argc,char **argv)
{
  if(argc > 1)
    sCopyString(gCmdLine,argv[1],sizeof(gCmdLine));

  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) != 0)
  {
    printf("[kk] SDL_Init failed: %s\n",SDL_GetError());
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);      // WebGL2 / GLES3
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,8);                 // the engine blends against destination alpha
                                                            // (specular accumulation), so the back buffer
                                                            // needs a real alpha channel

  sSetConfig(sSF_DIRECT3D,1024,576);
  if(!sAppHandler(sAPPCODE_CONFIG,0))
    sSetConfig(sSF_DIRECT3D,1024,576);
  sSystem->ConfigFlags &= ~sSF_FULLSCREEN;                  // never in a browser tab

  // the page's resolution choice replaces the game's 640..1280 switch
  // (kkriegergame.cpp) and sizes the full-screen render targets (genoverlay.cpp)
  kkForcedResX = kkPageRes(0) & ~1;
  kkForcedResY = kkPageRes(1) & ~1;
  if(kkForcedResX >= 320 && kkForcedResY >= 200 && kkForcedResX <= 8192 && kkForcedResY <= 8192)
  {
    sSystem->ConfigX = kkForcedResX;
    sSystem->ConfigY = kkForcedResY;
    printf("[kk] resolution from the page: %dx%d\n",kkForcedResX,kkForcedResY);
  }
  else
    kkForcedResX = kkForcedResY = 0;

  gWindow = SDL_CreateWindow(".kkrieger",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,
                             sSystem->ConfigX,sSystem->ConfigY,SDL_WINDOW_OPENGL);
  if(!gWindow) { printf("[kk] SDL_CreateWindow failed: %s\n",SDL_GetError()); return 1; }
  gGL = SDL_GL_CreateContext(gWindow);
  if(!gGL) { printf("[kk] SDL_GL_CreateContext failed: %s\n",SDL_GetError()); return 1; }
  SDL_SetRelativeMouseMode(SDL_TRUE);         // mouse look: pointer lock; the browser only grants it on a
                                              // user gesture, so PumpEvents asks again on the first click

  SDL_AudioSpec want,have;
  SDL_memset(&want,0,sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 2048;
  want.callback = AudioCallback;
  gAudioDev = SDL_OpenAudioDevice(0,0,&want,&have,0);
  if(!gAudioDev)
    printf("[kk] audio unavailable: %s\n",SDL_GetError());

  sSystem->InitX();
  return 0;
}
#endif // !KK_HEADLESS
