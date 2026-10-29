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
#include "rendering/RenderSystemTypes.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#if defined(TARGET_POSIX)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
// edge264 wants to be told how many worker threads to use; the value has
// no relation to Kodi's own thread pools. Four is a starting point for a
// quad-core-class ARM part (Tegra X1: 4x Cortex-A57 + 4x Cortex-A53) and
// should be benchmarked, not trusted - this is exactly the "real-time
// feasibility" question that needs measuring on actual hardware before
// this codec is anything more than a PoC.
constexpr int EDGE264_THREADS = 4;
constexpr int EDGE264_WORKER_NICE = 6;

class CScopedThreadNice
{
public:
  explicit CScopedThreadNice(int nice)
  {
#if defined(TARGET_POSIX)
    m_tid = static_cast<id_t>(syscall(SYS_gettid));
    errno = 0;
    const int current = getpriority(PRIO_PROCESS, m_tid);
    if (errno == 0 && setpriority(PRIO_PROCESS, m_tid, nice) == 0)
    {
      m_previous = current;
      m_changed = true;
    }
#endif
  }

  ~CScopedThreadNice()
  {
#if defined(TARGET_POSIX)
    if (m_changed)
      setpriority(PRIO_PROCESS, m_tid, m_previous);
#endif
  }

  CScopedThreadNice(const CScopedThreadNice&) = delete;
  CScopedThreadNice& operator=(const CScopedThreadNice&) = delete;

private:
  id_t m_tid{0};
  int m_previous{0};
  bool m_changed{false};
};

int64_t PtsToInt(double pts)
{
  int64_t value;
  std::memcpy(&value, &pts, sizeof(value));
  return value;
}

double IntToPts(int64_t value)
{
  double pts;
  std::memcpy(&pts, &value, sizeof(pts));
  return pts;
}

const uint8_t* FindStartCode(const uint8_t* buf, const uint8_t* end)
{
  return buf + edge264mvc_find_start_code(buf, static_cast<size_t>(end - buf));
}
} // namespace

CDVDVideoCodecMVCSoftware::CDVDVideoCodecMVCSoftware(CProcessInfo& processInfo)
  : CDVDVideoCodec(processInfo)
{
}

CDVDVideoCodecMVCSoftware::~CDVDVideoCodecMVCSoftware()
{
  edge264mvc_close(&m_decoder);
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

  // CDVDDemuxBluray3D merges both views into one access unit and flags the stream
  if (hints.multiview)
    return true;

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
  const auto appSettings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const auto isFrameSequential = [&appSettings](const char* id)
  { return appSettings->GetInt(id) == static_cast<int>(RenderStereoMode::FRAME_SEQUENTIAL); };
  m_fullResolution =
      appSettings->GetInt(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICTABRESOLUTION) == 2160 ||
      isFrameSequential(CSettings::SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE) ||
      isFrameSequential(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE);
  // A Blu-ray playlist may code the right eye as the base view
  m_baseViewIsRightEye = hints.stereo_mode == "right_left" || hints.stereo_mode == "block_rl";

  // Only avcC needs rewriting; MPEG-TS (Blu-ray) packets and extradata are Annex-B already
  m_annexB = !(hints.extradata && hints.extradata.GetSize() >= 7 &&
               hints.extradata.GetData()[0] == 1);
  if (!m_annexB && !m_bitstream.Open(AV_CODEC_ID_H264, hints.extradata.GetData(),
                                     hints.extradata.GetSize(), true /* to_annexb */))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::Open - bitstream converter init failed");
    return false;
  }

  Edge264MvcSettings settings;
  edge264mvc_default_settings(&settings);
  settings.n_threads = EDGE264_THREADS;
  int openRet;
  {
    CScopedThreadNice workerPriority(EDGE264_WORKER_NICE);
    openRet = edge264mvc_open(&m_decoder, &settings);
  }
  if (openRet != EDGE264MVC_OK || !m_decoder)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::Open - edge264mvc_open failed ({})", openRet);
    m_decoder = nullptr;
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

bool CDVDVideoCodecMVCSoftware::Reconfigure(CDVDStreamInfo& hints)
{
  // Carries the decoder over a clip change of a disc, so that the player does not rebuild
  // it - and with it the stereo mode it has announced, which would read as a 2D title
  // for the moment it takes the first picture to arrive, and switch the display twice.
  if (!m_decoder || !DetectStereoHighProfile(hints))
    return false;

  const bool annexB = !(hints.extradata && hints.extradata.GetSize() >= 7 &&
                        hints.extradata.GetData()[0] == 1);
  if (!annexB)
    return false;

  // A clip handed over before it is parsed has no size yet; one that has differs or not
  const bool sized = hints.width > 0 && hints.height > 0;
  if (sized && (static_cast<unsigned int>(hints.width) != m_width ||
                static_cast<unsigned int>(hints.height) != m_height))
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::Reconfigure - {}x{} does not fit {}x{}",
              hints.width, hints.height, m_width, m_height);
    return false;
  }

  const bool baseViewIsRightEye =
      hints.stereo_mode == "right_left" || hints.stereo_mode == "block_rl";
  if (baseViewIsRightEye != m_baseViewIsRightEye)
    return false;

  if (!sized)
  {
    hints.width = static_cast<int>(m_width);
    hints.height = static_cast<int>(m_height);
  }
  m_hints = hints;

  // The clips join at an access unit boundary and the last one was drained on close, so
  // nothing is lost by starting the new one clean
  Reset();

  CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::Reconfigure - carrying on with the next clip");
  return true;
}

bool CDVDVideoCodecMVCSoftware::AddData(const DemuxPacket& packet)
{
  if (!m_decoder)
    return true;

  if (packet.pData == nullptr || packet.iSize == 0)
    return true;

  const uint8_t* buf = packet.pData;
  const uint8_t* end = buf + packet.iSize;
  if (!m_annexB)
  {
    if (!m_bitstream.Convert(packet.pData, packet.iSize))
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::AddData - bitstream conversion failed");
      return true;
    }
    buf = m_bitstream.GetConvertBuffer();
    end = buf + m_bitstream.GetConvertSize();
  }

  const int64_t pts = PtsToInt(packet.pts != DVD_NOPTS_VALUE ? packet.pts : packet.dts);

  auto decode = [this, pts](const uint8_t* start, const uint8_t* stop)
  {
    const int ret = DecodeNal(start, static_cast<size_t>(stop - start), pts);
    if (ret == EDGE264MVC_NOMEM)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecMVCSoftware::AddData - out of memory, dropping a NAL");
    }
    else if (ret != EDGE264MVC_OK)
    {
      CLog::Log(LOGDEBUG, "CDVDVideoCodecMVCSoftware::AddData - edge264mvc_send_nal returned {}",
                ret);
    }
  };

  // The base view's end_of_seq/end_of_stream precedes the dependent view's slices in
  // a merged access unit, and edge264 clears its parameter sets on it - feed it last
  std::vector<std::pair<const uint8_t*, const uint8_t*>> trailing;

  // edge264mvc_find_start_code() points at the "00 00 01" itself; a 4-byte start
  // code is found by its last three bytes, so skipping 3 is right for both
  const uint8_t* nal = FindStartCode(buf, end);
  if (nal < end)
  {
    nal += 3;
    while (nal < end)
    {
      const uint8_t* nalEnd = FindStartCode(nal, end);

      const uint8_t nalType = nal[0] & 0x1f;
      if (nalType == 10 || nalType == 11)
        trailing.emplace_back(nal, nalEnd);
      else
        decode(nal, nalEnd);

      if (nalEnd >= end)
        break;
      nal = nalEnd + 3;
    }
  }

  for (const auto& [start, stop] : trailing)
    decode(start, stop);

  DrainFrames();

  return true;
}

int CDVDVideoCodecMVCSoftware::DecodeNal(const uint8_t* nal, size_t size, int64_t pts)
{
  int ret = edge264mvc_send_nal(m_decoder, nal, size, pts, 0);
  while (ret == EDGE264MVC_AGAIN)
  {
    DrainFrames();
    ret = edge264mvc_send_nal(m_decoder, nal, size, pts, 0);
  }
  return ret;
}

int CDVDVideoCodecMVCSoftware::DrainFrames()
{
  int count = 0;
  Edge264MvcFrame frame{};
  while (edge264mvc_receive_frame(m_decoder, &frame) == EDGE264MVC_OK)
  {
    if (frame.views[0].planes[0])
    {
      auto picture = std::make_unique<VideoPicture>();
      if (PackFrame(frame, picture.get()))
      {
        picture->pts = IntToPts(frame.views[0].pts);
        picture->dts = picture->pts;
        m_pictures.push_back(std::move(picture));
        count++;
      }
    }
    edge264mvc_release_frame(m_decoder, &frame);
  }
  return count;
}

void CDVDVideoCodecMVCSoftware::Reset()
{
  if (m_decoder)
    edge264mvc_flush(m_decoder);
  m_pictures.clear();
}

CDVDVideoCodec::VCReturn CDVDVideoCodecMVCSoftware::GetPicture(VideoPicture* pVideoPicture)
{
  // Draining: nothing more is coming to push the held frames out
  if (m_pictures.empty() && m_decoder && (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
  {
    edge264mvc_send_end(m_decoder);
    DrainFrames();
  }

  if (m_pictures.empty())
    return VC_BUFFER;

  pVideoPicture->CopyRef(*m_pictures.front());
  m_pictures.pop_front();
  return VC_PICTURE;
}

bool CDVDVideoCodecMVCSoftware::PackFrame(const Edge264MvcFrame& frame, VideoPicture* pVideoPicture)
{
  if (frame.width_Y != static_cast<int>(m_width) || frame.height_Y != static_cast<int>(m_height))
  {
    CLog::Log(LOGWARNING, "CDVDVideoCodecMVCSoftware::PackFrame - size mismatch {}x{} vs {}x{}",
              frame.width_Y, frame.height_Y, m_width, m_height);
  }

  const unsigned int packedWidth =
      m_packMode == PackMode::SBS && m_fullResolution ? m_width * 2 : m_width;
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
  // Idents, menus and the run-up after a seek carry no dependent view: show the base
  // view to both eyes rather than drop to 2D, which would cost a stereo mode switch
  const bool stereo = frame.views[1].planes[0] != nullptr;
  for (int plane = 0; plane < 3; plane++)
  {
    const uint8_t* srcL = frame.views[0].planes[plane];
    const uint8_t* srcR = stereo ? frame.views[1].planes[plane] : srcL;
    int srcStride = plane == 0 ? frame.stride_Y : frame.stride_C;
    int srcW = plane == 0 ? frame.width_Y : frame.width_C;
    int srcH = plane == 0 ? frame.height_Y : frame.height_C;

    uint8_t* dst = planes[plane];
    int dstStride = strides[plane];

    if (m_packMode == PackMode::SBS && m_fullResolution)
    {
      for (int y = 0; y < srcH; y++)
      {
        uint8_t* row = dst + static_cast<size_t>(y) * dstStride;
        std::memcpy(row, srcL + static_cast<size_t>(y) * srcStride, srcW);
        std::memcpy(row + srcW, srcR + static_cast<size_t>(y) * srcStride, srcW);
      }
    }
    else if (m_packMode == PackMode::SBS)
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
  if (m_packMode == PackMode::SBS)
    pVideoPicture->stereoMode = m_baseViewIsRightEye ? "right_left" : "left_right";
  else
    pVideoPicture->stereoMode = m_baseViewIsRightEye ? "bottom_top" : "top_bottom";
  pVideoPicture->iFlags = 0;
  pVideoPicture->color_range = 0;
  pVideoPicture->colorBits = 8;

  return true;
}
