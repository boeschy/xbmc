/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - see DemuxStreamSSIF.h for scope and known
 *  limitations.
 */

#include "DemuxStreamSSIF.h"

#include "DVDDemuxUtils.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemux.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <cmath>
#include <cstring>

//#define DEBUG_VERBOSE

namespace
{
// Arbitrary, unturned - deep enough to ride out a normal burst of dependent-view packets
// arriving ahead of their base-view partners without starving the base view while
// FillMVCQueue() catches up.
constexpr size_t MVC_QUEUE_SIZE = 100;

// Never let an unmatched base-view packet wait forever for a dependent-view partner that
// isn't coming (end of stream, or a disc where the two views' timestamps don't land on the
// same tick despite the continuous offset sync - see the .h known limitations).
constexpr size_t H264_QUEUE_LIMIT = 32;

// Well below one 90 kHz tick; only absorbs double rounding, never a real offset.
constexpr double MVC_PAIR_TOLERANCE = 1.0;
} // namespace

DemuxPacket* CDemuxStreamSSIF::AddPacket(DemuxPacket*& srcPkt)
{
  if (srcPkt->iStreamId != m_h264StreamId && srcPkt->iStreamId != m_mvcStreamId)
    return srcPkt;

  if (srcPkt->iStreamId == m_h264StreamId)
  {
    // Not a BD-3D title (or the dependent-view clip failed to open) - pass the base view
    // through untouched rather than stall behind an MVC queue that will never fill.
    if (m_bluRay && !m_bluRay->HasExtention())
      return srcPkt;
#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> MVC add h264 packet: pts: {:.3f} dts: {:.3f}", srcPkt->pts * 1e-6,
              srcPkt->dts * 1e-6);
#endif
    m_H264queue.push(srcPkt);
  }
  else if (srcPkt->iStreamId == m_mvcStreamId)
  {
    m_MVCqueue.push_back(srcPkt);
#if defined(DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, ">>> MVC add mvc packet: pts: {:.3f} dts: {:.3f}", srcPkt->pts * 1e-6,
              srcPkt->dts * 1e-6);
#endif
  }

  return GetMVCPacket();
}

void CDemuxStreamSSIF::Flush()
{
  while (!m_H264queue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_H264queue.front());
    m_H264queue.pop();
  }
  while (!m_MVCqueue.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_MVCqueue.front());
    m_MVCqueue.pop_front();
  }
}

DemuxPacket* CDemuxStreamSSIF::MergePacket(DemuxPacket*& srcPkt, DemuxPacket*& appendPkt)
{
  DemuxPacket* newpkt = CDVDDemuxUtils::AllocateDemuxPacket(srcPkt->iSize + appendPkt->iSize);
  newpkt->iSize = srcPkt->iSize + appendPkt->iSize;

  newpkt->pts = srcPkt->pts;
  newpkt->dts = srcPkt->dts;
  newpkt->duration = srcPkt->duration;
  newpkt->iGroupId = srcPkt->iGroupId;
  newpkt->iStreamId = srcPkt->iStreamId;
  memcpy(newpkt->pData, srcPkt->pData, srcPkt->iSize);
  memcpy(newpkt->pData + srcPkt->iSize, appendPkt->pData, appendPkt->iSize);

  CDVDDemuxUtils::FreeDemuxPacket(srcPkt);
  srcPkt = nullptr;
  CDVDDemuxUtils::FreeDemuxPacket(appendPkt);
  appendPkt = nullptr;

  return newpkt;
}

DemuxPacket* CDemuxStreamSSIF::GetMVCPacket()
{
  // On a bluray, top up the dependent-view queue from the second demuxer before trying to
  // pair anything - CDemuxMVC's Read() is pull-based, nothing arrives on its own.
  if (m_bluRay && m_MVCqueue.empty() && !m_H264queue.empty())
    FillMVCQueue(m_H264queue.front()->dts);

  while (!m_H264queue.empty() && !m_MVCqueue.empty())
  {
    DemuxPacket* h264pkt = m_H264queue.front();
    double tsH264 = (h264pkt->dts != DVD_NOPTS_VALUE ? h264pkt->dts : h264pkt->pts);
    DemuxPacket* mvcpkt = m_MVCqueue.front();
    double tsMVC = (mvcpkt->dts != DVD_NOPTS_VALUE ? mvcpkt->dts : mvcpkt->pts);

    if (std::fabs(tsH264 - tsMVC) <= MVC_PAIR_TOLERANCE)
    {
      m_H264queue.pop();
      m_MVCqueue.pop_front();

      // Absorb any further base-view fragments carrying no timestamp of their own (a
      // packet split across a PES boundary) into the same access unit before merging.
      while (!m_H264queue.empty())
      {
        DemuxPacket* pkt = m_H264queue.front();
        double ts = (pkt->dts != DVD_NOPTS_VALUE ? pkt->dts : pkt->pts);
        if (ts != DVD_NOPTS_VALUE)
          break;
        h264pkt = MergePacket(h264pkt, pkt);
        m_H264queue.pop();
      }
#if defined(DEBUG_VERBOSE)
      CLog::Log(LOGDEBUG, ">>> MVC merge packet: {:6}+{:6}, pts({:.3f}/{:.3f}) dts({:.3f}/{:.3f})",
                h264pkt->iSize, mvcpkt->iSize, h264pkt->pts * 1e-6, mvcpkt->pts * 1e-6,
                h264pkt->dts * 1e-6, mvcpkt->dts * 1e-6);
#endif
      return MergePacket(h264pkt, mvcpkt);
    }

    if (tsH264 > tsMVC)
    {
      // Dependent-view leftover with no base-view partner left to pair with (e.g. after a
      // seek discarded the base-view side already) - drop it and keep looking.
      CDVDDemuxUtils::FreeDemuxPacket(mvcpkt);
      m_MVCqueue.pop_front();
    }
    else
    {
      // Base-view frame with no dependent-view partner (yet) - drop it rather than feed
      // the codec a lone base-view AU, which edge264_get_frame() would never surface as a
      // picture anyway (CDVDVideoCodecMVCSoftware::AddData() requires samples_mvc[0]).
      CDVDDemuxUtils::FreeDemuxPacket(h264pkt);
      m_H264queue.pop();
    }
  }

  if (m_H264queue.size() > H264_QUEUE_LIMIT)
  {
    CLog::Log(LOGWARNING, "CDemuxStreamSSIF::GetMVCPacket - dependent view not keeping up, "
                          "dropping oldest base-view packet unpaired");
    DemuxPacket* h264pkt = m_H264queue.front();
    m_H264queue.pop();
    return h264pkt;
  }

  return CDVDDemuxUtils::AllocateDemuxPacket(0);
}

bool CDemuxStreamSSIF::FillMVCQueue(double dtsBase)
{
  if (!m_bluRay)
    return false;

  CDVDDemux* demux = m_bluRay->GetExtentionDemux();
  if (!demux)
    return false;

  DemuxPacket* mvc;
  while (m_MVCqueue.size() < MVC_QUEUE_SIZE && (mvc = demux->Read()) != nullptr)
  {
    if (mvc->iSize == 0)
    {
      CDVDDemuxUtils::FreeDemuxPacket(mvc);
      continue; // "waiting"/empty packet from the extension demux, nothing to buffer yet
    }
    if (dtsBase != DVD_NOPTS_VALUE && mvc->dts != DVD_NOPTS_VALUE && mvc->dts < dtsBase)
    {
      CDVDDemuxUtils::FreeDemuxPacket(mvc);
      continue;
    }
    m_MVCqueue.push_back(mvc);
  }

  if (m_MVCqueue.size() != MVC_QUEUE_SIZE)
  {
    // Ran out of data in the current dependent-view clip - move on to the next one queued
    // by CDVDInputStreamBluray. A false return means there is no next clip.
    m_bluRay->OpenNextStream();
  }

  return true;
}
