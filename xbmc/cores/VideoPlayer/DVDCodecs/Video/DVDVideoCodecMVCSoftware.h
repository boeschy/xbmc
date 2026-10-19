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
 *     (the packet's pts is attached to the picture its NALs start)
 *   - no secure/DRM path
 *   - thread count and buffering are unturned
 */

#pragma once

#include "DVDStreamInfo.h"
#include "DVDVideoCodec.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/BitstreamConverter.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "edge264mvc.h"

class CDVDVideoCodecMVCSoftware : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecMVCSoftware(CProcessInfo& processInfo);
  ~CDVDVideoCodecMVCSoftware() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static bool Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool Reconfigure(CDVDStreamInfo& hints) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  void SetCodecControl(int flags) override { m_codecControlFlags = flags; }
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

  bool PackFrame(const Edge264MvcFrame& frame, VideoPicture* pVideoPicture);
  // Moves every frame edge264-mvc has ready into m_pictures; returns how many
  int DrainFrames();
  // Sends one NAL, receiving frames while the decoder is full; returns the
  // EDGE264MVC_* result
  int DecodeNal(const uint8_t* nal, size_t size, int64_t pts);

  Edge264MvcDecoder* m_decoder = nullptr;
  CBitstreamConverter m_bitstream;
  bool m_annexB = false; // input is Annex-B already, m_bitstream stays closed
  PackMode m_packMode = PackMode::SBS;
  bool m_baseViewIsRightEye = false;
  int m_codecControlFlags = 0;

  unsigned int m_width = 0;
  unsigned int m_height = 0;

  // Packed pictures waiting for GetPicture(), in display order
  std::deque<std::unique_ptr<VideoPicture>> m_pictures;

  CDVDStreamInfo m_hints;
};
