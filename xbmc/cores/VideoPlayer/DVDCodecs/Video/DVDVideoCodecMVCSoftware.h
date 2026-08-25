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
 *   - PTS reordering assumes AddData() is fed one demux packet == one AU
 *     (each feed contributes exactly one pending pts to reorder against)
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
#include <set>
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
  unsigned GetAllowedReferences() override { return 16; }

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
  // Single pool keyed by the *combined* samples+mbs size. edge264's
  // internal default allocator (used whenever no alloc_cb is supplied,
  // i.e. exactly what a plain vanilla caller like edge264_test gets)
  // allocates samples and mbs as ONE contiguous block and derives mbs as
  // samples+samples_size (see internal_alloc() in edge264.c) - and its
  // SIMD-based inter-prediction code (edge264_inter.c, e.g.
  // decode_inter_chroma's _mm_loadu_si128/NEON equivalent) reads a short
  // distance past the nominal end of the samples region, relying on that
  // spilling harmlessly into the adjacent, same-allocation mbs region.
  // Handing back samples/mbs as two independent allocations (our
  // original approach here) breaks that: the exact same read becomes a
  // genuine heap-buffer-overflow into unrelated memory the moment it
  // walks off the end of the standalone samples block, rather than
  // landing safely inside the (still allocated, just logically separate
  // to us) mbs block. Confirmed by reproducing the crash standalone
  // under ASan against the actual failing stream: identical two-pool
  // scheme => heap-buffer-overflow in decode_inter_chroma every time;
  // switching to one contiguous samples+mbs allocation => clean decode,
  // zero ASan reports, same stream, same alloc/borrow pattern otherwise.
  // Pool by the *sum* so a same-format stream keeps reusing one
  // contiguous block per slot, exactly as it did per-plane before.
  struct PooledBlock
  {
    void* ptr;
    unsigned size;
  };
  void* AllocFromPool(std::vector<PooledBlock>& pool, unsigned size);
  void ReleaseToPool(std::vector<PooledBlock>& pool, void* ptr);
  static void DrainPool(std::vector<PooledBlock>& pool);

  // Defensive cap so a misbehaving/variable-resolution stream can't grow
  // this pool without bound; edge264's actual DPB is far smaller than
  // this in practice (typically <= 16 frames * 2 views).
  static constexpr size_t MAX_POOLED_BLOCKS = 40;

  std::vector<PooledBlock> m_framePool;
  // Records the allocation size for every live pointer handed out by
  // AllocFromPool(), since edge264's FreeCb only gives us the pointer
  // back, not the size - and the pool needs the size to bucket the block
  // for reuse.
  std::unordered_map<void*, unsigned> m_blockSizes;
  // AllocCb/FreeCb are called by edge264 from its own worker threads
  // (n_threads=4 in Open()), not just from the thread that calls
  // AddData() - m_framePool/m_blockSizes are ordinary STL containers
  // with no internal locking, so concurrent mutation from multiple
  // worker threads (e.g. two threads both reallocating the same
  // vector's backing store at once) is a real, unguarded data race.
  // Every access to those two members must hold this mutex.
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
  // edge264 decodes in bitstream/decode order but - per its own
  // documented guarantee - always emits frames via edge264_get_frame()
  // in strict, monotonic DISPLAY order. m_ptsPending only ever holds the
  // pts of whichever demux packet was *most recently fed*, i.e. decode
  // order; whenever a B-frame (or any reordering) is in play, that is
  // not the pts of the frame actually being emitted right now. A real
  // pts (unlike decode order) is fundamentally a display-time value, so
  // sorting the pending, not-yet-consumed pts values and handing out the
  // smallest one on every successful frame is correct independent of
  // GOP structure, reorder depth, or B-frame count - no bitstream POC
  // math required. See AddData()/GetPicture().
  std::multiset<double> m_ptsQueue;

  CDVDStreamInfo m_hints;
};
