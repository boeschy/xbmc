/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  PROOF OF CONCEPT - NOT PRODUCTION CODE.
 *
 *  Software H.264/MVC (Stereo High Profile) decoder for platforms whose
 *  hardware decoder has no MVC support and whose platform media framework
 *  exposes no MVC mime type to reach it even if it did (this is the
 *  situation on Android/Tegra: NVDEC only advertises Baseline/Main/High
 *  AVC profiles, and MediaCodec has no "video/mvc" type at all).
 *
 *  Both views are decoded entirely in software via the edge264-mvc
 *  decoder (https://github.com/jens-duttke/edge264-mvc) and packed into a
 *  single half-resolution side-by-side (or top-bottom) YUV420P frame, so
 *  Kodi's existing stereoscopic render path can display it without any
 *  HDMI frame-packing support on the output side.
 *
 *  Known PoC limitations (see accompanying .cpp for details):
 *   - no interlace/field support
 *   - PTS handling assumes AddData() is fed one demux packet == one AU
 *   - no secure/DRM path
 *   - thread count and buffering are unturned
 */

#pragma once

#include "DVDStreamInfo.h"
#include "DVDVideoCodec.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/BitstreamConverter.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C"
{
#include "edge264.h"
}

class CDVDVideoCodecMVCSoftware : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecMVCSoftware(CProcessInfo& processInfo);
  ~CDVDVideoCodecMVCSoftware() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static bool Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return "mvc-sw-edge264"; }
  unsigned GetAllowedReferences() override { return 4; }

protected:
  enum class PackMode
  {
    SBS,
    TAB
  };

  // Returns true if the stream is H.264 Stereo High Profile (MVC), i.e.
  // the only case this codec should ever claim in CDVDFactoryCodec.
  // Relies primarily on hints.profile as reported by the demuxer; falls
  // back to a minimal extradata scan for a subset-SPS (NAL type 15),
  // which is present whenever a second (MVC) view is muxed alongside the
  // base view, independent of whether the demuxer populated `profile`.
  static bool DetectStereoHighProfile(const CDVDStreamInfo& hints);

  bool PackFrame(const Edge264Frame& frame, VideoPicture* pVideoPicture);

  // Simple size-bucketed free-list pool backing AllocCb/FreeCb: edge264
  // requests/releases DPB slots continuously during decode, and for a
  // given stream (fixed resolution) these come in a small, stable set of
  // sizes, so a malloc/free per frame is pure, avoidable churn. Actual
  // frees only happen in the destructor via DrainPool().
  struct PooledBlock
  {
    void* ptr;
    unsigned size;
  };
  void* AllocFromPool(std::vector<PooledBlock>& pool, unsigned size);
  void ReleaseToPool(std::vector<PooledBlock>& pool, void* ptr);
  static void DrainPool(std::vector<PooledBlock>& pool);

  // Defensive cap so a misbehaving/variable-resolution stream can't grow
  // these pools without bound; edge264's actual DPB is far smaller than
  // this in practice (typically <= 16 frames * 2 views).
  static constexpr size_t MAX_POOLED_BLOCKS = 40;

  std::vector<PooledBlock> m_samplesPool;
  std::vector<PooledBlock> m_mbsPool;
  // Records the allocation size for every live pointer handed out by
  // AllocFromPool(), since edge264's FreeCb only gives us the pointer
  // back, not the size - and the pool needs the size to bucket the block
  // for reuse.
  std::unordered_map<void*, unsigned> m_blockSizes;
  // AllocCb/FreeCb are called by edge264 from its own worker threads
  // (n_threads=4 in Open()), not just from the thread that calls
  // AddData() - m_samplesPool/m_mbsPool/m_blockSizes are ordinary STL
  // containers with no internal locking, so concurrent mutation from
  // multiple worker threads (e.g. two threads both reallocating the same
  // vector's backing store at once) is a real, unguarded data race.
  // Every access to those three members must hold this mutex.
  std::mutex m_poolMutex;

  static void AllocCb(void** samples,
                       unsigned samples_size,
                       void** mbs,
                       unsigned mbs_size,
                       int errno_on_fail,
                       void* alloc_arg);
  static void FreeCb(void* samples, void* mbs, void* alloc_arg);
  static int LogCb(const char* str, void* log_arg);

  Edge264Decoder* m_decoder = nullptr;
  CBitstreamConverter m_bitstream;
  PackMode m_packMode = PackMode::SBS;

  unsigned int m_width = 0;
  unsigned int m_height = 0;

  bool m_hasPicture = false;
  VideoPicture m_picture; // holds the ref-counted buffer built by PackFrame() until GetPicture() collects it
  double m_pts = DVD_NOPTS_VALUE;
  double m_ptsPending = DVD_NOPTS_VALUE;

  CDVDStreamInfo m_hints;
};
