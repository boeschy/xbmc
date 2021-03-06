/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mastering_display_metadata.h>
}

namespace DRMPRIME
{

// colorimetry definitions are copied from linux include/drm/drm_connector.h (not part of uapi yet)
/* CEA 861 Extended Colorimetry Options */
constexpr int DRM_MODE_COLORIMETRY_XVYCC_601{3};
constexpr int DRM_MODE_COLORIMETRY_XVYCC_709{4};
constexpr int DRM_MODE_COLORIMETRY_SYCC_601{5};
constexpr int DRM_MODE_COLORIMETRY_OPYCC_601{6};
constexpr int DRM_MODE_COLORIMETRY_OPRGB{7};
constexpr int DRM_MODE_COLORIMETRY_BT2020_CYCC{8};
constexpr int DRM_MODE_COLORIMETRY_BT2020_RGB{9};
constexpr int DRM_MODE_COLORIMETRY_BT2020_YCC{10};

// HDR enums is copied from linux include/linux/hdmi.h (strangely not part of uapi)
enum hdmi_metadata_type
{
  HDMI_STATIC_METADATA_TYPE1 = 0,
};
enum hdmi_eotf
{
  HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
  HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
  HDMI_EOTF_SMPTE_ST2084,
  HDMI_EOTF_BT_2100_HLG,
};

int GetColorimetry(const VideoPicture& picture);
std::string GetColorEncoding(const VideoPicture& picture);
std::string GetColorRange(const VideoPicture& picture);
uint8_t GetEOTF(const VideoPicture& picture);
const AVMasteringDisplayMetadata* GetMasteringDisplayMetadata(const VideoPicture& picture);
const AVContentLightMetadata* GetContentLightMetadata(const VideoPicture& picture);

} // namespace DRMPRIME

class CVideoBufferDRMPRIME : public CVideoBuffer
{
public:
  CVideoBufferDRMPRIME() = delete;
  ~CVideoBufferDRMPRIME() override = default;

  virtual void SetPictureParams(const VideoPicture& picture) { m_picture.SetParams(picture); }
  virtual const VideoPicture& GetPicture() const { return m_picture; }
  virtual uint32_t GetWidth() const { return GetPicture().iWidth; }
  virtual uint32_t GetHeight() const { return GetPicture().iHeight; }

  virtual AVDRMFrameDescriptor* GetDescriptor() const = 0;
  virtual bool IsValid() const { return true; }
  virtual bool AcquireDescriptor() { return true; }
  virtual void ReleaseDescriptor() {}

  uint32_t m_fb_id = 0;
  uint32_t m_handles[AV_DRM_MAX_PLANES] = {};

protected:
  explicit CVideoBufferDRMPRIME(int id);

  VideoPicture m_picture;
};

class CVideoBufferDRMPRIMEFFmpeg : public CVideoBufferDRMPRIME
{
public:
  CVideoBufferDRMPRIMEFFmpeg(IVideoBufferPool& pool, int id);
  ~CVideoBufferDRMPRIMEFFmpeg() override;
  void SetRef(AVFrame* frame);
  void Unref();

  AVDRMFrameDescriptor* GetDescriptor() const override;
  bool IsValid() const override;
  bool AcquireDescriptor() override;
  void ReleaseDescriptor() override;

protected:
  AVFrame* m_pFrame = nullptr;
  AVFrame* m_pMapFrame = nullptr;
};

class CVideoBufferPoolDRMPRIMEFFmpeg : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolDRMPRIMEFFmpeg() override;
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferDRMPRIMEFFmpeg*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
};
