/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Packs 12-bit 4:2:2 YCbCr and the metadata bits into the 8-bit carrier.
// Output texels hold the carrier bytes B, G, R, A from the LSB up.

Texture2D<uint> gY : register(t0);
Texture2D<uint> gCb : register(t1);
Texture2D<uint> gCr : register(t2);
RWTexture2D<uint> gOut : register(u0);
cbuffer Params : register(b0)
{
  uint gW;
  uint gH;
  uint gBits; // packet count * 3072
  uint gTopLeft; // chroma sited at the top left of its luma rows, else left
  uint4 gMask; // left, top, right, bottom
  uint4 gPackets[40];
};

// A 4:2:0 chroma sample of a row clamped to the picture, which takes no
// chroma from outside it.
int Tap(uint x, int row)
{
  int lo = (int)gMask.y / 2;
  int hi = (gMask.w > gMask.y ? (int)gMask.w : (int)gH) / 2 - 1;
  int3 at = int3(x >> 1, clamp(row, lo, hi), 0);
  return (int)((x & 1) ? gCr.Load(at) : gCb.Load(at));
}

// 4:2:0 to 4:2:2 per ETSI GS CCM 001 Annex B.
uint Chroma(uint x, uint y)
{
  int r = (int)(y >> 1);
  int c;
  if (gTopLeft)
  {
    if ((y & 1) == 0)
      return Tap(x, r);
    c = (22 * (Tap(x, r - 3) + Tap(x, r + 4)) + 94 * (Tap(x, r - 2) + Tap(x, r + 3)) -
         524 * (Tap(x, r - 1) + Tap(x, r + 2)) + 2456 * (Tap(x, r) + Tap(x, r + 1)) + 2048) >> 12;
  }
  else if ((y & 1) == 0)
    c = (2 * Tap(x, r - 3) - 12 * Tap(x, r - 2) + 65 * Tap(x, r - 1) + 222 * Tap(x, r) -
         25 * Tap(x, r + 1) + 4 * Tap(x, r + 2) + 128) >> 8;
  else
    c = (4 * Tap(x, r - 2) - 25 * Tap(x, r - 1) + 222 * Tap(x, r) + 65 * Tap(x, r + 1) -
         12 * Tap(x, r + 2) + 2 * Tap(x, r + 3) + 128) >> 8;
  return (uint)clamp(c, 0, 4095);
}

uint PayloadBit(uint p)
{
  uint b = p % 1024;
  uint index = (p / 3072) * 128 + b / 8;
  uint word = gPackets[index >> 4][(index >> 2) & 3];
  return (word >> (8 * (index & 3) + 7 - b % 8)) & 1;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gW || id.y >= gH)
    return;
  uint x = id.x;
  uint y = id.y;
  uint luma = gY.Load(int3(id.xy, 0));
  uint chroma = Chroma(x, y);

  if (x < gMask.x || y < gMask.y || x >= gMask.z || y >= gMask.w)
  {
    luma = 0;
    chroma = 2048;
  }

  uint p = y * gW + x;
  if (p < gBits)
  {
    chroma &= ~1u;
    chroma |= PayloadBit(p) ^ ((countbits(luma) + countbits(chroma)) & 1);
  }

  uint blue = ((chroma & 15) << 4) | (luma & 15);
  gOut[id.xy] = blue | ((luma >> 4) << 8) | ((chroma >> 4) << 16) | 0xFF000000;
}
