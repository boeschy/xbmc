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
#include "cores/VideoPlayer/DVDDemuxers/DemuxStreamSSIF.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <set>

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
    // which returns each block to m_framePool rather than freeing it
    // immediately (see ReleaseToPool()) - so the actual std::free() call
    // only happens afterwards, here.
    edge264_free(&m_decoder);
  }
  DrainPool(m_framePool);
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

  // BD-3D via a separate dependent-view clip demuxer (see
  // CDVDInputStreamBluray's m_bMVCPlayback comment and DemuxStreamSSIF.h): the base view
  // and dependent view are demuxed from two independent AVFormatContexts and merged into
  // single access units at the Kodi packet level, so codec_id here legitimately stays
  // plain AV_CODEC_ID_H264 the whole time (see CDVDDemuxFFmpeg::SetupMVCMerge() for
  // exactly why retagging codec_id itself does not work for an MPEG-TS source - it gets
  // silently reset by the demuxer's own repeated PMT parsing). codec_tag is the only
  // signal available for this path.
  if (hints.codec == AV_CODEC_ID_H264 && hints.codec_tag == BD3D_MVC_CODEC_TAG)
    return true;

  if (hints.codec != AV_CODEC_ID_H264)
    return false;

  // MKV path (the actual demuxer path for this PoC's test files): the
  // Matroska demuxer patch (009-ffmpeg-mkv-mvcc-block-addition.patch,
  // mkv_parse_mvcc() in libavformat/matroskadec.c) deliberately does NOT
  // touch st->codecpar->codec_id - it only appends the raw mvcC
  // BlockAddition config record to codecpar->extradata, tagged with a
  // fixed marker: a 4-byte fill (0xfdf8f800), a 4-byte length, and the
  // 4-byte 'mvcC' fourcc (0x6d766343), followed by the config bytes
  // themselves. So for MKV sources codec_id stays plain AV_CODEC_ID_H264
  // and profile stays whatever the base-view SPS alone reports (usually
  // High, not Stereo High) - this marker in extradata is the only
  // reliable signal available. Confirmed against a real capture: a
  // 252-byte base avcC grew to 436 bytes (+184, i.e. 12-byte marker
  // header + a 172-byte mvcC config record) once 009 was applied.
  if (hints.extradata)
  {
    const uint8_t* data = hints.extradata.GetData();
    size_t size = hints.extradata.GetSize();
    for (size_t i = 0; i + 12 <= size; i++)
    {
      uint32_t fill = (static_cast<uint32_t>(data[i]) << 24) | (static_cast<uint32_t>(data[i + 1]) << 16) |
                      (static_cast<uint32_t>(data[i + 2]) << 8) | data[i + 3];
      if (fill != 0xfdf8f800)
        continue;

      uint32_t fourcc = (static_cast<uint32_t>(data[i + 8]) << 24) | (static_cast<uint32_t>(data[i + 9]) << 16) |
                        (static_cast<uint32_t>(data[i + 10]) << 8) | data[i + 11];
      if (fourcc == 0x6d766343) // 'mvcC'
        return true;
    }
  }

  // Fallback path: plain AV_CODEC_ID_H264 with Stereo High profile, for
  // sources that never go through either patched demuxer (e.g. a raw
  // elementary .264 stream demuxed generically) and therefore never get
  // remapped/tagged at all. AV_PROFILE_H264_STEREO_HIGH (128); FFmpeg
  // renamed the old FF_PROFILE_H264_* constants to AV_PROFILE_H264_* in
  // the 6.x/7.x cycle, this Kodi checkout vendors FFmpeg 9.0.1, which
  // only has the new name.
  if (hints.codec_tag == 0 && hints.profile == AV_PROFILE_H264_STEREO_HIGH)
    return true;

  // Last-resort fallback: scan extradata for a subset SPS (NAL unit type
  // 15) sitting directly in the base avcC SPS list, Annex-B style. Kept
  // for completeness/other muxing conventions this PoC hasn't seen; it
  // will not find anything for the mvcC-tagged MKV case above, since
  // that config record uses its own internal (non-Annex-B) framing, not
  // 00 00 01 start codes. This is a minimal byte scan, not a real
  // bitstream parser - it does not validate emulation-prevention bytes
  // and can in principle false-positive on other streams that happen to
  // contain the NAL-type-15 byte pattern outside of a real start code.
  // Good enough for a PoC; a real implementation should reuse a proper
  // Annex-B/AVCC NAL walker (e.g. CBitstreamConverter's parsing helpers)
  // instead.
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
    // Not fatal. This path was only ever verified against Matroska's avcC-formatted
    // extradata (see DetectStereoHighProfile() above) - CBitstreamConverter::Open()
    // requires extradata that starts with the avcC configurationVersion byte (0x01) and
    // fails otherwise, which is exactly what happens for a BD/MPEG-TS elementary stream:
    // those carry SPS/PPS in-band, repeated before every IDR, rather than in a
    // container-level avcC record, so hints.extradata here is typically empty or already
    // Annex-B. Either way, m_bitstream.Open() still runs its first two statements
    // (m_to_annexb = true; m_codec = AV_CODEC_ID_H264;) before hitting that check, and its
    // m_convert_bitstream flag defaults to false and is only ever set true inside the
    // avcC branch we didn't take - which is exactly the internal state
    // Convert()/GetConvertBuffer() need to pass Annex-B packets through unchanged instead
    // of (wrongly) trying to convert them. A genuinely malformed stream would misbehave
    // silently here rather than via this return value; not handled, matching the same
    // unverified-edge-case scope already accepted by the avcC path above.
    CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::Open - no avcC extradata, treating input "
                         "as already-Annex-B (BD/MPEG-TS elementary stream)");
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
  // Queue every fed access unit's pts in decode order; consumed below
  // (smallest-first) once a frame actually comes out, not when it was
  // fed - see the m_ptsQueue comment in the header for why.
  m_ptsQueue.insert(m_ptsPending);

  const uint8_t* buf = m_bitstream.GetConvertBuffer();
  const uint8_t* end = buf + m_bitstream.GetConvertSize();

  // Feed every NAL of this access unit to the decoder. edge264 expects
  // Annex-B start codes, which is why we ran the bitstream converter
  // above (mirrors how CDVDVideoCodecAndroidMediaCodec handles MPEG2/VC1
  // elsewhere in this codebase).
  //
  // edge264_find_start_code() returns a pointer to the start of the
  // "00 00 01"/"00 00 00 01" delimiter itself, NOT past it - passing
  // that pointer straight to edge264_decode_NAL() (as an earlier version
  // of this function did) feeds it the delimiter's leading zero byte as
  // if it were the NAL header, which decodes to nal_unit_type 0
  // ("Unknown") and an ENOTSUP from every single NAL. The delimiter has
  // to be skipped manually first - "nal[2] == 0" distinguishes a 3-byte
  // (00 00 01) from a 4-byte (00 00 00 01) delimiter, exactly matching
  // the reference decode loop in edge264-mvc's own src/edge264_test.c.
  const uint8_t* nal = edge264_find_start_code(buf, end, 1);
  if (nal < end)
  {
    nal += 3 + (nal[2] == 0);
    while (nal < end)
    {
      const uint8_t* nalEnd = edge264_find_start_code(nal, end, 0);

      // edge264 uses plain (non-negated) errno-style return codes - 0 on
      // success, positive errno values (ENOBUFS, ENOTSUP, ...)
      // otherwise - unlike FFmpeg's negative-errno convention. ENOBUFS
      // just means the DPB is temporarily full and is expected during
      // normal decode, not an error worth logging.
      int ret = edge264_decode_NAL(m_decoder, nal, nalEnd, nullptr, nullptr);
      if (ret != 0 && ret != ENOBUFS)
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::AddData - edge264_decode_NAL returned {}",
                  ret);
      }

      if (nalEnd >= end)
        break;
      nal = nalEnd + 3 + (nalEnd[2] == 0);
    }
  }

  // Pull at most one frame per AddData call; GetPicture() drives further
  // draining. A production version should decouple decode and drain more
  // carefully around edge264's internal reordering/DPB behaviour.
  if (!m_hasPicture)
  {
    Edge264Frame frame{};
    // borrow=1: with borrow=0, edge264_get_frame() immediately clears the
    // DPB slot's "still owned by caller" bit *inside the call itself*,
    // before PackFrame() below has copied a single pixel out of
    // frame.samples/samples_mvc - and edge264's n_threads=4 worker pool
    // keeps decoding in the background the whole time (visibly, in
    // practice: workers are often already multiple frames ahead by the
    // time this runs), so that freed buffer can get reused and
    // overwritten by a worker thread mid-copy. That's a genuine data
    // race, not a hypothetical one - it explains a decode that runs
    // cleanly for a while (pure thread-timing luck) and then corrupts/
    // crashes once a worker catches up at the wrong moment. borrow=1
    // keeps the slot reserved until we explicitly release it below.
    if (edge264_get_frame(m_decoder, &frame, 1) == 0 && frame.samples[0] && frame.samples_mvc[0])
    {
      m_picture.Reset();
      if (PackFrame(frame, &m_picture))
      {
        m_hasPicture = true;
        // Pop the smallest still-pending pts, not m_ptsPending (decode
        // order) - see m_ptsQueue's declaration for why. Guard against an
        // empty queue defensively; it should be impossible (one insert
        // per AddData() call, one erase per frame out) but a stray/odd
        // stream is not worth a crash over.
        if (!m_ptsQueue.empty())
        {
          m_pts = *m_ptsQueue.begin();
          m_ptsQueue.erase(m_ptsQueue.begin());
        }
        else
        {
          m_pts = m_ptsPending;
        }
      }
      // Only safe to give the slot back now that PackFrame() has
      // finished copying every plane out of it.
      edge264_return_frame(m_decoder, frame.return_arg);
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
  m_ptsQueue.clear();
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

  // Get() only reserves a block of storage sized for the format above -
  // it does NOT configure the buffer's plane pointers/strides.
  // CVideoBufferSysMem zero-initializes its YuvImage in its constructor
  // (see VideoBuffer.cpp), so without an explicit SetDimensions() call
  // here, GetPlanes()/GetStrides() below return {nullptr, nullptr,
  // nullptr} / {0, 0, 0} on a freshly allocated buffer, and the copy
  // loop writes straight through a null pointer on literally the first
  // packed frame - a guaranteed SIGSEGV that happens before a single
  // picture is ever handed to the renderer, which is why it isn't
  // preceded by anything more specific in Kodi's own log. Every other
  // caller of GetVideoBufferManager().Get() in this codebase
  // (AddonVideoCodec::GetFrameBuffer, CDVDVideoPPFFmpeg::Process) calls
  // SetDimensions() for the same reason before touching the planes.
  const int initialStrides[YuvImage::MAX_PLANES] = {
      static_cast<int>(packedWidth), static_cast<int>(packedWidth) / 2, static_cast<int>(packedWidth) / 2};
  buffer->SetDimensions(static_cast<int>(packedWidth), static_cast<int>(packedHeight), initialStrides);

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
  // Called from AllocCb, which edge264 invokes from whichever of its own
  // worker threads needs a new DPB slot - never assume single-threaded
  // access here.
  std::lock_guard<std::mutex> lock(m_poolMutex);

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

  // Same rationale as AllocFromPool(): FreeCb is called from edge264's
  // worker threads too.
  std::lock_guard<std::mutex> lock(m_poolMutex);

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
  // Only called from the destructor, after edge264_free() has already
  // joined every worker thread (see edge264.c's shutdown path) - no
  // concurrent AllocCb/FreeCb calls can be in flight here, so no lock
  // needed despite this being the counterpart to the two locked
  // functions above.
  for (auto& block : pool)
    std::free(block.ptr);
  pool.clear();
}

void CDVDVideoCodecMVCSoftware::AllocCb(
    void** samples, unsigned samples_size, void** mbs, unsigned mbs_size, int errno_on_fail, void* alloc_arg)
{
  auto* self = static_cast<CDVDVideoCodecMVCSoftware*>(alloc_arg);
  // One contiguous allocation for samples+mbs - see the m_framePool
  // comment in the header for why this must not be two separate
  // allocations (edge264's SIMD inter-prediction reads a short distance
  // past the nominal samples region, relying on it spilling harmlessly
  // into the immediately-following mbs region of the same block).
  void* base = self->AllocFromPool(self->m_framePool, samples_size + mbs_size);
  *samples = base;
  *mbs = base ? static_cast<uint8_t*>(base) + samples_size : nullptr;
  if (!base && errno_on_fail)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::AllocCb - allocation failed ({} + {} bytes)",
              samples_size, mbs_size);
  }
}

void CDVDVideoCodecMVCSoftware::FreeCb(void* samples, void* mbs, void* alloc_arg)
{
  auto* self = static_cast<CDVDVideoCodecMVCSoftware*>(alloc_arg);
  // mbs is just samples+samples_size within the same allocation (see
  // AllocCb) - only samples is ever handed back to the pool/freed,
  // exactly like edge264's own internal_free() only frees the base
  // pointer it originally returned as *samples.
  self->ReleaseToPool(self->m_framePool, samples);
}

int CDVDVideoCodecMVCSoftware::LogCb(const char* str, void* log_arg)
{
  CLog::Log(LOGDEBUG, "edge264-mvc: {}", str);
  return 0;
}
