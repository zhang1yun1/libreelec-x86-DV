/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace DVTUNNEL
{

constexpr std::size_t MAX_METADATA_BYTES = 512;
constexpr std::size_t PACKET_BYTES = 128;
constexpr std::size_t MAX_PACKETS = 5;

/*!
 * \brief Source colour parameters in the fixed-point form of the transport
 * header.
 */
struct DvColourParams
{
  std::int32_t ycc[9] = {};
  std::uint32_t offset[3] = {};
  std::int32_t lms[9] = {};
  std::uint8_t colorSpace = 0;
};

/*!
 * \brief Canvas rectangle, right and bottom exclusive.
 */
struct DvRect
{
  unsigned left = 0;
  unsigned top = 0;
  unsigned right = 0;
  unsigned bottom = 0;

  bool IsEmpty() const { return left >= right || top >= bottom; }
};

/*!
 * \brief Serialized display management metadata and the colour parameters it
 * carries.
 */
struct DvDisplayMetadata
{
  std::vector<std::uint8_t> bytes;
  DvColourParams colour;
  bool cmv40 = false; //!< the source metadata is content mapping v4.0
  bool cmv40Appended = false;
  unsigned sourceMaxPq = 0; //!< the mastering display's peak, 12-bit PQ
};

using DvPacket = std::array<std::uint8_t, PACKET_BYTES>;

/*!
 * \brief When content mapping v2.9 metadata is sent as v4.0.
 */
enum class DvCmv40Append
{
  OFF,
  WITHOUT_TRIMS, //!< only when the metadata has no level 2 trims
  ALWAYS,
  AUTO, //!< without level 2 trims, or when the master is no brighter than maxSourcePq
};

/*!
 * \brief When and for which sources content mapping v4.0 is appended.
 */
struct DvCmv40Policy
{
  DvCmv40Append append = DvCmv40Append::OFF;
  //! AUTO: the highest source_max_pq (12-bit PQ) that is appended; 0 appends
  //! only metadata without trims
  unsigned maxSourcePq = 0;
};

/*!
 * \brief Converts a luminance in cd/m² to a 12-bit SMPTE ST 2084 code value.
 */
unsigned NitsToPq(double nits);

/*!
 * \brief Converts a 12-bit SMPTE ST 2084 code value to a luminance in cd/m².
 */
double PqToNits(unsigned pq);

/*!
 * \brief Serializes the display management metadata of a picture.
 *
 * Level 6 blocks are dropped; the other extension blocks are rebuilt from
 * FFmpeg's parsed fields. Appending content mapping v4.0 adds neutral levels
 * 3, 9, 11 and 254 to v2.9 metadata, so the display maps it with its v4.0
 * engine.
 *
 * \param blob an AVDOVIMetadata side-data blob
 * \param blobSize size of blob in bytes
 * \param cmv40 when to append content mapping v4.0
 * \return the metadata, or empty if the blob is unreadable or a value does not
 *         fit the transport
 */
std::optional<DvDisplayMetadata> SerializeDisplayMetadata(const void* blob,
                                                          std::size_t blobSize,
                                                          const DvCmv40Policy& cmv40 = {});

/*!
 * \brief Where a picture sits on the carrier canvas.
 */
struct DvPlacement
{
  unsigned sourceWidth = 0;
  unsigned sourceHeight = 0;
  unsigned canvasWidth = 0;
  unsigned canvasHeight = 0;
  DvRect picture;
};

/*!
 * \brief Sets the level 5 active area of serialized metadata.
 *
 * The level 5 offsets, in source pixels, are moved to the picture's place on
 * the canvas; metadata without a level 5 block gets one for the canvas outside
 * the picture. Without a placement the whole canvas is active.
 *
 * \return false if the block walk is inconsistent or the picture is not on the
 *         canvas
 */
bool SetLevel5ActiveArea(std::vector<std::uint8_t>& bytes, const DvPlacement* placement);

/*!
 * \brief The left, right, top and bottom offsets of the level 5 block of
 * serialized metadata, or empty if it has none.
 */
std::optional<std::array<std::uint16_t, 4>> Level5Offsets(const std::vector<std::uint8_t>& bytes);

/*!
 * \brief Decides when the display must drop its scene state: on the first
 * picture and after a jump in time of more than a second (a seek or a chapter
 * skip, not dropped pictures), where the next scene cut flagged in the
 * metadata may be far away.
 */
class CDvSceneTracker
{
public:
  /*!
   * \param seconds the picture's presentation time
   * \return true when the picture's metadata must have scene_refresh_flag set;
   *         a repeated picture keeps the previous answer
   */
  bool Update(double seconds);

private:
  std::optional<double> m_last;
  bool m_refresh = false;
};

/*!
 * \brief Sets scene_refresh_flag in serialized metadata.
 */
void SetSceneRefresh(std::vector<std::uint8_t>& bytes);

/*!
 * \brief Number of packets that carry metadataBytes of metadata.
 */
std::size_t PacketCount(std::size_t metadataBytes);

/*!
 * \brief Splits serialized metadata into CRC-protected transport packets.
 *
 * \param metadataId 0..15, changed by the caller whenever the metadata changes
 * \return the packets, or empty if metadata is empty or longer than
 *         MAX_METADATA_BYTES
 */
std::optional<std::vector<DvPacket>> Packetize(const std::vector<std::uint8_t>& metadata,
                                               unsigned metadataId = 0);

/*!
 * \brief CRC-32/MPEG-2 of data.
 */
std::uint32_t Crc32Mpeg2(const std::uint8_t* data, std::size_t size);

} // namespace DVTUNNEL
