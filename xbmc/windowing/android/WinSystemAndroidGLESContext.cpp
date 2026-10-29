/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAndroidGLESContext.h"

#include "ServiceBroker.h"
#include "VideoSyncAndroid.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "rendering/RenderSystemTypes.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

#include "platform/android/activity/XBMCApp.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <memory>

#include <sys/resource.h>
#include <sys/syscall.h>

#include <unistd.h>

#include "PlatformDefs.h"

#include <EGL/eglext.h>

void CWinSystemAndroidGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem);
}

std::unique_ptr<CWinSystemBase> CWinSystemAndroidGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemAndroidGLESContext>();
}

bool CWinSystemAndroidGLESContext::InitWindowSystem()
{
  if (!CWinSystemAndroid::InitWindowSystem())
  {
    return false;
  }

  if (!m_pGLContext.CreateDisplay(m_nativeDisplay))
  {
    return false;
  }

  if (!m_pGLContext.InitializeDisplay(EGL_OPENGL_ES_API))
  {
    return false;
  }

  if (!m_pGLContext.ChooseConfig(EGL_OPENGL_ES2_BIT))
  {
    return false;
  }

  m_hasHDRConfig = m_pGLContext.ChooseConfig(EGL_OPENGL_ES2_BIT, 0, true);

  m_hasEGL_BT2020_PQ_Colorspace_Extension =
      CEGLUtils::HasExtension(m_pGLContext.GetEGLDisplay(), "EGL_EXT_gl_colorspace_bt2020_pq");
  m_hasEGL_ST2086_Extension =
      CEGLUtils::HasExtension(m_pGLContext.GetEGLDisplay(), "EGL_EXT_surface_SMPTE2086_metadata");

  bool hasEGLHDRExtensions = m_hasEGL_BT2020_PQ_Colorspace_Extension && m_hasEGL_ST2086_Extension;

  CLog::Log(LOGDEBUG,
            "CWinSystemAndroidGLESContext::InitWindowSystem: HDRConfig: {}, HDRExtensions: {}",
            static_cast<int>(m_hasHDRConfig), static_cast<int>(hasEGLHDRExtensions));

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

  if (!m_pGLContext.CreateContext(contextAttribs))
  {
    return false;
  }

  return true;
}

bool CWinSystemAndroidGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  m_frameSequentialPresenter.reset();
  m_frameSequentialPresenterFailed = false;
  m_pGLContext.DestroySurface();

  if (!CWinSystemAndroid::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  if (!CreateSurface())
  {
    return false;
  }

  if (!m_pGLContext.BindContext())
  {
    return false;
  }

  return true;
}

bool CWinSystemAndroidGLESContext::DestroyWindow()
{
  m_frameSequentialPresenter.reset();
  return CWinSystemAndroid::DestroyWindow();
}

bool CWinSystemAndroidGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAndroidGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  CreateNewWindow("", fullScreen, res);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAndroidGLESContext::SetVSyncImpl(bool enable)
{
  // We use Choreographer for timing
  m_pGLContext.SetVSync(m_frameSequentialVSync);
}

namespace
{
constexpr int64_t FRAME_SEQUENTIAL_MAX_QUEUED_FRAMES{4};
} // namespace

void CWinSystemAndroidGLESContext::PresentRenderImpl(bool rendered)
{
  if (!m_nativeWindow)
  {
    usleep(10000);
    return;
  }

  // Mode change finalization was triggered by timer
  if (IsHdmiModeTriggered())
    SetHdmiState(true);

  // Ignore EGL_BAD_SURFACE: It seems to happen during/after mode changes, but
  // we can't actually do anything about it
  if (rendered)
  {
    CGraphicContext& gfxContext = GetGfxContext();
    const bool frameSequential = gfxContext.GetStereoMode() == RenderStereoMode::FRAME_SEQUENTIAL;
    if (frameSequential != m_frameSequentialVSync)
    {
      m_pGLContext.SetVSync(frameSequential);
      m_frameSequentialVSync = frameSequential;
      ResetFrameSequentialTracking();
      gfxContext.SetFrameSequentialPhase(0.0);
      SetRenderThreadPriority(frameSequential);
      m_frameSequentialPresenterFailed = false;
    }

    if (!frameSequential)
      m_frameSequentialPresenter.reset();
    else if (PresentFrameSequential(gfxContext))
      return;

    uint64_t frameId{0};
    bool trackFrame{false};
    if (frameSequential)
    {
      gfxContext.BindFrameSequentialSlot();
      const int64_t presentTime = gfxContext.GetFrameSequentialPresentTime();
      if (presentTime > 0 && !m_pGLContext.SetPresentationTime(presentTime) &&
          !m_presentationTimeWarned)
      {
        CLog::LogF(LOGWARNING, "EGL_ANDROID_presentation_time is not available, frame sequential "
                               "output can not pin frames to refreshes");
        m_presentationTimeWarned = true;
      }

      trackFrame = presentTime > 0 && m_pGLContext.GetNextFrameId(frameId);
      if (trackFrame)
        m_pendingFrames.push_back({frameId, gfxContext.GetFrameSequentialTargetTime(),
                                   gfxContext.GetFrameSequentialEye() == RenderStereoView::RIGHT});
      else if (presentTime > 0 && !m_timestampsWarned)
      {
        CLog::LogF(LOGWARNING, "EGL_ANDROID_get_frame_timestamps is not available, frame "
                               "sequential output can not be checked against the display");
        m_timestampsWarned = true;
      }
    }

    if (trackFrame)
    {
      int64_t count{0};
      int64_t timeNanos{0};
      int64_t periodNanos{0};
      if (GetVsyncTimeline(count, timeNanos, periodNanos))
      {
        const double lagMs = static_cast<double>(CurrentHostCounter() - timeNanos) * 1.0e-6;
        m_submitLagSumMs += lagMs;
        m_submitLagMaxMs = std::max(m_submitLagMaxMs, lagMs);
        ++m_submitLagFrames;
      }
    }

    if (!m_pGLContext.TrySwapBuffers())
      CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");

    if (trackFrame)
      TrackFrameSequentialFrames(frameId);
  }

  if (rendered && GetGfxContext().GetStereoMode() == RenderStereoMode::FRAME_SEQUENTIAL)
  {
    if (GetGfxContext().GetFrameSequentialQueuedFrames() >= FRAME_SEQUENTIAL_MAX_QUEUED_FRAMES)
      CXBMCApp::Get().WaitVSync(1000);
    return;
  }

  CXBMCApp::Get().WaitVSync(1000);
}

namespace
{
constexpr uint64_t FRAME_SEQUENTIAL_TIMESTAMP_DELAY{4};
constexpr uint64_t FRAME_SEQUENTIAL_TIMESTAMP_GIVEUP{24};
constexpr int FRAME_SEQUENTIAL_CALIBRATION_SAMPLES{16};
constexpr int FRAME_SEQUENTIAL_SETTLE_SAMPLES{24};
constexpr int FRAME_SEQUENTIAL_CHECK_SAMPLES{240};
constexpr double FRAME_SEQUENTIAL_TWO_PI{6.283185307179586};
constexpr int FRAME_SEQUENTIAL_RENDER_NICE{-8};
} // namespace

void CWinSystemAndroidGLESContext::SetRenderThreadPriority(bool boost)
{
  const id_t tid = static_cast<id_t>(syscall(SYS_gettid));
  if (boost)
  {
    errno = 0;
    const int current = getpriority(PRIO_PROCESS, tid);
    if (errno != 0 || setpriority(PRIO_PROCESS, tid, FRAME_SEQUENTIAL_RENDER_NICE) != 0)
    {
      CLog::LogF(LOGWARNING, "Frame sequential: could not raise the render thread priority ({})",
                 errno);
      return;
    }
    m_renderThreadNice = current;
    m_renderThreadBoosted = true;
    CLog::LogF(LOGINFO, "Frame sequential: render thread priority raised to {}",
               FRAME_SEQUENTIAL_RENDER_NICE);
  }
  else if (m_renderThreadBoosted)
  {
    setpriority(PRIO_PROCESS, tid, m_renderThreadNice);
    m_renderThreadBoosted = false;
  }
}

void CWinSystemAndroidGLESContext::ResetFrameSequentialTracking()
{
  m_lastPresentTime = 0;
  m_submitLagSumMs = 0.0;
  m_submitLagMaxMs = 0.0;
  m_submitLagFrames = 0;
  m_pendingFrames.clear();
  m_phaseState = FrameSequentialPhase::CALIBRATE;
  m_phaseSamples = 0;
  m_phaseSin = 0.0;
  m_phaseCos = 0.0;
  m_baselineSamples.clear();
  m_framesChecked = 0;
  m_framesMisplaced = 0;
}

void CWinSystemAndroidGLESContext::TrackFrameSequentialFrames(uint64_t latestId)
{
  int64_t count{0};
  int64_t timeNanos{0};
  int64_t periodNanos{0};
  if (!GetVsyncTimeline(count, timeNanos, periodNanos))
    return;

  while (!m_pendingFrames.empty())
  {
    const PendingFrame frame = m_pendingFrames.front();
    if (frame.id + FRAME_SEQUENTIAL_TIMESTAMP_DELAY > latestId)
      break;

    int64_t presentTime{0};
    const int result = m_pGLContext.GetDisplayPresentTime(frame.id, presentTime);
    if (result == 0 && frame.id + FRAME_SEQUENTIAL_TIMESTAMP_GIVEUP > latestId)
      break;

    m_pendingFrames.pop_front();
    if (result == 1)
    {
      CountFrameSequentialFrame(frame, presentTime, periodNanos);
      AddFrameSequentialSample(static_cast<double>(presentTime - frame.targetTime) /
                               static_cast<double>(periodNanos));
    }
  }
}

void CWinSystemAndroidGLESContext::CountFrameSequentialFrame(const PendingFrame& frame,
                                                             int64_t presentTime,
                                                             int64_t period)
{
  const int eye = frame.rightEye ? 1 : 0;
  m_eyeOffsetSum[eye] +=
      static_cast<double>(presentTime - frame.targetTime) / static_cast<double>(period);
  ++m_eyeOffsetCount[eye];

  if (m_lastPresentTime > 0)
  {
    const int64_t spacing = std::llround(static_cast<double>(presentTime - m_lastPresentTime) /
                                         static_cast<double>(period));
    ++m_spacingCount[std::clamp<int64_t>(spacing, 0, 3)];
  }
  m_lastPresentTime = presentTime;
}

void CWinSystemAndroidGLESContext::AddFrameSequentialSample(double offset)
{
  switch (m_phaseState)
  {
    case FrameSequentialPhase::CALIBRATE:
    {
      m_phaseSin += std::sin(FRAME_SEQUENTIAL_TWO_PI * offset);
      m_phaseCos += std::cos(FRAME_SEQUENTIAL_TWO_PI * offset);
      if (++m_phaseSamples < FRAME_SEQUENTIAL_CALIBRATION_SAMPLES)
        break;

      double phase = std::atan2(m_phaseSin, m_phaseCos) / FRAME_SEQUENTIAL_TWO_PI;
      if (phase < 0.0)
        phase += 1.0;
      GetGfxContext().SetFrameSequentialPhase(phase);
      CLog::LogF(LOGINFO, "Frame sequential: the display shows frames {:.2f} refreshes after the "
                          "Choreographer tick, aiming between two refreshes",
                 phase);
      m_phaseState = FrameSequentialPhase::SETTLE;
      m_phaseSamples = 0;
      break;
    }
    case FrameSequentialPhase::SETTLE:
      if (++m_phaseSamples >= FRAME_SEQUENTIAL_SETTLE_SAMPLES)
      {
        m_phaseState = FrameSequentialPhase::BASELINE;
        m_baselineSamples.clear();
      }
      break;
    case FrameSequentialPhase::BASELINE:
      m_baselineSamples.push_back(offset);
      if (m_baselineSamples.size() >= FRAME_SEQUENTIAL_CALIBRATION_SAMPLES)
      {
        std::ranges::sort(m_baselineSamples);
        m_baseline = m_baselineSamples[m_baselineSamples.size() / 2];
        m_phaseState = FrameSequentialPhase::RUN;
        m_framesChecked = 0;
        m_framesMisplaced = 0;
      }
      break;
    case FrameSequentialPhase::RUN:
      ++m_framesChecked;
      if (std::abs(offset - m_baseline) >= 0.5)
        ++m_framesMisplaced;

      if (m_framesChecked >= FRAME_SEQUENTIAL_CHECK_SAMPLES)
      {
        CLog::LogF(m_framesMisplaced > 0 ? LOGWARNING : LOGINFO,
                   "Frame sequential: {} of {} frames were not shown on their refresh, frames "
                   "were handed over {:.1f} ms (worst {:.1f} ms) after the refresh tick",
                   m_framesMisplaced, m_framesChecked,
                   m_submitLagFrames > 0 ? m_submitLagSumMs / m_submitLagFrames : 0.0,
                   m_submitLagMaxMs);
        CLog::LogF(LOGINFO,
                   "Frame sequential: mean offset left {:.2f} right {:.2f} refreshes, frames shown "
                   "0/1/2/3+ refreshes after the previous one: {}/{}/{}/{}",
                   m_eyeOffsetCount[0] > 0 ? m_eyeOffsetSum[0] / m_eyeOffsetCount[0] : 0.0,
                   m_eyeOffsetCount[1] > 0 ? m_eyeOffsetSum[1] / m_eyeOffsetCount[1] : 0.0,
                   m_spacingCount[0], m_spacingCount[1], m_spacingCount[2], m_spacingCount[3]);
        m_eyeOffsetSum[0] = m_eyeOffsetSum[1] = 0.0;
        m_eyeOffsetCount[0] = m_eyeOffsetCount[1] = 0;
        m_spacingCount[0] = m_spacingCount[1] = m_spacingCount[2] = m_spacingCount[3] = 0;
        m_submitLagSumMs = 0.0;
        m_submitLagMaxMs = 0.0;
        m_submitLagFrames = 0;
        const bool restart = m_framesMisplaced * 4 > m_framesChecked;
        m_framesChecked = 0;
        m_framesMisplaced = 0;
        if (restart)
        {
          m_phaseState = FrameSequentialPhase::CALIBRATE;
          m_phaseSamples = 0;
          m_phaseSin = 0.0;
          m_phaseCos = 0.0;
        }
      }
      break;
  }
}

bool CWinSystemAndroidGLESContext::PresentFrameSequential(CGraphicContext& gfxContext)
{
  EGLint width{0};
  EGLint height{0};
  const EGLDisplay display = m_pGLContext.GetEGLDisplay();
  const EGLSurface surface = m_pGLContext.GetEGLSurface();
  if (surface == EGL_NO_SURFACE || !eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
      !eglQuerySurface(display, surface, EGL_HEIGHT, &height))
    return false;

  const float refreshRate = gfxContext.GetFPS();
  if (refreshRate < STEREO_FRAME_SEQUENTIAL_MIN_REFRESH)
  {
    // Passing through a slow mode, e.g. on the way out of playback
    m_frameSequentialPresenter.reset();
    return false;
  }

  if (m_frameSequentialPresenter &&
      !m_frameSequentialPresenter->Matches(width, height, refreshRate))
    m_frameSequentialPresenter.reset();

  if (!m_frameSequentialPresenter)
  {
    if (m_frameSequentialPresenterFailed)
      return false;

    m_frameSequentialPresenter = CFrameSequentialPresenter::Create(
        m_nativeWindow->GetWindow(), display, width, height, refreshRate);
    if (!m_frameSequentialPresenter)
    {
      CLog::LogF(LOGWARNING, "Frame sequential: no surface control output, falling back to "
                             "EGL presentation times");
      m_frameSequentialPresenterFailed = true;
      return false;
    }
  }

  // The window surface is never swapped while the presenter runs: its back buffer is only the
  // canvas each eye is drawn on before it is copied out.
  const bool rightEye = gfxContext.GetFrameSequentialEye() == RenderStereoView::RIGHT;
  m_frameSequentialPresenter->CaptureEye(rightEye);
  if (rightEye)
    m_frameSequentialPresenter->SubmitPair(
        CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_VIDEOSCREEN_FRAMESEQUENTIALSWAPEYES));

  return true;
}

bool CWinSystemAndroidGLESContext::GetVsyncTimeline(int64_t& count,
                                                    int64_t& timeNanos,
                                                    int64_t& periodNanos)
{
  return CXBMCApp::Get().GetVsyncTimeline(count, timeNanos, periodNanos);
}

float CWinSystemAndroidGLESContext::GetFrameLatencyAdjustment()
{
  // A pair rendered now is shown this far ahead; the video frame for that moment is wanted
  if (m_frameSequentialPresenter)
    return -static_cast<float>(m_frameSequentialPresenter->GetPresentLeadMs());

  return CXBMCApp::Get().GetFrameLatencyMs();
}

EGLDisplay CWinSystemAndroidGLESContext::GetEGLDisplay() const
{
  return m_pGLContext.GetEGLDisplay();
}

EGLSurface CWinSystemAndroidGLESContext::GetEGLSurface() const
{
  return m_pGLContext.GetEGLSurface();
}

EGLContext CWinSystemAndroidGLESContext::GetEGLContext() const
{
  return m_pGLContext.GetEGLContext();
}

EGLConfig  CWinSystemAndroidGLESContext::GetEGLConfig() const
{
  return m_pGLContext.GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAndroidGLESContext::GetVideoSync(CVideoReferenceClock* clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAndroid(clock));
  return pVSync;
}

bool CWinSystemAndroidGLESContext::CreateSurface()
{
  if (!m_pGLContext.CreateSurface(static_cast<EGLNativeWindowType>(m_nativeWindow->GetWindow()),
                                  m_HDRColorSpace))
  {
    if (m_HDRColorSpace != EGL_NONE)
    {
      // Loud on purpose: this silently drops the GUI back to sRGB while callers that asked
      // for HDR carry on. Without it, an HDR GUI surface can disappear at any surface rebuild
      // (a display mode change, an HDMI state flip) with nothing in the log to say so.
      CLog::Log(LOGWARNING,
                "CWinSystemAndroidGLESContext::CreateSurface: could not create the surface with "
                "colorspace {}, falling back to sRGB",
                m_HDRColorSpace);
      m_HDRColorSpace = EGL_NONE;
      m_displayMetadata = nullptr;
      m_lightMetadata = nullptr;
      if (!m_pGLContext.CreateSurface(
              static_cast<EGLNativeWindowType>(m_nativeWindow->GetWindow())))
        return false;
    }
    else
      return false;
  }

#if EGL_EXT_surface_SMPTE2086_metadata
  if (m_displayMetadata)
  {
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_RX_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[0][0]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_RY_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[0][1]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_GX_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[1][0]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_GY_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[1][1]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_BX_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[2][0]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_DISPLAY_PRIMARY_BY_EXT, static_cast<int>(av_q2d(m_displayMetadata->display_primaries[2][1]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_WHITE_POINT_X_EXT, static_cast<int>(av_q2d(m_displayMetadata->white_point[0]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_WHITE_POINT_Y_EXT, static_cast<int>(av_q2d(m_displayMetadata->white_point[1]) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_MAX_LUMINANCE_EXT, static_cast<int>(av_q2d(m_displayMetadata->max_luminance) * EGL_METADATA_SCALING_EXT + 0.5));
    m_pGLContext.SurfaceAttrib(EGL_SMPTE2086_MIN_LUMINANCE_EXT, static_cast<int>(av_q2d(m_displayMetadata->min_luminance) * EGL_METADATA_SCALING_EXT + 0.5));
  }
  if (m_lightMetadata)
  {
    m_pGLContext.SurfaceAttrib(EGL_CTA861_3_MAX_CONTENT_LIGHT_LEVEL_EXT, static_cast<int>(m_lightMetadata->MaxCLL * EGL_METADATA_SCALING_EXT));
    m_pGLContext.SurfaceAttrib(EGL_CTA861_3_MAX_FRAME_AVERAGE_LEVEL_EXT, static_cast<int>(m_lightMetadata->MaxFALL * EGL_METADATA_SCALING_EXT));
  }
#endif
  return true;
}

bool CWinSystemAndroidGLESContext::IsHDRDisplay()
{
  return m_hasHDRConfig && (m_hasEGL_BT2020_PQ_Colorspace_Extension || m_hasEGL_ST2086_Extension) &&
         CWinSystemAndroid::IsHDRDisplay();
}

bool CWinSystemAndroidGLESContext::SetHDR(const VideoPicture* videoPicture)
{
  if (!CServiceBroker::GetWinSystem()->IsHDRDisplaySettingEnabled())
    return false;

  EGLint HDRColorSpace = 0;

#if EGL_EXT_gl_colorspace_bt2020_linear
  if (m_hasHDRConfig && m_hasEGL_BT2020_PQ_Colorspace_Extension && m_hasEGL_ST2086_Extension)
  {
    HDRColorSpace = EGL_NONE;
    // Like CWinSystemGbm::SetHDR: HDR is decided by the transfer curve (PQ/HLG/DoVi), not by
    // mastering metadata (a PQ GUI surface may legitimately carry none) and not by the colour
    // matrix (BT.709 is the matrix of every SDR stream).
    if (videoPicture && (videoPicture->color_transfer == AVCOL_TRC_SMPTE2084 ||
                         videoPicture->color_transfer == AVCOL_TRC_ARIB_STD_B67 ||
                         videoPicture->hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION))
    {
      HDRColorSpace = EGL_GL_COLORSPACE_BT2020_PQ_EXT;
    }

    if (HDRColorSpace != m_HDRColorSpace)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAndroidGLESContext::SetHDR: ColorSpace: {}", HDRColorSpace);

      m_HDRColorSpace = HDRColorSpace;
      m_displayMetadata =
          (m_HDRColorSpace == EGL_NONE || !videoPicture || !videoPicture->hasDisplayMetadata)
              ? nullptr
              : std::make_unique<AVMasteringDisplayMetadata>(videoPicture->displayMetadata);
      // Attach content-light metadata (CTA861.3 MaxCLL/MaxFALL) too when supplied, so an HDR
      // GUI/overlay surface advertises the same envelope as the video plane and the compositor
      // has no reason to re-derive the HDMI HDR infoframe on GUI redraws.
      m_lightMetadata =
          (m_HDRColorSpace == EGL_NONE || !videoPicture || !videoPicture->hasLightMetadata)
              ? nullptr
              : std::make_unique<AVContentLightMetadata>(videoPicture->lightMetadata);
      m_pGLContext.DestroySurface();
      CreateSurface();
      m_pGLContext.BindContext();
    }
  }
#endif

  return m_HDRColorSpace == HDRColorSpace;
}
