/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// Composed 12-bit chroma: polynomial or MMR prediction plus the residual.
// MMR runs in double-float (float2 hi+lo) so GPUs without native binary64
// compute shader support (such as Intel Iris Xe) execute the full 3rd-order
// MMR polynomial accurately.

#include "dovi_el_horizontal.hlsl"

Texture2D<uint> gBlY : register(t0);
Texture2D<uint> gBlCb : register(t1);
Texture2D<uint> gBlCr : register(t2);
Texture2D<uint> gElCb : register(t3);
Texture2D<uint> gElCr : register(t4);
Buffer<int> gTables : register(t5);
RWTexture2D<uint> gOutCb : register(u0);
RWTexture2D<uint> gOutCr : register(u1);
cbuffer Params : register(b0)
{
  uint gChromaW;
  uint gChromaH;
  uint gLumaW;
  uint gUseEl;
  uint gElW;
  uint gMmrMask; // bit 1: Cb is MMR, bit 2: Cr is MMR
  uint gOrderCb;
  uint gOrderCr;
  uint4 gPivotLow;
  uint4 gPivotHigh;
  uint4 gScale; // xy: float2(hi, lo) of 2^-(denom + 4)
  // Per chroma component 22 float2(hi, lo) pairs: constant * 2^20, then a[0..20].
  uint4 gMmr[22];
};

// floor(a * b / 2^20) for a, b < 2^20 without 64-bit integers.
uint MulShr20(uint a, uint b)
{
  uint p = (a >> 10) * b;
  return (p >> 10) + ((((p & 1023) << 10) + (a & 1023) * b) >> 20);
}

float2 QuickTwoSum(float a, float b)
{
  float s = a + b;
  float e = b - (s - a);
  return float2(s, e);
}

float2 TwoSum(float a, float b)
{
  float s = a + b;
  float v = s - a;
  float e = (a - (s - v)) + (b - v);
  return float2(s, e);
}

float2 DfAdd(float2 a, float2 b)
{
  float2 s = TwoSum(a.x, b.x);
  float2 t = TwoSum(a.y, b.y);
  s.y += t.x;
  s = QuickTwoSum(s.x, s.y);
  s.y += t.y;
  return QuickTwoSum(s.x, s.y);
}

float2 Split(float a)
{
  float c = 4097.0f * a;
  float aBig = c - a;
  float aHi = c - aBig;
  float aLo = a - aHi;
  return float2(aHi, aLo);
}

float2 TwoProd(float a, float b)
{
  float p = a * b;
  float2 sa = Split(a);
  float2 sb = Split(b);
  float err = ((sa.x * sb.x - p) + sa.x * sb.y + sa.y * sb.x) + sa.y * sb.y;
  return float2(p, err);
}

float2 DfMulScalar(float2 a, float b)
{
  float2 p = TwoProd(a.x, b);
  p.y += a.y * b;
  return QuickTwoSum(p.x, p.y);
}

float2 DfMul(float2 a, float2 b)
{
  float2 p = TwoProd(a.x, b.x);
  p.y += a.x * b.y + a.y * b.x;
  return QuickTwoSum(p.x, p.y);
}

float2 Coef(uint base, uint d)
{
  uint4 v = gMmr[base + (d >> 1)];
  return (d & 1) ? float2(asfloat(v.z), asfloat(v.w)) : float2(asfloat(v.x), asfloat(v.y));
}

int MmrQ16(uint base, uint order, uint3 x)
{
  uint f[21];
  f[0] = x.x * 1024;
  f[1] = x.y * 1024;
  f[2] = x.z * 1024;
  f[3] = x.x * x.y;
  f[4] = x.x * x.z;
  f[5] = x.y * x.z;
  f[6] = MulShr20(f[3], f[2]);
  f[7] = x.x * x.x;
  f[8] = x.y * x.y;
  f[9] = x.z * x.z;
  [unroll] for (uint k = 3; k <= 6; ++k)
    f[7 + k] = MulShr20(f[k], f[k]);
  [unroll] for (uint m = 0; m <= 6; ++m)
    f[14 + m] = MulShr20(f[m], f[m + 7]);

  float2 a = Coef(base, 0);
  uint used = 7 * order;
  [unroll] for (uint t = 0; t < 21; ++t)
  {
    if (t < used)
      a = DfAdd(a, DfMulScalar(Coef(base, t + 1), (float)f[t]));
  }
  if (a.x <= 0.0f && (a.x < 0.0f || a.y <= 0.0f))
    return 0;
  float2 scale = float2(asfloat(gScale.x), asfloat(gScale.y));
  float2 v = DfMul(a, scale);
  float vf = v.x + v.y;
  if (vf >= 65535.0f)
    return 65535;
  if (vf <= 0.0f)
    return 0;
  uint q = (uint)vf;
  float rem = (v.x - (float)q) + v.y;
  if (rem < 0.0f && q > 0)
    q -= 1;
  else if (rem >= 1.0f && q < 65535)
    q += 1;
  return (int)q;
}

uint BlLuma(int x, int y)
{
  return gBlY.Load(int3(clamp(x, 0, (int)gLumaW - 1), y, 0)) >> 2;
}

uint Guide(int x, int y)
{
  return (BlLuma(x - 1, y) + 2 * BlLuma(x, y) + BlLuma(x + 1, y) + 2) >> 2;
}

uint Output(int q16)
{
  return (uint)clamp((q16 + 8) >> 4, 0, 4095);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
  if (id.x >= gChromaW || id.y >= gChromaH)
    return;
  int i = (int)id.x;
  int j = (int)id.y;
  uint cb = gBlCb.Load(int3(id.xy, 0)) >> 2;
  uint cr = gBlCr.Load(int3(id.xy, 0)) >> 2;

  int pCb;
  int pCr;
  if (gMmrMask != 0)
  {
    uint g = (Guide(2 * i, 2 * j) + Guide(2 * i, 2 * j + 1) + 1) >> 1;
    uint3 x = clamp(uint3(g, cb, cr), gPivotLow.xyz, gPivotHigh.xyz);
    pCb = (gMmrMask & 2) ? MmrQ16(0, gOrderCb, x) : gTables[1024 + cb];
    pCr = (gMmrMask & 4) ? MmrQ16(11, gOrderCr, x) : gTables[2048 + cr];
  }
  else
  {
    pCb = gTables[1024 + cb];
    pCr = gTables[2048 + cr];
  }

  if (gUseEl)
  {
    pCb += gTables[3072 + 1024 + ElCode(gElCb, i, j, (int)gElW)];
    pCr += gTables[3072 + 2048 + ElCode(gElCr, i, j, (int)gElW)];
  }
  gOutCb[id.xy] = Output(pCb);
  gOutCr[id.xy] = Output(pCr);
}
