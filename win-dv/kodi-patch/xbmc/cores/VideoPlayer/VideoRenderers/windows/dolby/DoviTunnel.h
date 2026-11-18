/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include "DvRpuTransport.h"
#include "cores/VideoSettings.h"
#include "guilib/D3DResource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class CRenderBuffer;

/*!
 * \brief Checks whether the tunnel can render a stream's Dolby Vision metadata.
 *
 * \param metadata the AV_FRAME_DATA_DOVI_METADATA side data of a picture
 * \param metadataSize size of metadata in bytes
 * \param profile Dolby Vision profile of the stream
 * \param compatibilityId base-layer signal compatibility ID of the stream
 * \return true for a supported profile with a mapping the tunnel computes
 */
bool IsDolbyVisionTunnelMetadata(const void* metadata,
                                 std::size_t metadataSize,
                                 int profile,
                                 int compatibilityId);

/*!
 * \brief True when the mapping needs the GPU composer: an enhancement layer
 * or MMR chroma prediction.
 */
bool DolbyVisionMetadataNeedsComposer(const void* metadata, std::size_t metadataSize);

/*!
 * \brief What the tunnel sends, for the player info dialog.
 */
struct DvTunnelInfo
{
  std::string profile; //!< e.g. "8.1" or "7 FEL (used)"
  std::string contentMapping; //!< "v2.9", "v4.0" or "v2.9 sent as v4.0"
  std::string master; //!< the mastering display's peak
  std::string activeArea; //!< level 5 as sent while no GUI is visible

  bool operator==(const DvTunnelInfo&) const = default;
};

/*!
 * \brief Packs Dolby Vision pictures and their metadata into 8-bit carrier
 * frames for TV-led output.
 *
 * Each picture is reshaped or composed to 12-bit PQ, scaled to the carrier
 * frame, and packed with its display management metadata. Metadata layout per
 * ETSI GS CCM 001, transfer per SMPTE ST 2084.
 */
class CDoviTunnel
{
public:
  CDoviTunnel();
  ~CDoviTunnel();

  CDoviTunnel(const CDoviTunnel&) = delete;
  CDoviTunnel& operator=(const CDoviTunnel&) = delete;

  bool Initialize(ID3D11Device* device);

  /*!
   * \brief True when the GPU composer is available on the device.
   */
  bool CanCompose() const;

  /*!
   * \brief Blends Kodi's GUI over the last rendered frame into target, in the
   * frame's source colour space.
   *
   * \param peakNits the luminance of GUI white
   * \return false when no frame was rendered or its colour space is not
   *         supported for blending
   */
  bool CompositeGUI(ID3D11Texture2D* gui, ID3D11Texture2D* target, float peakNits);

  /*!
   * \brief Ends playback: logs the playback summary and releases the GPU
   * resources. Initialize must be called before the next Render.
   */
  void Reset();

  /*!
   * \brief Sets Kodi's video scaling method, used when the picture is upscaled.
   */
  void SetScalingMethod(ESCALINGMETHOD method);

  /*!
   * \brief Sets when content mapping v2.9 metadata is sent as v4.0.
   */
  void SetCmv40Append(const DVTUNNEL::DvCmv40Policy& policy);

  /*!
   * \brief What the last rendered picture was sent with.
   */
  DvTunnelInfo Info() const;

  /*!
   * \brief Renders one picture into a carrier frame.
   *
   * The picture is scaled to fit target with its aspect ratio kept.
   *
   * \param buffer the render buffer holding the picture's metadata and
   *        enhancement layer
   * \param width picture width
   * \param height picture height
   * \param decoded the decoder's P010 texture array on the tunnel's device, or
   *        null when the picture is unavailable
   * \param slice the picture's index in decoded
   * \param guiVisible true when the GUI will be blended over the frame
   * \param target the carrier frame
   * \return false when the picture was skipped; the reason is logged once
   */
  bool Render(const CRenderBuffer& buffer,
              unsigned width,
              unsigned height,
              ID3D11Texture2D* decoded,
              unsigned slice,
              bool guiVisible,
              CD3DTexture& target);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
