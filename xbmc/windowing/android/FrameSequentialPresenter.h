/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <EGL/egl.h>

struct ANativeWindow;
struct AHardwareBuffer;
struct ASurfaceControl;
struct ASurfaceTransactionStats;

/*!
 * \brief Frame sequential 3D through an ASurfaceControl child layer (Android 10 and later).
 *
 * Every eye is copied out of the GL window surface into a buffer of our own and handed to
 * SurfaceFlinger in a transaction of its own, pinned to one display refresh by its desired present
 * time. A thread of its own submits a left/right pair every two refreshes: the newest finished
 * pair, or the previous one again when rendering fell behind. The display therefore always shows
 * left and right on the same alternating refreshes, whatever the render thread does. Present
 * fences report the refresh each buffer really reached; they keep the refresh grid and the
 * submission timing in line with the display.
 */
class CFrameSequentialPresenter
{
public:
  static std::unique_ptr<CFrameSequentialPresenter> Create(
      ANativeWindow* window, EGLDisplay display, int width, int height, float refreshRate);
  ~CFrameSequentialPresenter();

  CFrameSequentialPresenter(const CFrameSequentialPresenter&) = delete;
  CFrameSequentialPresenter& operator=(const CFrameSequentialPresenter&) = delete;

  bool Matches(int width, int height, float refreshRate) const;

  //! @brief Copy the eye just rendered to the default framebuffer. Render thread, context current.
  bool CaptureEye(bool rightEye);

  //! @brief Hand the captured pair over. Blocks while the previous pair waits for its refreshes,
  //!        which paces the render thread to one pair per two refreshes.
  void SubmitPair(bool swapEyes);

  //! @brief Time from now until a pair rendered now reaches the display.
  double GetPresentLeadMs();

  struct Shared;

private:
  enum class BufferState
  {
    FREE,
    CAPTURED,
    LATEST
  };

  struct Buffer
  {
    AHardwareBuffer* hardwareBuffer{nullptr};
    void* image{nullptr};
    unsigned int texture{0};
    unsigned int framebuffer{0};
    BufferState state{BufferState::FREE};
    int submissions{0};
    std::vector<int> fences;
  };

  struct Pair
  {
    int left{-1};
    int right{-1};
    int leftFence{-1};
    int rightFence{-1};
    bool Valid() const { return left >= 0 && right >= 0; }
  };

  struct Submission
  {
    uint64_t seq;
    int buffer;
    int64_t slot;
    uint64_t epoch;
    int64_t submitted;
    bool completed{false};
    int presentFence{-1};
    int releaseFence{-1};
  };

  struct Measurement
  {
    int64_t slot;
    uint64_t epoch;
    int fence;
    int64_t queued;
  };

  enum class Feedback
  {
    CALIBRATE,
    RUN
  };

  CFrameSequentialPresenter(EGLDisplay display, int width, int height, float refreshRate);

  bool Init(ANativeWindow* window);
  bool InitBuffer(Buffer& buffer);
  void Run();

  int AcquireBuffer(std::unique_lock<std::mutex>& lock);
  void CopyToBuffer(const Buffer& buffer);
  int CreateFence();
  void ReleasePair(Pair& pair);
  void ProcessCompletions();
  void ProcessMeasurements();
  void Measure(int64_t slot, uint64_t epoch, int64_t presentTime);
  void Adapt();
  void LogStats();
  uint64_t Record(int buffer, int64_t slot);
  void Apply(int buffer, int64_t desiredTime, uint64_t seq);

  void UpdateGrid();
  int64_t LeftSlotParity() const;
  void TrackParity(int64_t offset);
  int64_t SlotTime(int64_t slot) const;
  int64_t SlotAt(int64_t time) const;
  int64_t LeadNanos() const;
  int64_t MinLeadNanos() const;

  static void OnTransactionComplete(void* context, ASurfaceTransactionStats* stats);

  EGLDisplay m_display;
  const int m_width;
  const int m_height;
  const float m_refreshRate;
  const double m_nominalPeriod;
  bool m_blit{false};
  bool m_nativeFence{false};
  ASurfaceControl* m_surface{nullptr};
  std::vector<Buffer> m_buffers;

  std::shared_ptr<Shared> m_shared;
  std::thread m_thread;
  bool m_stop{false};
  bool m_bufferWarned{false};

  Pair m_capture;
  Pair m_ready;
  Pair m_latest;
  bool m_swapEyes{false};

  std::deque<Submission> m_submissions;
  std::deque<Measurement> m_measurements;
  int m_shownBuffer{-1};
  uint64_t m_seq{0};
  uint64_t m_epoch{0};

  enum class GridSource
  {
    CHOREOGRAPHER,
    DISPLAY
  };

  // Refresh grid: refresh n is shown at m_gridTime + (n - m_gridCount) * m_period. Once present
  // fences arrive, the grid follows the refreshes the display really shows and numbers them by
  // itself, so a Choreographer clock that runs apart from the HDMI output can not renumber them.
  // The Choreographer only stands in until then, or when the fences stop.
  GridSource m_gridSource{GridSource::CHOREOGRAPHER};
  int64_t m_gridCount{0};
  int64_t m_gridTime{0};
  double m_period;
  int64_t m_refCount{0};
  int64_t m_refTime{0};
  int m_offGrid{0};
  int64_t m_lastFeedback{0};
  // Choreographer stand-in: our refresh number minus the Choreographer count, and the time from a
  // Choreographer tick to the refresh it stands for
  int64_t m_countOffset{0};
  int64_t m_choreographerLag{0};
  // Where the Choreographer ticks sit in the display grid, in refreshes, to log their drift
  double m_choreographerPosition{0.0};
  double m_choreographerPositionStart{0.0};
  bool m_hasChoreographerPosition{false};
  double m_choreographerPeriod{0.0};

  // Refreshes between the slot a buffer was meant for and the one it reached, while all is well.
  // Eyes are pinned to the parity of the refresh reached, not of the slot asked for.
  int64_t m_baseline{0};

  // Refreshes between slot asked for and refresh reached, as seen in the latest buffers (median
  // of a few). The eyes are pinned to this one instead of m_baseline: m_baseline is only
  // re-measured after a calibration and can be wrong for seconds after a hiccup of the
  // compositor, which showed as swapped eyes.
  int64_t m_parityOffset{0};
  std::vector<int64_t> m_recentOffsets;
  uint64_t m_recentEpoch{0};

  // Direction (-1 early, +1 late) of the last window that asked for a change of the present time
  int m_adaptDirection{0};

  // Desired present time of a buffer, in refreshes before the refresh it is meant for
  double m_presentAhead;
  Feedback m_feedback{Feedback::CALIBRATE};
  std::vector<int64_t> m_calibration;

  bool m_started{false};
  int64_t m_nextSlot{0};

  int m_checked{0};
  int m_late{0};
  int m_early{0};
  int m_adaptChecked{0};
  int m_adaptLate{0};
  int m_adaptEarly{0};
  int64_t m_runDelta{0};
  int m_runLength{0};
  bool m_drainTried{false};
  int m_newPairs{0};
  int m_repeatedPairs{0};
  int m_skippedPairs{0};
  int m_droppedPairs{0};
  int m_withoutPresentFence{0};
  int m_unreadableFence{0};
  int64_t m_statsStart{0};
};
