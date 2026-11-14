/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DvRpuTransport.h"

#include "DvBlob.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace DVTUNNEL
{
namespace
{

constexpr std::size_t BASE_BLOCK_BYTES = 71;

class CByteWriter
{
public:
  explicit CByteWriter(std::vector<std::uint8_t>& out) : m_out(out) {}

  void U8(std::uint32_t v) { m_out.push_back(static_cast<std::uint8_t>(v)); }
  void U16(std::uint32_t v)
  {
    U8(v >> 8);
    U8(v);
  }
  void U32(std::uint32_t v)
  {
    U16(v >> 16);
    U16(v);
  }
  void S16(std::int32_t v) { U16(static_cast<std::uint32_t>(v) & 0xFFFF); }
  // A parsed DM field, sent as 16 bits; signed values in two's complement.
  bool Field(std::int64_t v)
  {
    if (v < -32768 || v > 65535)
      return false;
    U16(static_cast<std::uint32_t>(v) & 0xFFFF);
    return true;
  }

private:
  std::vector<std::uint8_t>& m_out;
};

// Exact conversion of num/den to a fixed-point integer with fracBits.
std::optional<std::int64_t> ToFixed(const AVRational& r,
                                    int fracBits,
                                    std::int64_t lo,
                                    std::int64_t hi)
{
  if (r.den <= 0)
    return {};
  const std::int64_t scaled = static_cast<std::int64_t>(r.num) * (std::int64_t{1} << fracBits);
  if (scaled % r.den != 0)
    return {};
  const std::int64_t v = scaled / r.den;
  if (v < lo || v > hi)
    return {};
  return v;
}

bool ColourOk(const AVDOVIColorMetadata& c)
{
  return c.signal_eotf == 65535 && c.signal_eotf_param0 == 0 && c.signal_eotf_param1 == 0 &&
         c.signal_eotf_param2 == 0 && c.signal_bit_depth == 12 &&
         (c.signal_color_space == 0 || c.signal_color_space == 2) && c.signal_chroma_format == 0 &&
         c.signal_full_range_flag == 1 && c.source_min_pq <= c.source_max_pq &&
         c.source_max_pq <= 4095;
}

std::optional<DvColourParams> WriteBaseBlock(const AVDOVIColorMetadata& c, CByteWriter& w)
{
  DvColourParams colour;
  w.U8(0);
  w.U8(c.scene_refresh_flag);
  for (int i = 0; i < 9; ++i)
  {
    const std::optional<std::int64_t> v = ToFixed(c.ycc_to_rgb_matrix[i], 13, -32768, 32767);
    if (!v)
      return {};
    colour.ycc[i] = static_cast<std::int32_t>(*v);
    w.S16(colour.ycc[i]);
  }
  for (int i = 0; i < 3; ++i)
  {
    const std::optional<std::int64_t> v = ToFixed(c.ycc_to_rgb_offset[i], 28, 0, 0xFFFFFFFFLL);
    if (!v)
      return {};
    colour.offset[i] = static_cast<std::uint32_t>(*v);
    w.U32(colour.offset[i]);
  }
  for (int i = 0; i < 9; ++i)
  {
    const std::optional<std::int64_t> v = ToFixed(c.rgb_to_lms_matrix[i], 14, -32768, 32767);
    if (!v)
      return {};
    colour.lms[i] = static_cast<std::int32_t>(*v);
    w.S16(colour.lms[i]);
  }
  colour.colorSpace = c.signal_color_space;
  w.U16(c.signal_eotf);
  w.U16(c.signal_eotf_param0);
  w.U16(c.signal_eotf_param1);
  w.U32(c.signal_eotf_param2);
  w.U8(c.signal_bit_depth);
  w.U8(c.signal_color_space);
  w.U8(1); // 4:2:0 is transmitted as 4:2:2
  w.U8(c.signal_full_range_flag);
  w.U16(c.source_min_pq);
  w.U16(c.source_max_pq);
  w.U16(c.source_diagonal);
  w.U8(0); // block count, patched once the blocks are written
  return colour;
}

// FFmpeg stores CIE xy as the RPU's signed 16-bit value over 32767.
bool PutCieXy(const AVCIExy& xy, CByteWriter& w)
{
  if (xy.x.den != 32767 || xy.y.den != 32767)
    return false;
  w.S16(xy.x.num);
  w.S16(xy.y.num);
  return true;
}

bool PutPrimaries(const AVColorPrimariesDesc& p, CByteWriter& w)
{
  return PutCieXy(p.prim.r, w) && PutCieXy(p.prim.g, w) && PutCieXy(p.prim.b, w) &&
         PutCieXy(p.wp, w);
}

// Optional primaries are absent when FFmpeg left them zero; a real white point
// is never at the origin.
bool HasPrimaries(const AVColorPrimariesDesc& p)
{
  return p.wp.x.num != 0 || p.wp.y.num != 0;
}

// Content-mapping v4 blocks are rebuilt from FFmpeg's parsed fields: fields of up
// to 8 bits become one byte, wider fields two bytes BE. Optional trailing fields
// are sent when FFmpeg read them; it leaves absent ones zero.
bool BuildPayload(const AVDOVIDmData& dm, std::vector<std::uint8_t>& payload)
{
  CByteWriter w(payload);
  switch (dm.level)
  {
    case 1:
      return w.Field(dm.l1.min_pq) && w.Field(dm.l1.max_pq) && w.Field(dm.l1.avg_pq);
    case 2:
      return w.Field(dm.l2.target_max_pq) && w.Field(dm.l2.trim_slope) &&
             w.Field(dm.l2.trim_offset) && w.Field(dm.l2.trim_power) &&
             w.Field(dm.l2.trim_chroma_weight) && w.Field(dm.l2.trim_saturation_gain) &&
             w.Field(dm.l2.ms_weight);
    case 3:
      return w.Field(dm.l3.min_pq_offset) && w.Field(dm.l3.max_pq_offset) &&
             w.Field(dm.l3.avg_pq_offset);
    case 4:
      return w.Field(dm.l4.anchor_pq) && w.Field(dm.l4.anchor_power);
    case 5:
      return w.Field(dm.l5.left_offset) && w.Field(dm.l5.right_offset) &&
             w.Field(dm.l5.top_offset) && w.Field(dm.l5.bottom_offset);
    case 8:
    {
      const AVDOVIDmLevel8& l8 = dm.l8;
      auto any = [](const std::uint8_t(&v)[6])
      { return std::any_of(std::begin(v), std::end(v), [](std::uint8_t x) { return x != 0; }); };
      const bool hue = any(l8.hue_vector_field);
      const bool saturation = hue || any(l8.saturation_vector_field);
      const bool clipTrim = saturation || l8.clip_trim != 0;
      const bool midContrast = clipTrim || l8.target_mid_contrast != 0;
      w.U8(l8.target_display_index);
      if (!w.Field(l8.trim_slope) || !w.Field(l8.trim_offset) || !w.Field(l8.trim_power) ||
          !w.Field(l8.trim_chroma_weight) || !w.Field(l8.trim_saturation_gain) ||
          !w.Field(l8.ms_weight))
        return false;
      if (midContrast && !w.Field(l8.target_mid_contrast))
        return false;
      if (clipTrim && !w.Field(l8.clip_trim))
        return false;
      for (int i = 0; saturation && i < 6; ++i)
        w.U8(l8.saturation_vector_field[i]);
      for (int i = 0; hue && i < 6; ++i)
        w.U8(l8.hue_vector_field[i]);
      return true;
    }
    case 9:
      w.U8(dm.l9.source_primary_index);
      return !HasPrimaries(dm.l9.source_display_primaries) ||
             PutPrimaries(dm.l9.source_display_primaries, w);
    case 10:
      w.U8(dm.l10.target_display_index);
      if (!w.Field(dm.l10.target_max_pq) || !w.Field(dm.l10.target_min_pq))
        return false;
      w.U8(dm.l10.target_primary_index);
      return !HasPrimaries(dm.l10.target_display_primaries) ||
             PutPrimaries(dm.l10.target_display_primaries, w);
    case 11:
      // The reserved bits are zero.
      w.U8(dm.l11.content_type);
      w.U8((dm.l11.whitepoint << 4) | (dm.l11.reference_mode_flag ? 0x08 : 0));
      w.U8(0);
      w.U8(0);
      return true;
    case 254:
      w.U8(dm.l254.dm_mode);
      w.U8(dm.l254.dm_version_index);
      return true;
    case 255:
      w.U8(dm.l255.dm_run_mode);
      w.U8(dm.l255.dm_run_version);
      for (std::uint8_t v : dm.l255.dm_debug)
        w.U8(v);
      return true;
    default:
      return false;
  }
}

// SMPTE ST 2084 constants.
constexpr double PQ_M1 = 2610.0 / 16384;
constexpr double PQ_M2 = 2523.0 / 4096 * 128;
constexpr double PQ_C1 = 3424.0 / 4096;
constexpr double PQ_C2 = 2413.0 / 4096 * 32;
constexpr double PQ_C3 = 2392.0 / 4096 * 32;

struct BlockLayout
{
  std::optional<std::size_t> level5; //!< offset of the level 5 payload
  std::size_t insertAt = 0; //!< where a missing level 5 block goes; 0 is the end
};

// Walks the extension blocks of serialized metadata.
std::optional<BlockLayout> ReadBlockLayout(const std::vector<std::uint8_t>& bytes)
{
  if (bytes.size() < BASE_BLOCK_BYTES)
    return {};
  const unsigned count = bytes[BASE_BLOCK_BYTES - 1];
  std::size_t pos = BASE_BLOCK_BYTES;
  BlockLayout layout;
  for (unsigned i = 0; i < count; ++i)
  {
    if (bytes.size() - pos < 5)
      return {};
    const std::size_t length = (std::size_t{bytes[pos]} << 24) |
                               (std::size_t{bytes[pos + 1]} << 16) |
                               (std::size_t{bytes[pos + 2]} << 8) | bytes[pos + 3];
    const std::uint8_t level = bytes[pos + 4];
    // Blocks are in RPU order, so a missing level 5 block goes after levels 1,
    // 2 and 4.
    if (!layout.insertAt && level != 1 && level != 2 && level != 4)
      layout.insertAt = pos;
    pos += 5;
    if (length > bytes.size() - pos || (level == 5 && (length != 8 || layout.level5)))
      return {};
    if (level == 5)
      layout.level5 = pos;
    pos += length;
  }
  if (pos != bytes.size())
    return {};
  return layout;
}

} // namespace

unsigned NitsToPq(double nits)
{
  const double y = std::pow(std::clamp(nits / 10000, 0.0, 1.0), PQ_M1);
  return static_cast<unsigned>(
      std::lround(std::pow((PQ_C1 + PQ_C2 * y) / (1 + PQ_C3 * y), PQ_M2) * 4095));
}

double PqToNits(unsigned pq)
{
  const double e = std::pow(std::min(pq, 4095u) / 4095.0, 1 / PQ_M2);
  return 10000 * std::pow(std::max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), 1 / PQ_M1);
}

std::optional<DvDisplayMetadata> SerializeDisplayMetadata(const void* blobData,
                                                          std::size_t blobSize,
                                                          const DvCmv40Policy& cmv40)
{
  const CDvBlob blob(blobData, blobSize);
  const std::optional<AVDOVIRpuDataHeader> header = blob.Header();
  const std::optional<AVDOVIColorMetadata> colour = blob.Color();
  if (!header || !colour)
    return {};

  const AVDOVIMetadata& root = blob.Root();
  const int count = root.num_ext_blocks;
  if (count < 0 || count > AV_DOVI_MAX_EXT_BLOCKS)
    return {};
  if (count > 0 && (root.ext_block_size < sizeof(AVDOVIDmData) ||
                    root.ext_block_size > blobSize / static_cast<std::size_t>(count) ||
                    !blob.RangeOk(root.ext_block_offset, root.ext_block_size * count)))
    return {};

  if (header->vdr_bit_depth != 12 || !ColourOk(*colour))
    return {};

  std::vector<std::uint8_t> bytes;
  bytes.reserve(MAX_METADATA_BYTES);
  CByteWriter w(bytes);
  DvDisplayMetadata out;
  const std::optional<DvColourParams> colourParams = WriteBaseBlock(*colour, w);
  if (!colourParams)
    return {};
  out.colour = *colourParams;

  std::vector<AVDOVIDmData> blocks;
  for (int i = 0; i < count; ++i)
  {
    const std::optional<AVDOVIDmData> block =
        blob.Read<AVDOVIDmData>(root.ext_block_offset + root.ext_block_size * i);
    if (!block)
      return {};
    if (block->level != 6)
      blocks.push_back(*block);
  }
  auto hasLevel = [&blocks](std::uint8_t level)
  {
    return std::any_of(blocks.begin(), blocks.end(),
                       [level](const AVDOVIDmData& dm) { return dm.level == level; });
  };
  // Level 254 marks v4.0 metadata. The added levels are those of dovi_tool's
  // safe defaults: no L1 offsets, DCI-P3 D65 mastering, cinema content.
  out.cmv40 = hasLevel(254);
  out.sourceMaxPq = colour->source_max_pq;
  const DvCmv40Append mode = cmv40.append;
  const bool append =
      mode == DvCmv40Append::ALWAYS ||
      ((mode == DvCmv40Append::WITHOUT_TRIMS || mode == DvCmv40Append::AUTO) && !hasLevel(2)) ||
      (mode == DvCmv40Append::AUTO && cmv40.maxSourcePq > 0 &&
       colour->source_max_pq <= cmv40.maxSourcePq);
  if (!hasLevel(254) && append)
  {
    AVDOVIDmData dm{};
    dm.level = 3;
    dm.l3 = {2048, 2048, 2048};
    blocks.push_back(dm);
    dm = {};
    dm.level = 9;
    dm.l9.source_primary_index = 0;
    blocks.push_back(dm);
    dm = {};
    dm.level = 11;
    dm.l11.content_type = 1;
    blocks.push_back(dm);
    dm = {};
    dm.level = 254;
    dm.l254.dm_version_index = 2;
    blocks.push_back(dm);
    out.cmv40Appended = true;
  }
  // FFmpeg lists the static blocks (levels 10 and 254 here) first. Send the
  // RPU's order instead: content mapping v2.9 levels, then v4.0 levels.
  auto rank = [](const AVDOVIDmData& dm)
  {
    const bool cm40 = dm.level == 3 || (dm.level >= 8 && dm.level <= 11) || dm.level == 254;
    return dm.level + (cm40 ? 256 : 0);
  };
  std::stable_sort(blocks.begin(), blocks.end(),
                   [&rank](const AVDOVIDmData& a, const AVDOVIDmData& b)
                   { return rank(a) < rank(b); });

  unsigned written = 0;
  std::vector<std::uint8_t> payload;
  for (const AVDOVIDmData& dm : blocks)
  {
    payload.clear();
    if (!BuildPayload(dm, payload))
      return {};

    w.U32(static_cast<std::uint32_t>(payload.size()));
    w.U8(dm.level);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (bytes.size() > MAX_METADATA_BYTES)
      return {};
    ++written;
  }
  bytes[BASE_BLOCK_BYTES - 1] = static_cast<std::uint8_t>(written);

  out.bytes = std::move(bytes);
  return out;
}

bool SetLevel5ActiveArea(std::vector<std::uint8_t>& bytes, const DvPlacement* placement)
{
  const std::optional<BlockLayout> layout = ReadBlockLayout(bytes);
  if (!layout)
    return false;
  const std::optional<std::size_t>& level5 = layout->level5;

  std::uint16_t offsets[4] = {}; // left, right, top, bottom
  if (placement)
  {
    const DvPlacement& p = *placement;
    const DvRect& r = p.picture;
    if (r.IsEmpty() || r.right > p.canvasWidth || r.bottom > p.canvasHeight ||
        p.sourceWidth == 0 || p.sourceHeight == 0)
      return false;
    unsigned source[4] = {};
    // Level 5 refers to the full 16:9 frame; a cropped encode has already lost
    // the letterbox it describes.
    if (level5 && p.sourceWidth * 9 == p.sourceHeight * 16)
    {
      for (int i = 0; i < 4; ++i)
        source[i] = (unsigned{bytes[*level5 + i * 2]} << 8) | bytes[*level5 + i * 2 + 1];
      if (source[0] + source[1] >= p.sourceWidth || source[2] + source[3] >= p.sourceHeight)
        std::fill_n(source, 4, 0u);
    }
    const unsigned width = r.right - r.left;
    const unsigned height = r.bottom - r.top;
    auto scale = [](unsigned v, unsigned to, unsigned from) { return (v * to + from / 2) / from; };
    offsets[0] = static_cast<std::uint16_t>(r.left + scale(source[0], width, p.sourceWidth));
    offsets[1] = static_cast<std::uint16_t>(p.canvasWidth - r.right +
                                            scale(source[1], width, p.sourceWidth));
    offsets[2] = static_cast<std::uint16_t>(r.top + scale(source[2], height, p.sourceHeight));
    offsets[3] = static_cast<std::uint16_t>(p.canvasHeight - r.bottom +
                                            scale(source[3], height, p.sourceHeight));
  }

  std::uint8_t payload[8];
  for (int i = 0; i < 4; ++i)
  {
    payload[i * 2] = static_cast<std::uint8_t>(offsets[i] >> 8);
    payload[i * 2 + 1] = static_cast<std::uint8_t>(offsets[i]);
  }
  if (level5)
  {
    std::copy_n(payload, 8, bytes.begin() + *level5);
    return true;
  }
  if (!placement)
    return true;
  const unsigned count = bytes[BASE_BLOCK_BYTES - 1];
  if (count >= AV_DOVI_MAX_EXT_BLOCKS || bytes.size() + 13 > MAX_METADATA_BYTES)
    return false;
  const std::uint8_t block[13] = {0, 0, 0, 8, 5, payload[0], payload[1], payload[2],
                                  payload[3], payload[4], payload[5], payload[6], payload[7]};
  bytes.insert(bytes.begin() + (layout->insertAt ? layout->insertAt : bytes.size()), std::begin(block),
               std::end(block));
  bytes[BASE_BLOCK_BYTES - 1] = static_cast<std::uint8_t>(count + 1);
  return true;
}

std::optional<std::array<std::uint16_t, 4>> Level5Offsets(const std::vector<std::uint8_t>& bytes)
{
  const std::optional<BlockLayout> layout = ReadBlockLayout(bytes);
  if (!layout || !layout->level5)
    return {};
  const std::size_t pos = *layout->level5;
  std::array<std::uint16_t, 4> offsets;
  for (int k = 0; k < 4; ++k)
    offsets[k] = static_cast<std::uint16_t>((bytes[pos + k * 2] << 8) | bytes[pos + k * 2 + 1]);
  return offsets;
}

bool CDvSceneTracker::Update(double seconds)
{
  if (m_last && seconds == *m_last)
    return m_refresh;
  m_refresh = !m_last || seconds < *m_last || seconds - *m_last > 1.0;
  m_last = seconds;
  return m_refresh;
}

void SetSceneRefresh(std::vector<std::uint8_t>& bytes)
{
  if (bytes.size() > 1)
    bytes[1] = 1; // after the base block's header byte
}

std::size_t PacketCount(std::size_t metadataBytes)
{
  if (metadataBytes <= 119)
    return 1;
  return 1 + (metadataBytes - 119 + 120) / 121;
}

std::optional<std::vector<DvPacket>> Packetize(const std::vector<std::uint8_t>& metadata,
                                               unsigned metadataId)
{
  if (metadata.empty() || metadata.size() > MAX_METADATA_BYTES || metadataId > 15)
    return {};

  const std::size_t n = PacketCount(metadata.size());
  std::vector<DvPacket> packets(n);
  std::size_t pos = 0;
  for (std::size_t k = 0; k < n; ++k)
  {
    DvPacket& p = packets[k];
    p.fill(0);
    if (n == 1)
      p[0] = 0x00;
    else if (k == 0)
      p[0] = 0x40;
    else if (k == n - 1)
      p[0] = 0xC0;
    else
      p[0] = 0x80;
    p[1] = static_cast<std::uint8_t>((metadataId << 4) | metadataId);

    std::size_t start = 3;
    if (k == 0)
    {
      p[3] = static_cast<std::uint8_t>(metadata.size() >> 8);
      p[4] = static_cast<std::uint8_t>(metadata.size());
      start = 5;
    }
    const std::size_t take = std::min(metadata.size() - pos, std::size_t{124} - start);
    std::copy_n(metadata.begin() + pos, take, p.begin() + start);
    pos += take;

    const std::uint32_t crc = Crc32Mpeg2(p.data(), 124);
    p[124] = static_cast<std::uint8_t>(crc >> 24);
    p[125] = static_cast<std::uint8_t>(crc >> 16);
    p[126] = static_cast<std::uint8_t>(crc >> 8);
    p[127] = static_cast<std::uint8_t>(crc);
  }
  if (pos != metadata.size())
    return {};
  return packets;
}

std::uint32_t Crc32Mpeg2(const std::uint8_t* data, std::size_t size)
{
  std::uint32_t crc = 0xFFFFFFFF;
  for (std::size_t i = 0; i < size; ++i)
  {
    crc ^= static_cast<std::uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

} // namespace DVTUNNEL
