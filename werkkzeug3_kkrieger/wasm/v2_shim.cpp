// kkrieger-side half of the V2 bridge: implements the minimal CV2MPlayer that
// _viruz2.hpp declares for WebAssembly builds on top of wasm/v2_bridge.cpp.
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#include "_types.hpp"
#include "_viruz2.hpp"

extern "C"
{
  void *kkv2_create();
  void  kkv2_destroy(void *h);
  int   kkv2_open(void *h,const void *v2m,int len,unsigned int samplerate);
  void  kkv2_close(void *h);
  void  kkv2_play(void *h,unsigned int ms);
  void  kkv2_stop(void *h,unsigned int fade);
  int   kkv2_render(void *h,float *buf,unsigned int len);
}

// Length of the v2m blob about to be opened. CV2MPlayer::Open() never took a
// size (the x86 player parsed until it was done), but the format converter
// needs one; mainplayer.cpp sets this right before each Open() call.
sInt CV2MPlayerNextLength = 0;

CV2MPlayer::CV2MPlayer()                              { m_impl = kkv2_create(); }
CV2MPlayer::~CV2MPlayer()                             { kkv2_destroy(m_impl); m_impl = 0; }

sBool CV2MPlayer::Open(const void *a_v2mptr,sU32 a_samplerate)
{
  sInt len = CV2MPlayerNextLength;
  CV2MPlayerNextLength = 0;
  return kkv2_open(m_impl,a_v2mptr,len,a_samplerate) ? sTRUE : sFALSE;
}

void  CV2MPlayer::Close()                             { kkv2_close(m_impl); }
void  CV2MPlayer::Play(sU32 a_time)                   { kkv2_play(m_impl,a_time); }
void  CV2MPlayer::Stop(sU32 a_fadetime)               { kkv2_stop(m_impl,a_fadetime); }
sBool CV2MPlayer::Render(sF32 *a_buffer,sU32 a_len)   { return kkv2_render(m_impl,a_buffer,a_len) ? sTRUE : sFALSE; }
