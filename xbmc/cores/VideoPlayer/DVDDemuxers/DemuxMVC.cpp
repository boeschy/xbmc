/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - see DemuxMVC.h/DemuxStreamSSIF.h for scope.
 */

#include "DemuxMVC.h"

#include "DVDDemuxUtils.h"
#include "cores/FFmpeg.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

extern "C"
{
#include "libavutil/opt.h"
}

#include <cerrno>
#include <cstring>

namespace
{
// libbluray seeks the base view to an EP-map I-frame up to one GOP (normally <= 1 s) before
// the target; the dependent view has to end up before that point (90 kHz ticks, 2 s).
constexpr int64_t MVC_SEEK_TIME_WINDOW = 180000;
} // namespace

static int mvc_file_read(void* h, uint8_t* buf, int size)
{
  CDVDInputStream* pInputStream = static_cast<CDemuxMVC*>(h)->m_pInput;
  int s = pInputStream->Read(buf, size);

  if (pInputStream->IsEOF())
    return AVERROR_EOF;

  return s;
}

static int64_t mvc_file_seek(void* h, int64_t pos, int whence)
{
  CDVDInputStream* pInputStream = static_cast<CDemuxMVC*>(h)->m_pInput;
  if (whence == AVSEEK_SIZE)
    return pInputStream->GetLength();

  return pInputStream->Seek(pos, whence & ~AVSEEK_FORCE);
}

CDemuxMVC::CDemuxMVC() = default;

CDemuxMVC::~CDemuxMVC()
{
  Dispose();
}

bool CDemuxMVC::Open(CDVDInputStream* pInput)
{
  if (!pInput)
    return false;
  m_pInput = pInput;

  int bufferSize = 4096;
  int blockSize = m_pInput->GetBlockSize();
  if (blockSize > 1)
    bufferSize = blockSize;

  auto* buffer = static_cast<unsigned char*>(av_malloc(bufferSize));
  m_ioContext = avio_alloc_context(buffer, bufferSize, 0, this, mvc_file_read, nullptr, mvc_file_seek);

  m_pFormatContext = avformat_alloc_context();
  m_pFormatContext->pb = m_ioContext;

  const AVInputFormat* format = av_find_input_format("mpegts");
  int ret = avformat_open_input(&m_pFormatContext, m_pInput->GetFileName().c_str(), format, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGDEBUG, "CDemuxMVC::Open - opening MVC demuxing context failed ({})", ret);
    Dispose();
    return false;
  }

  av_opt_set_int(m_pFormatContext, "analyzeduration", 500000, 0);
  av_opt_set_int(m_pFormatContext, "correct_ts_overflow", 0, 0);

  // avformat_find_stream_info() on a bare dependent-view TS reliably returns < 0 (there is
  // no audio/PAT-adjacent context for it to lock onto beyond the one video PID) even though
  // it has already populated everything this demuxer needs (codec id + extradata on the
  // stream it does find) - so the return value is deliberately not treated as fatal here.
  avformat_find_stream_info(m_pFormatContext, nullptr);

  av_dump_format(m_pFormatContext, 0, m_pInput->GetFileName().c_str(), 0);

  CLog::Log(LOGDEBUG, "CDemuxMVC::Open - dependent-view clip has {} stream(s)",
            m_pFormatContext->nb_streams);
  for (unsigned int i = 0; i < m_pFormatContext->nb_streams; i++)
  {
    if (m_pFormatContext->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264_MVC)
    {
      m_nStreamIndex = i;
      break;
    }
    m_pFormatContext->streams[i]->discard = AVDISCARD_ALL;
  }

  if (m_nStreamIndex < 0)
  {
    CLog::Log(LOGDEBUG, "CDemuxMVC::Open - no H.264/MVC stream found in dependent-view clip");
    Dispose();
    return false;
  }

  return true;
}

bool CDemuxMVC::Reset()
{
  CDVDInputStream* pInput = m_pInput;
  Dispose();
  return Open(pInput);
}

void CDemuxMVC::Abort()
{
}

void CDemuxMVC::Flush()
{
  if (m_pFormatContext)
  {
    if (m_pFormatContext->pb)
      avio_flush(m_pFormatContext->pb);
    avformat_flush(m_pFormatContext);
  }
}

DemuxPacket* CDemuxMVC::Read()
{
  DemuxPacket* newPkt = nullptr;
  AVPacket* pkt = av_packet_alloc();
  if (!pkt)
  {
    CLog::Log(LOGERROR, "CDemuxMVC::Read - av_packet_alloc failed: {}", strerror(errno));
    return newPkt;
  }

  while (true)
  {
    int ret = av_read_frame(m_pFormatContext, pkt);

    if (ret == AVERROR(EINTR) || ret == AVERROR(EAGAIN))
      continue;

    if (ret == AVERROR_EOF)
      break;

    if (pkt->size <= 0 || pkt->stream_index != m_nStreamIndex)
    {
      av_packet_unref(pkt);
      continue;
    }

    AVStream* stream = m_pFormatContext->streams[pkt->stream_index];
    newPkt = CDVDDemuxUtils::AllocateDemuxPacket(pkt->size);
    if (pkt->data)
      memcpy(newPkt->pData, pkt->data, pkt->size);
    newPkt->iSize = pkt->size;
    newPkt->iStreamId = stream->id;
    newPkt->dts = ConvertTimestamp(pkt->dts, stream->time_base.den, stream->time_base.num);
    newPkt->pts = ConvertTimestamp(pkt->pts, stream->time_base.den, stream->time_base.num);
    newPkt->duration =
        DVD_SEC_TO_TIME(static_cast<double>(pkt->duration) * stream->time_base.num / stream->time_base.den);
    break;
  }

  av_packet_free(&pkt);
  return newPkt;
}

bool CDemuxMVC::SeekTime(double time, bool backwards, double* startpts)
{
  // time is on the base view's normalized timeline; undo ConvertTimestamp()'s offset.
  return SeekPts(static_cast<int64_t>((time / 1000.0 + m_timestampOffset) * 90000.0));
}

bool CDemuxMVC::SeekPts(int64_t pts)
{
  if (!m_pInput || !m_pFormatContext || m_nStreamIndex < 0)
    return false;

  const AVRational time_base = m_pFormatContext->streams[m_nStreamIndex]->time_base;
  const AVRational clock{1, 90000};
  int64_t seek_pts = av_rescale_q(pts, clock, time_base);
  const int64_t window = av_rescale_q(MVC_SEEK_TIME_WINDOW, clock, time_base);
  seek_pts = seek_pts > window ? seek_pts - window : 0;

  // Always land before the target: the SSIF drops a dependent view running ahead, but
  // drops the base view when the dependent view lags.
  const int ret = av_seek_frame(m_pFormatContext, m_nStreamIndex, seek_pts, AVSEEK_FLAG_BACKWARD);
  if (ret < 0)
    CLog::Log(LOGWARNING, "CDemuxMVC::SeekPts - seek to pts {} failed ({})", pts, ret);
  return ret >= 0;
}

std::string CDemuxMVC::GetFileName()
{
  return m_pInput ? m_pInput->GetFileName() : "";
}

AVStream* CDemuxMVC::GetAVStream()
{
  return m_pFormatContext && m_nStreamIndex >= 0 ? m_pFormatContext->streams[m_nStreamIndex]
                                                  : nullptr;
}

void CDemuxMVC::Dispose()
{
  if (m_pFormatContext)
    avformat_close_input(&m_pFormatContext);

  if (m_ioContext)
  {
    av_free(m_ioContext->buffer);
    av_free(m_ioContext);
  }

  m_ioContext = nullptr;
  m_pFormatContext = nullptr;
  m_pInput = nullptr;
  m_nStreamIndex = -1;
}

double CDemuxMVC::ConvertTimestamp(int64_t pts, int den, int num)
{
  if (pts == static_cast<int64_t>(AV_NOPTS_VALUE))
    return DVD_NOPTS_VALUE;

  // Float math, same rationale as CDVDDemuxFFmpeg::ConvertTimestamp(): an exact timestamp
  // isn't needed and integer math overflows far sooner here.
  double timestamp = static_cast<double>(pts) * num / den;

  // m_timestampOffset is kept continuously up to date with the base view's own resolved
  // start time by CDVDDemuxFFmpeg::ReadInternal() (via
  // CDVDInputStreamBluray::SetExtentionTimestampOffset()) - see SetTimestampOffset()'s own
  // comment for why this must not be a one-shot value transferred only at clip-open time.
  double starttime = m_timestampOffset;

  if (timestamp > starttime)
    timestamp -= starttime;
  else if (timestamp + 0.5 > starttime) // largest possible pts/dts difference for one packet
    timestamp = 0;

  return timestamp * DVD_TIME_BASE;
}

std::vector<CDemuxStream*> CDemuxMVC::GetStreams() const
{
  return {};
}
