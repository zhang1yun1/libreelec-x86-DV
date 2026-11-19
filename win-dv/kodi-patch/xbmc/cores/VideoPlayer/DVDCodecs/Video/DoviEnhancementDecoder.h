/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

class CDVDStreamInfo;
class CVideoBuffer;
struct AVBSFContext;
struct AVCodecContext;
struct AVPacket;

/*!
 * \brief Decodes the Dolby Vision Profile 7 enhancement layer with Kodi's
 * hardware decoder.
 *
 * The layer travels inside the base-layer access units as UNSPEC63 NAL units.
 * The two layers may be coded in different picture orders, so an access unit
 * can carry the enhancement picture of another base-layer picture. Pictures are
 * therefore matched by display position, from each layer's picture order count.
 */
class CDoviEnhancementDecoder : public ICallbackHWAccel
{
public:
  CDoviEnhancementDecoder();
  ~CDoviEnhancementDecoder() override;
  CDoviEnhancementDecoder(const CDoviEnhancementDecoder&) = delete;
  CDoviEnhancementDecoder& operator=(const CDoviEnhancementDecoder&) = delete;

  /*!
   * \brief Creates a decoder for a stream's enhancement layer.
   *
   * \return the decoder, or null unless the stream is HEVC Profile 7 with an
   *         enhancement layer that a hardware decoder can decode
   */
  static std::unique_ptr<CDoviEnhancementDecoder> Create(const CDVDStreamInfo& hints);
  bool Open(const CDVDStreamInfo& hints);

  /*!
   * \brief Decodes the enhancement layer of one base-layer packet.
   *
   * \param data the base-layer packet
   * \param size size of data in bytes
   * \param pts presentation timestamp of the packet
   * \param dts decoding timestamp of the packet
   */
  void Send(const std::uint8_t* data, int size, std::int64_t pts, std::int64_t dts);

  /*!
   * \brief Outputs the pictures the decoder holds back for reordering.
   */
  void Drain();

  /*!
   * \brief Whether the enhancement-layer picture for the base-layer picture
   * with presentation timestamp pts can still be decoded by sending more
   * packets.
   */
  bool Waiting(std::int64_t pts) const;

  /*!
   * \brief Whether the base-layer picture with presentation timestamp pts is
   * shown before the enhancement layer can be decoded, after a flush.
   */
  bool Preroll(std::int64_t pts) const;

  /*!
   * \brief Takes the enhancement-layer picture for the base-layer picture with
   * presentation timestamp pts.
   *
   * Pictures for earlier display positions are dropped.
   *
   * \return the picture, or null if there is none
   */
  std::shared_ptr<CVideoBuffer> Take(std::int64_t pts);
  void Flush();

  //! Number of base-layer packets sent since the last flush.
  std::uint64_t Sent() const { return m_sent; }

  IHardwareDecoder* GetHWAccel() override { return m_hardware; }
  bool GetPictureCommon(VideoPicture* picture) override { return true; }

private:
  class CDisplayOrder;
  struct BasePicture
  {
    std::int64_t pts;
    std::int64_t position;
    std::uint64_t sent;
  };

  static enum AVPixelFormat GetFormat(AVCodecContext* avctx, const enum AVPixelFormat* formats);
  void Receive();
  void DecodePending();
  void ClearPending();
  const BasePicture* FindBase(std::int64_t pts) const;

  // The hardware decoder reports to its own process info, not the stream's.
  std::unique_ptr<CProcessInfo> m_processInfo;
  std::unique_ptr<CDVDStreamInfo> m_hints;
  AVBSFContext* m_filter = nullptr;
  AVCodecContext* m_decoder = nullptr;
  IHardwareDecoder* m_hardware = nullptr;
  std::unique_ptr<CDisplayOrder> m_baseOrder;
  std::unique_ptr<CDisplayOrder> m_enhancementOrder;
  std::deque<BasePicture> m_basePictures;
  // Decoded pictures by display position, in output order.
  std::deque<std::pair<std::int64_t, std::shared_ptr<CVideoBuffer>>> m_frames;
  std::int64_t m_lastOutput = INT64_MIN;
  std::uint64_t m_sent = 0;
  // Added to enhancement-layer positions to give base-layer positions.
  std::optional<std::int64_t> m_enhancementShift;
  // Enhancement packets and their positions, kept until the shift is known.
  std::deque<std::pair<AVPacket*, std::int64_t>> m_pending;
};
