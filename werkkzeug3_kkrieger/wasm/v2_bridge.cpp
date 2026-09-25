// C bridge between the kkrieger side (which uses _types.hpp) and the modern
// V2 synth in ../v2 (which brings its own, ABI-different typedefs). Keeping
// the two type systems in separate translation units is the whole point of
// this file; see wasm/v2_shim.cpp for the other half.
//
// The song data embedded in kkrieger.k is in the 2004 .v2m layout; the
// portable core expects the current one, so Open() runs it through
// ConvertV2M first (v2mconv.cpp knows every version delta).
//
// This file is distributed under a BSD license. See LICENSE.txt for details.

#include "v2mplayer.h"
#include "v2mconv.h"
// sounddef.h defines its tables in the header. The const ones get internal
// linkage and are fine; v2sources (an array of pointers) does not, and
// v2mconv.cpp already owns a copy -- rename ours out of the way.
#define v2sources v2sources_bridge_local
#include "sounddef.h"
#undef v2sources
#include <stdio.h>
#include <string.h>

/****************************************************************************/
/***                                                                      ***/
/***   Version tables                                                     ***/
/***                                                                      ***/
/****************************************************************************/
//
// v2mconv.cpp needs to know, for every synth version, how many patch and
// global parameters existed. The editor computes these in sounddef.cpp
// (sdInit), which also drags in the whole VSTi UI; this is the same loop
// on its own.

int  v2version;
int *v2vsizes;
int *v2gsizes;

static void InitVersionTables()
{
  if(v2vsizes) return;

  v2version = 0;
  for(int i=0;i<v2nparms;i++)
    if(v2parms[i].version > v2version) v2version = v2parms[i].version;
  for(int i=0;i<v2ngparms;i++)
    if(v2gparms[i].version > v2version) v2version = v2gparms[i].version;

  v2vsizes = new int[v2version+1];
  v2gsizes = new int[v2version+1];
  memset(v2vsizes,0,(v2version+1)*sizeof(int));
  memset(v2gsizes,0,(v2version+1)*sizeof(int));

  for(int i=0;i<v2nparms;i++)
    for(int j=v2version;j>=v2parms[i].version;j--)
      v2vsizes[j]++;
  for(int i=0;i<v2ngparms;i++)
    for(int j=v2version;j>=v2gparms[i].version;j--)
      v2gsizes[j]++;

  for(int i=0;i<=v2version;i++)
    v2vsizes[i] += 1+255*3;               // patch header + 255 x 3 byte modulations
}

struct KKV2
{
  V2MPlayer Player;
  unsigned char *Converted;
  int Open;
};

extern "C"
{

void *kkv2_create()
{
  KKV2 *h = new KKV2;
  memset(h,0,sizeof(*h));
  // Play() positions are samples: the 2004 player (CV2MPlayer::Play at
  // 0x807f11 in the unpacked beta) compares its argument straight against
  // the sample counter, and the sound-effect table (VFX0) stores sample
  // offsets. At the later default of 1000 ticks/s every effect was cut from
  // silence 44 times further into the tune.
  h->Player.Init(44100);
  return h;
}

void kkv2_destroy(void *vh)
{
  KKV2 *h = (KKV2 *)vh;
  if(!h) return;
  if(h->Open) h->Player.Close();
  delete[] h->Converted;
  delete h;
}

int kkv2_open(void *vh,const void *v2m,int len,unsigned int samplerate)
{
  KKV2 *h = (KKV2 *)vh;
  if(!h || !v2m) return 0;
  if(h->Open) { h->Player.Close(); h->Open = 0; }
  delete[] h->Converted; h->Converted = 0;

  InitVersionTables();

  const void *play = v2m;
  if(len > 0)
  {
    int delta = CheckV2MVersion((const unsigned char *)v2m,len);
    if(delta < 0)
    {
      printf("[kk] v2m: unrecognised file (%d)\n",delta);
      return 0;
    }
    if(delta > 0)
    {
      int outlen = 0;
      ConvertV2M((const unsigned char *)v2m,len,&h->Converted,&outlen);
      if(!h->Converted) { printf("[kk] v2m: conversion failed\n"); return 0; }
      printf("[kk] v2m: converted %d -> %d bytes (version delta %d)\n",len,outlen,delta);
      play = h->Converted;
    }
  }
  else
    printf("[kk] v2m: no length given, assuming current format\n");

  h->Open = h->Player.Open(play,samplerate) ? 1 : 0;
  return h->Open;
}

void kkv2_close(void *vh)
{
  KKV2 *h = (KKV2 *)vh;
  if(h && h->Open) { h->Player.Close(); h->Open = 0; }
}

void kkv2_play(void *vh,unsigned int smp)           { KKV2 *h=(KKV2*)vh; if(h && h->Open) h->Player.Play(smp); }
void kkv2_stop(void *vh,unsigned int fade)          { KKV2 *h=(KKV2*)vh; if(h && h->Open) h->Player.Stop(fade); }

int kkv2_render(void *vh,float *buf,unsigned int len)
{
  KKV2 *h = (KKV2 *)vh;
  if(!h || !h->Open) { memset(buf,0,len*2*sizeof(float)); return 0; }
  h->Player.Render(buf,len,0);
  return 1;
}

} // extern "C"
