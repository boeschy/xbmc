/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - NOT PRODUCTION CODE. Part of the BD-3D SSIF/MVC
 *  playback path; see DemuxStreamSSIF.h for the overall design.
 *
 *  On a BD-3D disc, the H.264/MVC dependent (right-eye) view is authored
 *  as its own clip with its own .m2ts under BDMV/STREAM - referenced from
 *  the playlist's stereoscopic sub-path (MPLS_PL::ext_sub_path, type 8),
 *  not from the main play item list. It is a completely independent
 *  transport stream multiplex with its own PMT, in which the dependent
 *  view is carried as stream_type 0x20 ("MVC video sub-bitstream"),
 *  mapped by this build's FFmpeg patches (0001-added_upstream_mvc_patches.patch)
 *  to AV_CODEC_ID_H264_MVC.
 *
 *  CDemuxMVC opens that file as its own, independent AVFormatContext,
 *  entirely separate from the main CDVDDemuxFFmpeg reading the base-view
 *  clip - which means it resolves its own, independent zero point for
 *  container timestamps. See ConvertTimestamp()/SetTimestampOffset() for
 *  why that matters and how it's corrected for.
 */

#pragma once

#include "DVDDemux.h"
#include "DVDInputStreams/DVDInputStream.h"

extern "C"
{
#include "libavformat/avformat.h"
}

class CDemuxMVC : public CDVDDemux
{
public:
  CDemuxMVC();
  ~CDemuxMVC() override;

  bool Open(CDVDInputStream* pInput);
  bool Reset() override;
  void Abort() override;
  void Flush() override;
  DemuxPacket* Read() override;
  bool SeekTime(double time, bool backwards = false, double* startpts = nullptr) override;
  //! pts in the clip's own 90 kHz timeline (shared by both views' m2ts), unlike SeekTime().
  bool SeekPts(int64_t pts);
  void SetSpeed(int iSpeed) override {}
  int GetStreamLength() override { return 0; }
  CDemuxStream* GetStream(int iStreamId) const override { return nullptr; }
  std::vector<CDemuxStream*> GetStreams() const override;
  int GetNrOfStreams() const override { return 1; }
  std::string GetFileName() override;

  /*!
   * \brief Sets the point in absolute (container) time that the *base view's* own
   *        demuxer is currently treating as zero (CDVDDemuxFFmpeg::m_startTime,
   *        resolved from the base view's own first packet - see
   *        CDVDDemuxFFmpeg::IsTransportStreamReady()), so ConvertTimestamp() can put
   *        this clip's packets on the same normalized timeline.
   *
   *        Deliberately not a one-time value passed at Open()/OpenNextStream() time
   *        (an earlier version of this class worked that way, transferring the
   *        previous clip's own resolved start time forward on each clip change): the
   *        base view's m_startTime isn't resolved until its own first packet is read,
   *        which can easily be *after* this clip's dependent-view packets have already
   *        started arriving and being timestamped - leaving the very first clip (the
   *        common case) permanently uncorrected, off by exactly m_startTime, and every
   *        pts/dts pairing in CDemuxStreamSSIF silently failing for the entire clip.
   *        Called on every packet instead (see CDVDDemuxFFmpeg::ReadInternal()) - cheap,
   *        and always correct regardless of when the base view resolves its own offset.
   */
  void SetTimestampOffset(double offsetSeconds) { m_timestampOffset = offsetSeconds; }

  AVStream* GetAVStream();

  CDVDInputStream* m_pInput = nullptr;

private:
  void Dispose();
  double ConvertTimestamp(int64_t pts, int den, int num);

  AVIOContext* m_ioContext = nullptr;
  AVFormatContext* m_pFormatContext = nullptr;
  int m_nStreamIndex = -1;
  double m_timestampOffset = 0.0;
};
