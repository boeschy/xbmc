/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - NOT PRODUCTION CODE.
 *
 *  This is the missing link between the BD-3D disc layout and
 *  CDVDVideoCodecMVCSoftware: the base view and the H.264/MVC dependent
 *  view live in two entirely separate elementary streams (see
 *  DemuxMVC.h for why), but the software MVC decoder's AddData() expects
 *  a single packet per access unit containing both views' NAL units back
 *  to back - exactly like FFmpeg's own H.264 MVC parser keeps them
 *  together for the Matroska mvcC path this codec was originally built
 *  against (see CDVDVideoCodecMVCSoftware::DetectStereoHighProfile()).
 *
 *  CDemuxStreamSSIF is that merge point. CDVDDemuxFFmpeg::ReadInternal()
 *  feeds every packet on the base-view stream id through AddPacket();
 *  this class pulls dependent-view packets from the second demuxer
 *  (CDemuxMVC, via CDVDInputStream::IExtentionStream) on demand, pairs
 *  them with the base-view packet carrying the same timestamp, and
 *  returns one concatenated packet per access unit. Every other stream
 *  id passes through untouched.
 *
 *  Known PoC limitations:
 *   - pairing is by exact dts/pts equality; a disc whose two views drift
 *     by even one clock tick will silently stop merging AUs (falling
 *     back to dropping whichever side is "ahead" - see GetMVCPacket())
 *     rather than pairing them within a tolerance. This is why
 *     CDemuxMVC's timestamp offset is kept continuously synced to the
 *     base view's own (see DemuxMVC.h) - the two sides need to agree on
 *     their zero point exactly, not just approximately, for this to work
 *     at all.
 *   - MVC_QUEUE_SIZE/H264_QUEUE_LIMIT (see .cpp) are arbitrary depths,
 *     unturned
 *   - no attempt is made to handle a disc angle change or PiP secondary
 *     video sharing the dependent-view sub-path
 */

#pragma once

#include "DVDInputStreams/DVDInputStream.h"

#include <deque>
#include <memory>
#include <queue>

extern "C"
{
#include "libavformat/avformat.h"
}

// Marker written to the base-view CDemuxStream's codec_fourcc (which flows straight into
// CDVDStreamInfo::codec_tag - see DVDStreamInfo.cpp's CDVDStreamInfo::Assign()) once a BD-3D
// dependent view has been found for it. See DVDDemuxFFmpeg.cpp's stream setup for why this -
// and not codec_id - is what gets tagged, and DVDVideoCodecMVCSoftware.cpp's
// DetectStereoHighProfile() for the matching check.
constexpr unsigned int BD3D_MVC_CODEC_TAG = MKTAG('K', 'M', 'V', 'C');

class CDemuxStreamSSIF
{
public:
  CDemuxStreamSSIF() = default;
  ~CDemuxStreamSSIF() { Flush(); }

  DemuxPacket* AddPacket(DemuxPacket*& srcPkt);
  void Flush();

  void SetH264StreamId(int id) { m_h264StreamId = id; }
  void SetMVCStreamId(int id) { m_mvcStreamId = id; }
  int GetH264StreamId() const { return m_h264StreamId; }
  int GetMVCStreamId() const { return m_mvcStreamId; }

  void SetBluRay(const std::shared_ptr<CDVDInputStream::IExtentionStream>& bluRay)
  {
    m_bluRay = bluRay;
  }
  bool IsBluRay() const { return m_bluRay != nullptr; }

private:
  DemuxPacket* GetMVCPacket();
  DemuxPacket* MergePacket(DemuxPacket*& srcPkt, DemuxPacket*& appendPkt);
  bool FillMVCQueue(double dtsBase);

  std::shared_ptr<CDVDInputStream::IExtentionStream> m_bluRay;
  std::queue<DemuxPacket*> m_H264queue;
  // deque so AddMVCExtPacket() can peek/replace the back element when reassembling a
  // dependent-view access unit split across a PES fragment boundary (rare, but the
  // dependent-view clip's own PES packetization isn't guaranteed to align 1:1 with AUs).
  std::deque<DemuxPacket*> m_MVCqueue;
  int m_h264StreamId = -1;
  int m_mvcStreamId = -1;
};
