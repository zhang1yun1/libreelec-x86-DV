/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DoviEnhancementDecoder.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "utils/log.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
}

namespace
{
// Pictures waiting for their base layer hold decoder surfaces, so keep few.
constexpr std::size_t MAXIMUM_QUEUED = 8;
// Covers the pictures the base-layer decoder can hold back for reordering.
constexpr std::size_t MAXIMUM_BASE_PICTURES = 64;
// Base-layer packets to send while waiting for an enhancement picture.
constexpr std::uint64_t MAXIMUM_LOOKAHEAD = 8;
// Enhancement packets to keep until the layers are matched up after a flush.
constexpr std::size_t MAXIMUM_PENDING = 16;
// Base-layer packets after a flush within which the enhancement layer is
// expected to become decodable; past this, pictures are shown without it.
constexpr std::uint64_t MAXIMUM_PREROLL = 128;
} // namespace

/*!
 * \brief Gives the pictures of one HEVC layer their display position: the
 * picture order count, carried on across IDR pictures.
 */
class CDoviEnhancementDecoder::CDisplayOrder
{
public:
  explicit CDisplayOrder(const AVCodecParameters* parameters)
  {
    m_context = avcodec_alloc_context3(nullptr);
    if (m_context && avcodec_parameters_to_context(m_context, parameters) == 0)
      Restart();
  }
  ~CDisplayOrder()
  {
    av_parser_close(m_parser);
    avcodec_free_context(&m_context);
  }
  CDisplayOrder(const CDisplayOrder&) = delete;
  CDisplayOrder& operator=(const CDisplayOrder&) = delete;

  bool IsOpen() const { return m_parser != nullptr; }

  void Restart()
  {
    av_parser_close(m_parser);
    m_parser = av_parser_init(AV_CODEC_ID_HEVC);
    if (m_parser)
      m_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    m_offset = 0;
    m_last = std::nullopt;
    m_parsed = 0;
    m_start = std::nullopt;
    m_leading = 0;
    m_base = std::nullopt;
  }

  struct Picture
  {
    std::int64_t position;
    bool randomAccess; //!< an IRAP picture: decoding can start here
  };

  //! Whether an IRAP picture was parsed since the restart.
  bool Started() const { return m_start.has_value(); }

  //! Position of the first IRAP picture parsed since the restart.
  std::optional<std::int64_t> StartPosition() const
  {
    return m_start ? std::optional(m_start->first) : std::nullopt;
  }

  /*!
   * \brief Display position minus the number of pictures shown before it,
   * known once the first IRAP picture and its leading pictures are parsed.
   *
   * It is the same for every picture, and the same number of pictures came
   * before the restart in both layers, so the difference between the layers'
   * bases converts one layer's positions to the other's.
   */
  std::optional<std::int64_t> Base() const { return m_base; }

  std::optional<Picture> Parse(const AVPacket& packet)
  {
    if (!m_parser)
      return {};
    std::uint8_t* output = nullptr;
    int outputSize = 0;
    m_parser->output_picture_number = INT_MIN;
    if (av_parser_parse2(m_parser, m_context, &output, &outputSize, packet.data, packet.size,
                         packet.pts, packet.dts, -1) != packet.size ||
        outputSize <= 0 || m_parser->output_picture_number == INT_MIN)
      return {};
    const int order = m_parser->output_picture_number;
    // An IDR picture restarts the picture order count at 0 and is shown after
    // every picture before it. The layers needn't have their IDR pictures in
    // the same place, so the count goes on rather than starting over.
    const bool randomAccess = m_parser->key_frame == 1;
    if (m_last && randomAccess && order == 0)
      m_offset = *m_last + 1;
    const std::int64_t position = m_offset + order;
    m_last = std::max(m_last.value_or(position), position);

    // An IRAP picture is shown after every picture decoded before it and after
    // its leading pictures, which come straight after it in decoding order.
    if (!m_start && randomAccess)
      m_start = std::make_pair(position, m_parsed);
    else if (m_start && !m_base && position < m_start->first)
      ++m_leading;
    else if (m_start && !m_base)
      m_base = m_start->first - static_cast<std::int64_t>(m_start->second) - m_leading;
    ++m_parsed;
    return Picture{position, randomAccess};
  }

private:
  AVCodecParserContext* m_parser = nullptr;
  AVCodecContext* m_context = nullptr;
  std::int64_t m_offset = 0;
  // The last position in display order so far.
  std::optional<std::int64_t> m_last;
  std::uint64_t m_parsed = 0;
  // Position and number of pictures before the first IRAP picture.
  std::optional<std::pair<std::int64_t, std::uint64_t>> m_start;
  std::int64_t m_leading = 0;
  std::optional<std::int64_t> m_base;
};

CDoviEnhancementDecoder::CDoviEnhancementDecoder() : m_processInfo(CProcessInfo::CreateInstance())
{
}

CDoviEnhancementDecoder::~CDoviEnhancementDecoder()
{
  m_frames.clear();
  ClearPending();
  av_bsf_free(&m_filter);
  avcodec_free_context(&m_decoder);
  if (m_hardware)
    m_hardware->Release();
}

enum AVPixelFormat CDoviEnhancementDecoder::GetFormat(AVCodecContext* avctx,
                                                      const enum AVPixelFormat* formats)
{
  auto* self = static_cast<CDoviEnhancementDecoder*>(static_cast<ICallbackHWAccel*>(avctx->opaque));
  if (self->m_hardware)
  {
    self->m_hardware->Release();
    self->m_hardware = nullptr;
    avctx->get_buffer2 = avcodec_default_get_buffer2;
    avctx->slice_flags = 0;
    av_buffer_unref(&avctx->hw_frames_ctx);
  }

  for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
  {
    for (const std::string& id : CDVDFactoryCodec::GetHWAccels())
    {
      IHardwareDecoder* decoder = CDVDFactoryCodec::CreateVideoCodecHWAccel(
          id, *self->m_hints, *self->m_processInfo, *format);
      if (!decoder)
        continue;
      if (decoder->Open(avctx, avctx, *format))
      {
        self->m_hardware = decoder;
        return *format;
      }
      decoder->Release();
    }
  }

  CLog::Log(LOGWARNING, "Dolby Vision enhancement layer: no hardware decoder; composing without "
                        "the enhancement layer");
  return AV_PIX_FMT_NONE;
}

std::unique_ptr<CDoviEnhancementDecoder> CDoviEnhancementDecoder::Create(
    const CDVDStreamInfo& hints)
{
  if (hints.codec != AV_CODEC_ID_HEVC || hints.dovi.dv_profile != 7 || !hints.dovi.el_present_flag)
    return nullptr;
  auto decoder = std::make_unique<CDoviEnhancementDecoder>();
  if (decoder->Open(hints))
    return decoder;
  CLog::Log(LOGWARNING, "Dolby Vision enhancement layer: decoder could not be opened");
  return nullptr;
}

bool CDoviEnhancementDecoder::Open(const CDVDStreamInfo& hints)
{
  const AVBitStreamFilter* split = av_bsf_get_by_name("dovi_split");
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  if (!split || !codec || av_bsf_alloc(split, &m_filter) < 0)
    return false;

  AVCodecParameters* in = m_filter->par_in;
  in->codec_type = AVMEDIA_TYPE_VIDEO;
  in->codec_id = AV_CODEC_ID_HEVC;
  in->width = hints.width;
  in->height = hints.height;
  if (hints.extradata)
  {
    in->extradata = static_cast<std::uint8_t*>(
        av_mallocz(hints.extradata.GetSize() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!in->extradata)
      return false;
    std::memcpy(in->extradata, hints.extradata.GetData(), hints.extradata.GetSize());
    in->extradata_size = static_cast<int>(hints.extradata.GetSize());
  }
  if (av_opt_set(m_filter->priv_data, "mode", "el", 0) < 0 || av_bsf_init(m_filter) < 0)
    return false;

  m_baseOrder = std::make_unique<CDisplayOrder>(m_filter->par_in);
  m_enhancementOrder = std::make_unique<CDisplayOrder>(m_filter->par_out);
  if (!m_baseOrder->IsOpen() || !m_enhancementOrder->IsOpen())
    return false;

  m_hints = std::make_unique<CDVDStreamInfo>(hints, false);
  m_hints->width = m_filter->par_out->width;
  m_hints->height = m_filter->par_out->height;

  m_decoder = avcodec_alloc_context3(codec);
  if (!m_decoder || avcodec_parameters_to_context(m_decoder, m_filter->par_out) < 0)
    return false;
  // Kodi's hardware decoders need single-threaded decoding.
  m_decoder->thread_count = 1;
  m_decoder->opaque = static_cast<ICallbackHWAccel*>(this);
  m_decoder->get_format = GetFormat;
  if (avcodec_open2(m_decoder, codec, nullptr) < 0)
    return false;

  CLog::Log(LOGINFO, "Dolby Vision enhancement layer: decoding {}x{} layer for {}x{} base layer",
            m_filter->par_out->width, m_filter->par_out->height, hints.width, hints.height);
  return true;
}

void CDoviEnhancementDecoder::Send(const std::uint8_t* data,
                                   int size,
                                   std::int64_t pts,
                                   std::int64_t dts)
{
  AVPacket* packet = av_packet_alloc();
  if (!packet || av_new_packet(packet, size) < 0)
  {
    av_packet_free(&packet);
    return;
  }
  std::memcpy(packet->data, data, size);
  packet->pts = pts;
  packet->dts = dts;

  ++m_sent;
  const std::optional<CDisplayOrder::Picture> base = m_baseOrder->Parse(*packet);
  if (base)
  {
    m_basePictures.push_back({pts, base->position, m_sent});
    if (m_basePictures.size() > MAXIMUM_BASE_PICTURES)
      m_basePictures.pop_front();
  }

  if (av_bsf_send_packet(m_filter, packet) == 0)
  {
    while (av_bsf_receive_packet(m_filter, packet) == 0)
    {
      const bool started = m_enhancementOrder->Started();
      const std::optional<CDisplayOrder::Picture> picture =
          packet->size > 0 ? m_enhancementOrder->Parse(*packet) : std::nullopt;
      if (picture && !m_enhancementShift)
      {
        // Both layers count from where decoding started, so their positions
        // are matched up at their first IRAP pictures.
        if (!started && picture->randomAccess && base && base->randomAccess)
          m_enhancementShift = base->position - picture->position;
        else if (m_baseOrder->Base() && m_enhancementOrder->Base())
          m_enhancementShift = *m_baseOrder->Base() - *m_enhancementOrder->Base();
      }
      if (picture && m_enhancementOrder->Started())
      {
        AVPacket* pending = av_packet_alloc();
        if (pending)
        {
          av_packet_move_ref(pending, packet);
          m_pending.emplace_back(pending, picture->position);
        }
        if (m_enhancementShift)
          DecodePending();
        else if (m_pending.size() > MAXIMUM_PENDING)
          ClearPending();
      }
      av_packet_unref(packet);
      Receive();
    }
  }
  av_packet_free(&packet);
}

void CDoviEnhancementDecoder::DecodePending()
{
  for (auto& [packet, position] : m_pending)
  {
    // The decoder passes the timestamp on to the picture.
    packet->pts = position + *m_enhancementShift;
    packet->dts = AV_NOPTS_VALUE;
    if (avcodec_send_packet(m_decoder, packet) == AVERROR(EAGAIN))
    {
      Receive();
      avcodec_send_packet(m_decoder, packet);
    }
    Receive();
  }
  ClearPending();
}

void CDoviEnhancementDecoder::ClearPending()
{
  for (auto& pending : m_pending)
    av_packet_free(&pending.first);
  m_pending.clear();
}

void CDoviEnhancementDecoder::Drain()
{
  avcodec_send_packet(m_decoder, nullptr);
  Receive();
}

void CDoviEnhancementDecoder::Receive()
{
  AVFrame* frame = av_frame_alloc();
  while (frame && avcodec_receive_frame(m_decoder, frame) == 0)
  {
    if (frame->pts == AV_NOPTS_VALUE)
    {
      av_frame_unref(frame);
      continue;
    }
    m_lastOutput = std::max(m_lastOutput, frame->pts);
    VideoPicture picture;
    if (m_hardware && m_hardware->Decode(m_decoder, frame) == CDVDVideoCodec::VC_PICTURE &&
        m_hardware->GetPicture(m_decoder, &picture) && picture.videoBuffer)
    {
      m_frames.emplace_back(frame->pts,
                            std::shared_ptr<CVideoBuffer>(picture.videoBuffer,
                                                          [](CVideoBuffer* b) { b->Release(); }));
      picture.videoBuffer = nullptr;
      if (m_frames.size() > MAXIMUM_QUEUED)
        m_frames.pop_front();
    }
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
}

const CDoviEnhancementDecoder::BasePicture* CDoviEnhancementDecoder::FindBase(
    std::int64_t pts) const
{
  const auto base = std::ranges::find(m_basePictures, pts, &BasePicture::pts);
  return base == m_basePictures.end() ? nullptr : &*base;
}

bool CDoviEnhancementDecoder::Waiting(std::int64_t pts) const
{
  // Pictures are output in display order, so a later output means this one
  // isn't coming.
  const BasePicture* base = FindBase(pts);
  return base && m_enhancementShift && m_lastOutput < base->position &&
         m_sent - base->sent < MAXIMUM_LOOKAHEAD;
}

bool CDoviEnhancementDecoder::Preroll(std::int64_t pts) const
{
  const BasePicture* base = FindBase(pts);
  if (!base || m_sent > MAXIMUM_PREROLL)
    return false;
  // The enhancement layer can be decoded from its first IRAP picture on.
  return !m_enhancementShift ||
         base->position < *m_enhancementOrder->StartPosition() + *m_enhancementShift;
}

std::shared_ptr<CVideoBuffer> CDoviEnhancementDecoder::Take(std::int64_t pts)
{
  const BasePicture* base = FindBase(pts);
  if (!base)
    return {};
  const std::int64_t position = base->position;
  // Base-layer pictures are output in display order too.
  std::erase_if(m_basePictures,
                [position](const BasePicture& picture) { return picture.position <= position; });

  while (!m_frames.empty() && m_frames.front().first < position)
    m_frames.pop_front();
  if (m_frames.empty() || m_frames.front().first != position)
    return {};
  std::shared_ptr<CVideoBuffer> picture = std::move(m_frames.front().second);
  m_frames.pop_front();
  return picture;
}

void CDoviEnhancementDecoder::Flush()
{
  m_frames.clear();
  m_basePictures.clear();
  m_lastOutput = INT64_MIN;
  m_sent = 0;
  m_enhancementShift.reset();
  ClearPending();
  m_baseOrder->Restart();
  m_enhancementOrder->Restart();
  av_bsf_flush(m_filter);
  avcodec_flush_buffers(m_decoder);
  if (m_hardware)
    m_hardware->Reset();
}
