/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "FrameSequentialPresenter.h"
#include "WinSystemAndroid.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/EGLUtils.h"
#include "utils/GlobalsHandling.h"

#include <cstdint>
#include <deque>
#include <vector>

struct AVMasteringDisplayMetadata;
struct AVContentLightMetadata;

class CWinSystemAndroidGLESContext : public CWinSystemAndroid, public CRenderSystemGLES
{
public:
  CWinSystemAndroidGLESContext() = default;
  ~CWinSystemAndroidGLESContext() override = default;

  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAndroid
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock* clock) override;

  float GetFrameLatencyAdjustment() override;
  bool GetVsyncTimeline(int64_t& count, int64_t& timeNanos, int64_t& periodNanos) override;
  bool IsHDRDisplay() override;
  bool SetHDR(const VideoPicture* videoPicture) override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;

private:
  bool CreateSurface();
  bool PresentFrameSequential(CGraphicContext& gfxContext);

  enum class FrameSequentialPhase
  {
    CALIBRATE,
    SETTLE,
    BASELINE,
    RUN
  };

  struct PendingFrame
  {
    uint64_t id;
    int64_t targetTime;
    bool rightEye;
  };

  void TrackFrameSequentialFrames(uint64_t latestId);
  void AddFrameSequentialSample(double offset);
  void CountFrameSequentialFrame(const PendingFrame& frame, int64_t presentTime, int64_t period);
  void ResetFrameSequentialTracking();
  void SetRenderThreadPriority(bool boost);

  bool m_frameSequentialVSync{false};
  std::deque<PendingFrame> m_pendingFrames;
  FrameSequentialPhase m_phaseState{FrameSequentialPhase::CALIBRATE};
  int m_phaseSamples{0};
  double m_phaseSin{0.0};
  double m_phaseCos{0.0};
  std::vector<double> m_baselineSamples;
  double m_baseline{0.0};
  int m_framesChecked{0};
  int m_framesMisplaced{0};
  bool m_timestampsWarned{false};
  bool m_renderThreadBoosted{false};
  int m_renderThreadNice{0};
  double m_submitLagSumMs{0.0};
  double m_submitLagMaxMs{0.0};
  int m_submitLagFrames{0};
  int64_t m_lastPresentTime{0};
  double m_eyeOffsetSum[2]{0.0, 0.0};
  int m_eyeOffsetCount[2]{0, 0};
  int m_spacingCount[4]{0, 0, 0, 0};

  CEGLContextUtils m_pGLContext;
  bool m_hasHDRConfig = false;
  bool m_presentationTimeWarned{false};
  std::unique_ptr<CFrameSequentialPresenter> m_frameSequentialPresenter;
  bool m_frameSequentialPresenterFailed{false};

  std::unique_ptr<AVMasteringDisplayMetadata> m_displayMetadata;
  std::unique_ptr<AVContentLightMetadata> m_lightMetadata;
  EGLint m_HDRColorSpace = EGL_NONE;
  bool m_hasEGL_ST2086_Extension = false;
  bool m_hasEGL_BT2020_PQ_Colorspace_Extension = false;
};
