/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - see header for scope and known limitations.
 */

#include "DVDVideoCodecMVCSoftware.h"

#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecs.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <cstdlib>

namespace
{
// edge264 wants to be told how many worker threads to use; the value has
// no relation to Kodi's own thread pools. Four is a starting point for a
// quad-core-class ARM part (Tegra X1: 4x Cortex-A57 + 4x Cortex-A53) and
// should be benchmarked, not trusted - this is exactly the "real-time
// feasibility" question that needs measuring on actual hardware before
// this codec is anything more than a PoC.
constexpr int EDGE264_THREADS = 4;
} // namespace

CDVDVideoCodecMVCSoftware::CDVDVideoCodecMVCSoftware(CProcessInfo& processInfo)
  : CDVDVideoCodec(processInfo)
{
}

CDVDVideoCodecMVCSoftware::~CDVDVideoCodecMVCSoftware()
{
  if (m_decoder)
  {
    // edge264_free() releases every DPB slot it still owns via FreeCb,
    // which returns each block to m_samplesPool/m_mbsPool rather than
    // freeing it immediately (see ReleaseToPool()) - so the actual
    // std::free() calls only happen afterwards, here.
    edge264_free(&m_decoder);
  }
  DrainPool(m_samplesPool);
  DrainPool(m_mbsPool);
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecMVCSoftware::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecMVCSoftware>(processInfo);
}

bool CDVDVideoCodecMVCSoftware::Register()
{
  // Registered under an id that sorts before "mediacodec_dec" so
  // CDVDFactoryCodec::CreateVideoCodec (which walks m_hwVideoCodecs, a
  // std::map, in key order) offers this codec Open() first. Open()
  // itself refuses anything that isn't Stereo High Profile, so for every
  // other stream this is a no-op fallthrough to mediacodec_dec exactly
  // as before.
  CDVDFactoryCodec::RegisterHWVideoCodec("edge264_mvc_dec", &CDVDVideoCodecMVCSoftware::Create);
  return true;
}

bool CDVDVideoCodecMVCSoftware::DetectStereoHighProfile(const CDVDStreamInfo& hints)
{
  // Primary path: AV_CODEC_ID_H264_MVC, a distinct codec id introduced by
  // the two FFmpeg patches this build integrates (tools/depends/target/
  // ffmpeg/008-.../009-...). Once those are applied, both the MPEG-TS
  // demuxer (STREAM_TYPE_VIDEO_MVC) and the Matroska demuxer (mvcC
  // BlockAddition) tag the stream with this id directly - the demuxer
  // has already done the disambiguation work, so no profile inspection
  // is needed here. This is also the only path that lines up with
  // ff_h264_mvc_parser actually being selected: FFmpeg picks a parser by
  // codec id (see libavcodec/parsers.c/PARSER_CODEC_LIST), and only
  // ff_h264_mvc_parser sets H264ParseContext.is_mvc, which is what makes
  // FFmpeg's own H.264 parser keep the dependent-view extension slices
  // (NAL type 20) attached to the same access unit as the base view
  // instead of splitting them off - without that, AddData() would only
  // ever see the base view regardless of what this function returns.
  if (hints.codec == AV_CODEC_ID_H264_MVC)
    return true;

  if (hints.codec != AV_CODEC_ID_H264)
    return false;

  // Fallback path: plain AV_CODEC_ID_H264 with Stereo High profile, for
  // sources that never go through the two demuxers patched above (e.g. a
  // raw elementary .264 stream demuxed generically) and therefore never
  // get remapped to AV_CODEC_ID_H264_MVC. AV_PROFILE_H264_STEREO_HIGH
  // (128); FFmpeg renamed the old FF_PROFILE_H264_* constants to
  // AV_PROFILE_H264_* in the 6.x/7.x cycle, this Kodi checkout vendors
  // FFmpeg 9.0.1, which only has the new name.
  if (hints.codec_tag == 0 && hints.profile == AV_PROFILE_H264_STEREO_HIGH)
    return true;

  // Last-resort fallback: scan extradata for a subset SPS (NAL unit type
  // 15). Kept for completeness, but in practice this rarely fires for
  // MKV-muxed BD 3D rips specifically - those carry the dependent view
  // via BlockAdditions (handled by the AV_CODEC_ID_H264_MVC path above),
  // not as an extra SPS inside the container's avcC extradata. This is a
  // minimal byte scan, not a real bitstream parser - it does not
  // validate emulation-prevention bytes and can in principle
  // false-positive on other streams that happen to contain the
  // NAL-type-15 byte pattern outside of a real start code. Good enough
  // for a PoC; a real implementation should reuse a proper Annex-B/AVCC
  // NAL walker (e.g. CBitstreamConverter's parsing helpers) instead.
  if (hints.extradata)
  {
    const uint8_t* data = hints.extradata.GetData();
    size_t size = hints.extradata.GetSize();
    for (size_t i = 0; i + 3 < size; i++)
    {
      bool startcode3 = data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1;
      if (!startcode3)
        continue;
      uint8_t nalType = data[i + 3] & 0x1F;
      if (nalType == 15) // subset SPS
        return true;
    }
  }

  return false;
}

bool CDVDVideoCodecMVCSoftware::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (!DetectStereoHighProfile(hints))
    return false;

  if (hints.width == 0 || hints.height == 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::Open - no size, cannot handle");
    return false;
  }

  m_hints = hints;
  m_width = hints.width;
  m_height = hints.height;

  // TODO: expose as a video setting; top-bottom halves vertical
  // resolution instead of horizontal and may suit some panels better.
  m_packMode = PackMode::SBS;

  // CBitstreamConverter's internal NAL/avcC-to-Annex-B logic switches
  // strictly on AV_CODEC_ID_H264 (see utils/BitstreamConverter.cpp) and
  // doesn't know AV_CODEC_ID_H264_MVC - it doesn't need to, since avcC
  // parsing and length-prefix-to-startcode rewriting are identical
  // regardless of the container-level MVC tagging. Passing the plain id
  // through avoids having to patch BitstreamConverter.cpp for something
  // it was already correct about.
  if (!m_bitstream.Open(AV_CODEC_ID_H264, hints.extradata.GetData(), hints.extradata.GetSize(),
                        true /* to_annexb */))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::Open - bitstream converter init failed");
    return false;
  }

  m_decoder = edge264_alloc(EDGE264_THREADS, &CDVDVideoCodecMVCSoftware::LogCb, this,
                             0 /* log_mbs */, &CDVDVideoCodecMVCSoftware::AllocCb,
                             &CDVDVideoCodecMVCSoftware::FreeCb, this);
  if (!m_decoder)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::Open - edge264_alloc failed");
    return false;
  }

  CLog::Log(LOGINFO,
             "CDVDVideoCodecMVCSoftware::Open - decoding {}x{} H.264 Stereo High (MVC) in "
             "software, packing as {}",
             m_width, m_height, m_packMode == PackMode::SBS ? "SBS" : "TAB");

  m_processInfo.SetVideoDecoderName(GetName(), false);
  m_processInfo.SetVideoDimensions(m_width, m_height);
  m_processInfo.SetVideoDeintMethod("none");
  m_processInfo.SetVideoDAR(hints.aspect);

  return true;
}

bool CDVDVideoCodecMVCSoftware::AddData(const DemuxPacket& packet)
{
  if (!m_decoder)
    return true;

  if (packet.pData == nullptr || packet.iSize == 0)
    return true;

  if (!m_bitstream.Convert(packet.pData, packet.iSize))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::AddData - bitstream conversion failed");
    return true;
  }

  m_ptsPending = packet.pts != DVD_NOPTS_VALUE ? packet.pts : packet.dts;

  const uint8_t* buf = m_bitstream.GetConvertBuffer();
  const uint8_t* end = buf + m_bitstream.GetConvertSize();

  // Feed every NAL of this access unit to the decoder. edge264 expects
  // Annex-B start codes, which is why we ran the bitstream converter
  // above (mirrors how CDVDVideoCodecAndroidMediaCodec handles MPEG2/VC1
  // elsewhere in this codebase).
  const uint8_t* nal = edge264_find_start_code(buf, end, 1);
  while (nal < end)
  {
    const uint8_t* nextStart = edge264_find_start_code(nal + 1, end, 1);
    const uint8_t* nalEnd = nextStart < end ? nextStart : end;

    int ret = edge264_decode_NAL(m_decoder, nal, nalEnd, nullptr, nullptr);
    if (ret < 0 && ret != -ENOBUFS)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::AddData - edge264_decode_NAL returned {}",
                ret);
    }

    nal = nextStart;
  }

  // Pull at most one frame per AddData call; GetPicture() drives further
  // draining. A production version should decouple decode and drain more
  // carefully around edge264's internal reordering/DPB behaviour.
  if (!m_hasPicture)
  {
    Edge264Frame frame{};
    if (edge264_get_frame(m_decoder, &frame, 0) == 0 && frame.samples[0] && frame.samples_mvc[0])
    {
      m_picture.Reset();
      if (PackFrame(frame, &m_picture))
      {
        m_hasPicture = true;
        m_pts = m_ptsPending;
      }
    }
  }

  return true;
}

void CDVDVideoCodecMVCSoftware::Reset()
{
  if (m_decoder)
    edge264_flush(m_decoder);
  m_hasPicture = false;
  m_pts = DVD_NOPTS_VALUE;
  m_ptsPending = DVD_NOPTS_VALUE;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecMVCSoftware::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_hasPicture)
    return VC_BUFFER;

  // m_picture was built by PackFrame() in AddData(); hand its buffer
  // reference to the caller. Ownership/reuse pooling and pacing against
  // DVD_CODEC_CTRL_DROP flags are unaddressed in this PoC.
  pVideoPicture->CopyRef(m_picture);
  pVideoPicture->pts = m_pts;
  pVideoPicture->dts = m_pts;
  m_hasPicture = false;
  return VC_PICTURE;
}

bool CDVDVideoCodecMVCSoftware::PackFrame(const Edge264Frame& frame, VideoPicture* pVideoPicture)
{
  if (frame.width_Y != static_cast<int>(m_width) || frame.height_Y != static_cast<int>(m_height))
  {
    CLog::Log(LOGWARNING, "CDVDVideoCodecMVCSoftware::PackFrame - size mismatch {}x{} vs {}x{}",
              frame.width_Y, frame.height_Y, m_width, m_height);
  }

  const unsigned int packedWidth = m_packMode == PackMode::SBS ? m_width : m_width;
  const unsigned int packedHeight = m_packMode == PackMode::SBS ? m_height : m_height;

  IVideoBufferPool* pool = nullptr;
  CVideoBuffer* buffer =
      m_processInfo.GetVideoBufferManager().Get(AV_PIX_FMT_YUV420P, packedWidth * packedHeight * 3 / 2, &pool);
  if (!buffer)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::PackFrame - failed to get video buffer");
    return false;
  }

  uint8_t* planes[YuvImage::MAX_PLANES];
  int strides[YuvImage::MAX_PLANES];
  buffer->GetPlanes(planes);
  buffer->GetStrides(strides);

  // Half-SBS / half-TAB packing: each eye is downscaled 2:1 with a 2-tap
  // box filter (see per-branch comments below) to half width (SBS) or
  // half height (TAB) and placed side by side, so the packed frame keeps
  // the source resolution and matches the layout Kodi's
  // stereoMode="left_right"/"top_bottom" renderer path already expects
  // for half-SBS/half-TAB files - no renderer changes needed.
  for (int plane = 0; plane < 3; plane++)
  {
    const uint8_t* srcL = frame.samples[plane];
    const uint8_t* srcR = frame.samples_mvc[plane];
    int srcStride = plane == 0 ? frame.stride_Y : frame.stride_C;
    int srcW = plane == 0 ? frame.width_Y : frame.width_C;
    int srcH = plane == 0 ? frame.height_Y : frame.height_C;

    uint8_t* dst = planes[plane];
    int dstStride = strides[plane];

    if (m_packMode == PackMode::SBS)
    {
      // 2-tap horizontal box filter (average of each adjacent pixel pair)
      // instead of nearest-neighbour line-skip: line-skip aliases badly
      // on a squeeze this aggressive (half horizontal resolution), losing
      // fine vertical detail (subtitles, on-screen text) to a moire-like
      // pattern. A simple box filter is the right amount of "proper
      // scaler" for a fixed, always-exactly-2:1 downscale - no need to
      // pull in swscale for this one ratio.
      int halfW = srcW / 2;
      for (int y = 0; y < srcH; y++)
      {
        uint8_t* row = dst + static_cast<size_t>(y) * dstStride;
        const uint8_t* rowL = srcL + static_cast<size_t>(y) * srcStride;
        const uint8_t* rowR = srcR + static_cast<size_t>(y) * srcStride;
        for (int x = 0; x < halfW; x++)
        {
          row[x] = static_cast<uint8_t>((rowL[x * 2] + rowL[x * 2 + 1] + 1) >> 1);
          row[halfW + x] = static_cast<uint8_t>((rowR[x * 2] + rowR[x * 2 + 1] + 1) >> 1);
        }
        // odd source width: carry the last column through unfiltered
        if (srcW & 1)
        {
          row[halfW - 1] = rowL[srcW - 1];
          row[halfW + halfW - 1] = rowR[srcW - 1];
        }
      }
    }
    else // TAB
    {
      // 2-tap vertical box filter, same rationale as the SBS branch
      // above: averaging both source rows instead of dropping every
      // other row keeps fine horizontal detail (subtitle strokes, thin
      // horizontal lines) from aliasing on the vertical squeeze.
      int halfH = srcH / 2;
      auto packHalf = [&](const uint8_t* src, uint8_t* out) {
        for (int y = 0; y < halfH; y++)
        {
          const uint8_t* rowTop = src + static_cast<size_t>(y * 2) * srcStride;
          const uint8_t* rowBot = rowTop + srcStride;
          uint8_t* dstRow = out + static_cast<size_t>(y) * dstStride;
          for (int x = 0; x < srcW; x++)
            dstRow[x] = static_cast<uint8_t>((rowTop[x] + rowBot[x] + 1) >> 1);
        }
      };
      packHalf(srcL, dst);
      packHalf(srcR, dst + static_cast<size_t>(halfH) * dstStride);
    }
  }

  pVideoPicture->Reset();
  pVideoPicture->videoBuffer = buffer;
  pVideoPicture->iWidth = packedWidth;
  pVideoPicture->iHeight = packedHeight;
  pVideoPicture->iDisplayWidth = packedWidth;
  pVideoPicture->iDisplayHeight = packedHeight;
  pVideoPicture->stereoMode = m_packMode == PackMode::SBS ? "left_right" : "top_bottom";
  pVideoPicture->iFlags = 0;
  pVideoPicture->color_range = 0;
  pVideoPicture->colorBits = 8;

  return true;
}

void* CDVDVideoCodecMVCSoftware::AllocFromPool(std::vector<PooledBlock>& pool, unsigned size)
{
  for (size_t i = 0; i < pool.size(); i++)
  {
    if (pool[i].size == size)
    {
      void* ptr = pool[i].ptr;
      pool.erase(pool.begin() + i);
      return ptr;
    }
  }

  void* ptr = std::malloc(size);
  if (ptr)
    m_blockSizes[ptr] = size;
  return ptr;
}

void CDVDVideoCodecMVCSoftware::ReleaseToPool(std::vector<PooledBlock>& pool, void* ptr)
{
  if (!ptr)
    return;

  auto it = m_blockSizes.find(ptr);
  if (it == m_blockSizes.end())
  {
    // Should not happen - we recorded a size for every block we ever
    // handed out. Free rather than leak if it does.
    std::free(ptr);
    return;
  }

  if (pool.size() >= MAX_POOLED_BLOCKS)
  {
    std::free(ptr);
    m_blockSizes.erase(it);
    return;
  }

  pool.push_back({ptr, it->second});
}

void CDVDVideoCodecMVCSoftware::DrainPool(std::vector<PooledBlock>& pool)
{
  for (auto& block : pool)
    std::free(block.ptr);
  pool.clear();
}

void CDVDVideoCodecMVCSoftware::AllocCb(
    void** samples, unsigned samples_size, void** mbs, unsigned mbs_size, int errno_on_fail, void* alloc_arg)
{
  auto* self = static_cast<CDVDVideoCodecMVCSoftware*>(alloc_arg);
  *samples = self->AllocFromPool(self->m_samplesPool, samples_size);
  *mbs = self->AllocFromPool(self->m_mbsPool, mbs_size);
  if ((!*samples || !*mbs) && errno_on_fail)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::AllocCb - allocation failed ({} + {} bytes)",
              samples_size, mbs_size);
  }
}

void CDVDVideoCodecMVCSoftware::FreeCb(void* samples, void* mbs, void* alloc_arg)
{
  auto* self = static_cast<CDVDVideoCodecMVCSoftware*>(alloc_arg);
  self->ReleaseToPool(self->m_samplesPool, samples);
  self->ReleaseToPool(self->m_mbsPool, mbs);
}

int CDVDVideoCodecMVCSoftware::LogCb(const char* str, void* log_arg)
{
  CLog::Log(LOGDEBUG, "edge264-mvc: {}", str);
  return 0;
}
