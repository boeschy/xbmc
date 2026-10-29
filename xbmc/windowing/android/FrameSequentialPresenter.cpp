/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FrameSequentialPresenter.h"

#include "utils/EGLUtils.h"
#include "utils/log.h"

#include "platform/android/activity/XBMCApp.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <EGL/eglext.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "system_gl.h"

// The NDK headers only declare these for __ANDROID_API__ >= 29; Kodi builds for an older minimum
// SDK, so everything is resolved at run time and declared here.
struct ASurfaceTransaction;

using namespace std::chrono_literals;

namespace
{
constexpr int BUFFER_COUNT{12};

constexpr int8_t SURFACE_VISIBILITY_SHOW{1};
constexpr int8_t SURFACE_TRANSPARENCY_OPAQUE{2};
constexpr int32_t WINDOW_TRANSFORM_MIRROR_VERTICAL{0x02};

constexpr uint32_t HARDWARE_BUFFER_FORMAT_R8G8B8A8_UNORM{1};
constexpr uint64_t HARDWARE_BUFFER_USAGE_GPU_SAMPLED_IMAGE{1ULL << 8};
constexpr uint64_t HARDWARE_BUFFER_USAGE_GPU_FRAMEBUFFER{1ULL << 9};
constexpr uint64_t HARDWARE_BUFFER_USAGE_COMPOSER_OVERLAY{1ULL << 11};

constexpr EGLenum EGL_NATIVE_BUFFER_ANDROID_TARGET{0x3140};
constexpr EGLint EGL_IMAGE_PRESERVED_ATTRIB{0x30D2};
constexpr EGLenum EGL_SYNC_NATIVE_FENCE_TYPE{0x3144};
constexpr EGLint EGL_SYNC_NATIVE_FENCE_FD_ATTRIB{0x3145};
constexpr EGLint EGL_NO_NATIVE_FENCE_FD{-1};

constexpr GLenum GL_READ_FRAMEBUFFER_TARGET{0x8CA8};
constexpr GLenum GL_DRAW_FRAMEBUFFER_TARGET{0x8CA9};
constexpr GLenum GL_READ_FRAMEBUFFER_BINDING_QUERY{0x8CAA};
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING_QUERY{0x8CA6};

// Desired present time, in refreshes before the refresh a buffer is meant for. SurfaceFlinger
// applies a transaction in the first composition whose expected present time is past it, so half
// a refresh sits in the middle of the window that leads to the intended refresh.
constexpr double PRESENT_AHEAD_START{0.5};
constexpr double PRESENT_AHEAD_STEP{0.25};
constexpr double PRESENT_AHEAD_MIN{0.125};
constexpr double PRESENT_AHEAD_MAX{4.0};
// A pair is handed to SurfaceFlinger this many refreshes before its desired present time ...
constexpr double SUBMIT_LEAD_REFRESHES{2.5};
// ... and given up on (its refreshes repeat the previous pair) when less than this is left.
constexpr double SUBMIT_MIN_LEAD_REFRESHES{1.5};

constexpr int CALIBRATION_FRAMES{16};
// Buffers measured before misplaced ones are acted on
constexpr int ADAPT_FRAMES{240};
// Latest offsets the eye parity is taken from. Odd, so the median is one of them; one missed
// frame of the compositor must not swap the eyes, a lasting shift must within a few buffers.
constexpr size_t PARITY_FRAMES{5};
constexpr size_t PARITY_MIN_FRAMES{3};
constexpr int BASELINE_FOLLOW_FRAMES{8};
constexpr int64_t STATS_INTERVAL_NS{10000000000};
constexpr double PERIOD_TOLERANCE{0.005};
constexpr int64_t PERIOD_BASELINE_SLOTS{600};
constexpr int64_t FEEDBACK_TIMEOUT_NS{1000000000};
// Present fences this far off the grid in a row mean the display timing jumped (HDMI resync)
constexpr int OFF_GRID_RESYNC{8};
constexpr int64_t CALLBACK_TIMEOUT_NS{1000000000};

constexpr auto BUFFER_WAIT{100ms};
constexpr auto PAIR_WAIT{100ms};
constexpr auto IDLE_WAIT{10ms};
constexpr int64_t MAX_SLEEP_NS{4000000};
constexpr int PRESENTER_NICE{-10};

struct HardwareBufferDesc
{
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t format;
  uint64_t usage;
  uint32_t stride;
  uint32_t rfu0;
  uint64_t rfu1;
};

using OnCompleteFn = void (*)(void* context, ASurfaceTransactionStats* stats);

struct SurfaceControlApi
{
  ASurfaceControl* (*createFromWindow)(ANativeWindow*, const char*){nullptr};
  void (*releaseSurface)(ASurfaceControl*){nullptr};
  ASurfaceTransaction* (*create)(){nullptr};
  void (*destroy)(ASurfaceTransaction*){nullptr};
  void (*apply)(ASurfaceTransaction*){nullptr};
  void (*setOnComplete)(ASurfaceTransaction*, void*, OnCompleteFn){nullptr};
  void (*setBuffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int){nullptr};
  void (*setDesiredPresentTime)(ASurfaceTransaction*, int64_t){nullptr};
  void (*setVisibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t){nullptr};
  void (*setZOrder)(ASurfaceTransaction*, ASurfaceControl*, int32_t){nullptr};
  void (*setBufferTransparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t){nullptr};
  void (*setGeometry)(
      ASurfaceTransaction*, ASurfaceControl*, const ARect&, const ARect&, int32_t){nullptr};
  void (*reparent)(ASurfaceTransaction*, ASurfaceControl*, ASurfaceControl*){nullptr};
  int (*statsPresentFence)(ASurfaceTransactionStats*){nullptr};
  void (*statsControls)(ASurfaceTransactionStats*, ASurfaceControl***, size_t*){nullptr};
  void (*statsReleaseControls)(ASurfaceControl**){nullptr};
  int (*statsReleaseFence)(ASurfaceTransactionStats*, ASurfaceControl*){nullptr};
  int (*allocateBuffer)(const HardwareBufferDesc*, AHardwareBuffer**){nullptr};
  void (*releaseBuffer)(AHardwareBuffer*){nullptr};
  bool loaded{false};
};

struct GlApi
{
  EGLClientBuffer (*getNativeClientBuffer)(const AHardwareBuffer*){nullptr};
  void* (*createImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*){nullptr};
  EGLBoolean (*destroyImage)(EGLDisplay, void*){nullptr};
  void (*imageTargetTexture)(GLenum, void*){nullptr};
  void* (*createSync)(EGLDisplay, EGLenum, const EGLint*){nullptr};
  EGLBoolean (*destroySync)(EGLDisplay, void*){nullptr};
  EGLint (*dupNativeFence)(EGLDisplay, void*){nullptr};
  void (*blitFramebuffer)(
      GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum){nullptr};
  bool loaded{false};
};

template<typename T>
bool LoadSymbol(void* library, const char* name, T& function)
{
  function = library ? reinterpret_cast<T>(dlsym(library, name)) : nullptr;
  if (!function)
    CLog::Log(LOGWARNING, "Frame sequential: {} is not available", name);
  return function != nullptr;
}

template<typename T>
bool LoadEglSymbol(const char* name, T& function)
{
  function = reinterpret_cast<T>(eglGetProcAddress(name));
  return function != nullptr;
}

const SurfaceControlApi& GetSurfaceControlApi()
{
  static const SurfaceControlApi api = []
  {
    SurfaceControlApi a;
    void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    void* nativeWindow = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);

    bool ok = true;
    ok &= LoadSymbol(android, "ASurfaceControl_createFromWindow", a.createFromWindow);
    ok &= LoadSymbol(android, "ASurfaceControl_release", a.releaseSurface);
    ok &= LoadSymbol(android, "ASurfaceTransaction_create", a.create);
    ok &= LoadSymbol(android, "ASurfaceTransaction_delete", a.destroy);
    ok &= LoadSymbol(android, "ASurfaceTransaction_apply", a.apply);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setOnComplete", a.setOnComplete);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setBuffer", a.setBuffer);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setDesiredPresentTime",
                     a.setDesiredPresentTime);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setVisibility", a.setVisibility);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setZOrder", a.setZOrder);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setBufferTransparency",
                     a.setBufferTransparency);
    ok &= LoadSymbol(android, "ASurfaceTransaction_setGeometry", a.setGeometry);
    ok &= LoadSymbol(android, "ASurfaceTransaction_reparent", a.reparent);
    ok &= LoadSymbol(android, "ASurfaceTransactionStats_getPresentFenceFd", a.statsPresentFence);
    ok &= LoadSymbol(android, "ASurfaceTransactionStats_getASurfaceControls", a.statsControls);
    ok &= LoadSymbol(android, "ASurfaceTransactionStats_releaseASurfaceControls",
                     a.statsReleaseControls);
    ok &= LoadSymbol(android, "ASurfaceTransactionStats_getPreviousReleaseFenceFd",
                     a.statsReleaseFence);
    ok &= LoadSymbol(nativeWindow, "AHardwareBuffer_allocate", a.allocateBuffer);
    ok &= LoadSymbol(nativeWindow, "AHardwareBuffer_release", a.releaseBuffer);
    a.loaded = ok;
    return a;
  }();
  return api;
}

const GlApi& GetGlApi()
{
  static const GlApi api = []
  {
    GlApi a;
    bool ok = true;
    ok &= LoadEglSymbol("eglGetNativeClientBufferANDROID", a.getNativeClientBuffer);
    ok &= LoadEglSymbol("eglCreateImageKHR", a.createImage);
    ok &= LoadEglSymbol("eglDestroyImageKHR", a.destroyImage);
    ok &= LoadEglSymbol("glEGLImageTargetTexture2DOES", a.imageTargetTexture);
    LoadEglSymbol("eglCreateSyncKHR", a.createSync);
    LoadEglSymbol("eglDestroySyncKHR", a.destroySync);
    LoadEglSymbol("eglDupNativeFenceFDANDROID", a.dupNativeFence);
    LoadEglSymbol("glBlitFramebuffer", a.blitFramebuffer);
    a.loaded = ok;
    return a;
  }();
  return api;
}

int64_t Now()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::chrono::steady_clock::time_point ToTimePoint(int64_t nanos)
{
  return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(nanos));
}

void CloseFd(int& fd)
{
  if (fd >= 0)
    close(fd);
  fd = -1;
}

bool FenceSignaled(int fd)
{
  if (fd < 0)
    return true;
  pollfd request{fd, POLLIN, 0};
  return poll(&request, 1, 0) != 0;
}

//! @return 1 with the signal time, 0 while pending, -1 when it can not be told
int FenceTimeFromInfo(const sync_file_info& info,
                      const sync_fence_info* fences,
                      int64_t& time)
{
  if (info.status == 0)
    return 0;
  if (info.status < 0 || info.num_fences == 0 || !fences)
    return -1;

  time = 0;
  for (uint32_t i = 0; i < info.num_fences; ++i)
    time = std::max(time, static_cast<int64_t>(fences[i].timestamp_ns));
  return time > 0 ? 1 : -1;
}

//! @return 1 with the signal time, 0 while pending, -1 when it can not be told
int FenceSignalTime(int fd, int64_t& time)
{
  // libsync also understands the legacy sync driver some kernels (Tegra among them) still use
  using SyncFileInfoFn = struct sync_file_info* (*)(int32_t);
  using SyncFileInfoFreeFn = void (*)(struct sync_file_info*);
  static const auto libsync = []
  {
    std::pair<SyncFileInfoFn, SyncFileInfoFreeFn> functions{nullptr, nullptr};
    if (void* library = dlopen("libsync.so", RTLD_NOW | RTLD_LOCAL))
    {
      functions.first = reinterpret_cast<SyncFileInfoFn>(dlsym(library, "sync_file_info"));
      functions.second = reinterpret_cast<SyncFileInfoFreeFn>(dlsym(library, "sync_file_info_free"));
    }
    if (!functions.first || !functions.second)
      functions = {nullptr, nullptr};
    return functions;
  }();

  if (libsync.first)
  {
    struct sync_file_info* info = libsync.first(fd);
    if (!info)
      return -1;
    const int result = FenceTimeFromInfo(
        *info, reinterpret_cast<const sync_fence_info*>(static_cast<uintptr_t>(info->sync_fence_info)),
        time);
    libsync.second(info);
    return result;
  }

  sync_file_info info{};
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0)
    return -1;
  if (info.status <= 0 || info.num_fences == 0)
    return FenceTimeFromInfo(info, nullptr, time);

  std::vector<sync_fence_info> fences(info.num_fences);
  sync_file_info details{};
  details.num_fences = info.num_fences;
  details.sync_fence_info = static_cast<__u64>(reinterpret_cast<uintptr_t>(fences.data()));
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &details) < 0)
    return -1;

  return FenceTimeFromInfo(details, fences.data(), time);
}
} // namespace

struct CFrameSequentialPresenter::Shared
{
  struct Completion
  {
    uint64_t seq;
    int presentFence;
    int releaseFence;
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Completion> completions;
  bool closed{false};
};

namespace
{
struct CallbackContext
{
  std::shared_ptr<CFrameSequentialPresenter::Shared> shared;
  ASurfaceControl* surface;
  uint64_t seq;
};
} // namespace

void CFrameSequentialPresenter::OnTransactionComplete(void* context, ASurfaceTransactionStats* stats)
{
  const std::unique_ptr<CallbackContext> callback(static_cast<CallbackContext*>(context));
  const SurfaceControlApi& api = GetSurfaceControlApi();

  Shared::Completion completion{callback->seq, api.statsPresentFence(stats), -1};

  // Asking for a surface the stats do not carry aborts the process, so look it up first
  ASurfaceControl** controls{nullptr};
  size_t count{0};
  api.statsControls(stats, &controls, &count);
  for (size_t i = 0; i < count; ++i)
  {
    if (controls[i] == callback->surface)
    {
      completion.releaseFence = api.statsReleaseFence(stats, controls[i]);
      break;
    }
  }
  if (controls)
    api.statsReleaseControls(controls);

  std::lock_guard lock(callback->shared->mutex);
  if (callback->shared->closed)
  {
    CloseFd(completion.presentFence);
    CloseFd(completion.releaseFence);
    return;
  }
  callback->shared->completions.push_back(completion);
  callback->shared->cv.notify_all();
}

std::unique_ptr<CFrameSequentialPresenter> CFrameSequentialPresenter::Create(
    ANativeWindow* window, EGLDisplay display, int width, int height, float refreshRate)
{
  if (!window || display == EGL_NO_DISPLAY || width <= 0 || height <= 0 || refreshRate < 1.0f)
    return nullptr;

  if (!GetSurfaceControlApi().loaded)
  {
    CLog::Log(LOGWARNING, "Frame sequential: ASurfaceControl needs Android 10 or later");
    return nullptr;
  }
  if (!GetGlApi().loaded || !CEGLUtils::HasExtension(display, "EGL_ANDROID_image_native_buffer") ||
      !CEGLUtils::HasExtension(display, "EGL_ANDROID_get_native_client_buffer"))
  {
    CLog::Log(LOGWARNING, "Frame sequential: EGL can not render into hardware buffers");
    return nullptr;
  }

  std::unique_ptr<CFrameSequentialPresenter> presenter(
      new CFrameSequentialPresenter(display, width, height, refreshRate));
  if (!presenter->Init(window))
    return nullptr;

  return presenter;
}

CFrameSequentialPresenter::CFrameSequentialPresenter(EGLDisplay display,
                                                     int width,
                                                     int height,
                                                     float refreshRate)
  : m_display(display),
    m_width(width),
    m_height(height),
    m_refreshRate(refreshRate),
    m_nominalPeriod(1.0e9 / static_cast<double>(refreshRate)),
    m_shared(std::make_shared<Shared>()),
    m_period(m_nominalPeriod),
    m_presentAhead(PRESENT_AHEAD_START)
{
}

bool CFrameSequentialPresenter::Init(ANativeWindow* window)
{
  const SurfaceControlApi& api = GetSurfaceControlApi();
  const GlApi& gl = GetGlApi();

  int glMajor{0};
  if (const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION)))
    std::sscanf(version, "OpenGL ES %d", &glMajor);
  m_blit = gl.blitFramebuffer && glMajor >= 3;
  m_nativeFence = gl.createSync && gl.destroySync && gl.dupNativeFence &&
                  CEGLUtils::HasExtension(m_display, "EGL_ANDROID_native_fence_sync");

  m_buffers.resize(BUFFER_COUNT);
  for (Buffer& buffer : m_buffers)
  {
    if (!InitBuffer(buffer))
      return false;
  }

  m_surface = api.createFromWindow(window, "Kodi frame sequential 3D");
  if (!m_surface)
  {
    CLog::Log(LOGERROR, "Frame sequential: could not create a surface control");
    return false;
  }

  // GL writes the bottom row first, the display shows the first row at the top. A blit turns the
  // picture over on the way into the buffer, a plain copy leaves that to the composer.
  const ARect source{0, 0, m_width, m_height};
  const ARect destination{0, 0, ANativeWindow_getWidth(window), ANativeWindow_getHeight(window)};
  ASurfaceTransaction* transaction = api.create();
  api.setVisibility(transaction, m_surface, SURFACE_VISIBILITY_SHOW);
  api.setZOrder(transaction, m_surface, 1);
  api.setBufferTransparency(transaction, m_surface, SURFACE_TRANSPARENCY_OPAQUE);
  api.setGeometry(transaction, m_surface, source, destination,
                  m_blit ? 0 : WINDOW_TRANSFORM_MIRROR_VERTICAL);
  api.apply(transaction);
  api.destroy(transaction);

  m_gridTime = Now();
  UpdateGrid();
  m_statsStart = Now();

  CLog::Log(LOGINFO,
            "Frame sequential: presenting {}x{} at {:.3f} Hz through a surface control ({}, {})",
            m_width, m_height, m_refreshRate, m_blit ? "blit" : "copy",
            m_nativeFence ? "native fences" : "glFinish");

  m_thread = std::thread(&CFrameSequentialPresenter::Run, this);
  return true;
}

bool CFrameSequentialPresenter::InitBuffer(Buffer& buffer)
{
  const SurfaceControlApi& api = GetSurfaceControlApi();
  const GlApi& gl = GetGlApi();

  HardwareBufferDesc desc{};
  desc.width = static_cast<uint32_t>(m_width);
  desc.height = static_cast<uint32_t>(m_height);
  desc.layers = 1;
  desc.format = HARDWARE_BUFFER_FORMAT_R8G8B8A8_UNORM;
  desc.usage = HARDWARE_BUFFER_USAGE_GPU_SAMPLED_IMAGE | HARDWARE_BUFFER_USAGE_GPU_FRAMEBUFFER |
               HARDWARE_BUFFER_USAGE_COMPOSER_OVERLAY;
  if (api.allocateBuffer(&desc, &buffer.hardwareBuffer) != 0)
  {
    desc.usage &= ~HARDWARE_BUFFER_USAGE_COMPOSER_OVERLAY;
    if (api.allocateBuffer(&desc, &buffer.hardwareBuffer) != 0)
    {
      buffer.hardwareBuffer = nullptr;
      CLog::Log(LOGERROR, "Frame sequential: could not allocate a {}x{} hardware buffer", m_width,
                m_height);
      return false;
    }
  }

  const EGLClientBuffer clientBuffer = gl.getNativeClientBuffer(buffer.hardwareBuffer);
  const EGLint attribs[] = {EGL_IMAGE_PRESERVED_ATTRIB, EGL_TRUE, EGL_NONE};
  buffer.image = clientBuffer ? gl.createImage(m_display, EGL_NO_CONTEXT,
                                               EGL_NATIVE_BUFFER_ANDROID_TARGET, clientBuffer,
                                               attribs)
                              : nullptr;
  if (!buffer.image)
  {
    CLog::Log(LOGERROR, "Frame sequential: could not wrap a hardware buffer in an EGL image");
    return false;
  }

  GLint previousTexture{0};
  GLint previousFramebuffer{0};
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

  // Errors left over from earlier rendering would be taken for ours
  while (glGetError() != GL_NO_ERROR)
  {
  }

  glGenTextures(1, &buffer.texture);
  glBindTexture(GL_TEXTURE_2D, buffer.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl.imageTargetTexture(GL_TEXTURE_2D, buffer.image);
  const GLenum textureError = glGetError();
  bool ok = textureError == GL_NO_ERROR;

  GLenum framebufferStatus{GL_FRAMEBUFFER_COMPLETE};
  if (ok && m_blit)
  {
    glGenFramebuffers(1, &buffer.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, buffer.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.texture, 0);
    framebufferStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    ok = framebufferStatus == GL_FRAMEBUFFER_COMPLETE;
  }

  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));

  if (!ok)
    CLog::Log(LOGERROR,
              "Frame sequential: a hardware buffer can not be rendered to (GL error {:#x}, "
              "framebuffer status {:#x})",
              textureError, framebufferStatus);
  return ok;
}

CFrameSequentialPresenter::~CFrameSequentialPresenter()
{
  {
    std::lock_guard lock(m_shared->mutex);
    m_stop = true;
  }
  m_shared->cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();

  const SurfaceControlApi& api = GetSurfaceControlApi();
  if (m_surface)
  {
    ASurfaceTransaction* transaction = api.create();
    api.reparent(transaction, m_surface, nullptr);
    api.apply(transaction);
    api.destroy(transaction);
    api.releaseSurface(m_surface);
  }

  {
    std::lock_guard lock(m_shared->mutex);
    m_shared->closed = true;
    for (auto& completion : m_shared->completions)
    {
      CloseFd(completion.presentFence);
      CloseFd(completion.releaseFence);
    }
    m_shared->completions.clear();
  }

  for (Submission& submission : m_submissions)
  {
    CloseFd(submission.presentFence);
    CloseFd(submission.releaseFence);
  }
  for (Measurement& measurement : m_measurements)
    CloseFd(measurement.fence);
  for (Pair* pair : {&m_capture, &m_ready, &m_latest})
  {
    CloseFd(pair->leftFence);
    CloseFd(pair->rightFence);
  }

  const GlApi& gl = GetGlApi();
  for (Buffer& buffer : m_buffers)
  {
    for (int& fd : buffer.fences)
      CloseFd(fd);
    if (buffer.framebuffer)
      glDeleteFramebuffers(1, &buffer.framebuffer);
    if (buffer.texture)
      glDeleteTextures(1, &buffer.texture);
    if (buffer.image)
      gl.destroyImage(m_display, buffer.image);
    if (buffer.hardwareBuffer)
      api.releaseBuffer(buffer.hardwareBuffer);
  }
}

bool CFrameSequentialPresenter::Matches(int width, int height, float refreshRate) const
{
  return width == m_width && height == m_height && std::abs(refreshRate - m_refreshRate) < 0.01f;
}

bool CFrameSequentialPresenter::CaptureEye(bool rightEye)
{
  int& index = rightEye ? m_capture.right : m_capture.left;
  int& fence = rightEye ? m_capture.rightFence : m_capture.leftFence;
  {
    std::unique_lock lock(m_shared->mutex);
    // A left eye starts a new pair, whatever was left of an unfinished one goes
    if (!rightEye)
      ReleasePair(m_capture);
    else if (index >= 0)
    {
      m_buffers[index].state = BufferState::FREE;
      CloseFd(fence);
      index = -1;
    }

    index = AcquireBuffer(lock);
    if (index < 0)
    {
      if (!m_bufferWarned)
        CLog::Log(LOGWARNING, "Frame sequential: no free buffer, the display still holds them all");
      m_bufferWarned = true;
      return false;
    }
  }

  CopyToBuffer(m_buffers[index]);
  fence = CreateFence();
  return true;
}

void CFrameSequentialPresenter::SubmitPair(bool swapEyes)
{
  std::unique_lock lock(m_shared->mutex);
  if (!m_capture.Valid())
  {
    ReleasePair(m_capture);
    // Nothing to show this time, the presenter repeats the previous pair. Keep the pace anyway.
    m_shared->cv.wait_for(lock, std::chrono::nanoseconds(std::llround(2.0 * m_period)));
    return;
  }

  if (!m_shared->cv.wait_for(lock, PAIR_WAIT, [this] { return !m_ready.Valid() || m_stop; }))
  {
    ReleasePair(m_ready);
    ++m_droppedPairs;
  }

  m_ready = m_capture;
  m_capture = {};
  m_swapEyes = swapEyes;
  m_shared->cv.notify_all();
}

double CFrameSequentialPresenter::GetPresentLeadMs()
{
  std::lock_guard lock(m_shared->mutex);
  if (!m_started)
    return static_cast<double>(LeadNanos()) * 1.0e-6 + 2.0 * m_period * 1.0e-6;

  const int64_t slot = m_nextSlot + (m_ready.Valid() ? 2 : 0);
  return static_cast<double>(SlotTime(slot) - Now()) * 1.0e-6;
}

int CFrameSequentialPresenter::AcquireBuffer(std::unique_lock<std::mutex>& lock)
{
  const auto deadline = std::chrono::steady_clock::now() + BUFFER_WAIT;
  while (true)
  {
    ProcessCompletions();
    for (size_t i = 0; i < m_buffers.size(); ++i)
    {
      Buffer& buffer = m_buffers[i];
      if (buffer.state != BufferState::FREE || buffer.submissions > 0 ||
          !std::ranges::all_of(buffer.fences, FenceSignaled))
        continue;

      for (int& fd : buffer.fences)
        CloseFd(fd);
      buffer.fences.clear();
      buffer.state = BufferState::CAPTURED;
      return static_cast<int>(i);
    }

    if (std::chrono::steady_clock::now() >= deadline)
      return -1;
    m_shared->cv.wait_for(lock, 2ms);
  }
}

void CFrameSequentialPresenter::CopyToBuffer(const Buffer& buffer)
{
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  if (scissor)
    glDisable(GL_SCISSOR_TEST);

  if (m_blit)
  {
    GLint readFramebuffer{0};
    GLint drawFramebuffer{0};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_QUERY, &readFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_QUERY, &drawFramebuffer);

    glBindFramebuffer(GL_READ_FRAMEBUFFER_TARGET, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_TARGET, buffer.framebuffer);
    GetGlApi().blitFramebuffer(0, 0, m_width, m_height, 0, m_height, m_width, 0,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER_TARGET, static_cast<GLuint>(readFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_TARGET, static_cast<GLuint>(drawFramebuffer));
  }
  else
  {
    GLint framebuffer{0};
    GLint texture{0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, buffer.texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, m_width, m_height);

    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
  }

  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

int CFrameSequentialPresenter::CreateFence()
{
  if (m_nativeFence)
  {
    const GlApi& gl = GetGlApi();
    const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ATTRIB, EGL_NO_NATIVE_FENCE_FD, EGL_NONE};
    if (void* sync = gl.createSync(m_display, EGL_SYNC_NATIVE_FENCE_TYPE, attribs))
    {
      glFlush();
      const int fd = gl.dupNativeFence(m_display, sync);
      gl.destroySync(m_display, sync);
      if (fd >= 0)
        return fd;
    }
  }

  glFinish();
  return -1;
}

void CFrameSequentialPresenter::ReleasePair(Pair& pair)
{
  for (const int index : {pair.left, pair.right})
  {
    if (index >= 0)
      m_buffers[index].state = BufferState::FREE;
  }
  CloseFd(pair.leftFence);
  CloseFd(pair.rightFence);
  pair = {};
}

void CFrameSequentialPresenter::ProcessCompletions()
{
  for (Shared::Completion& completion : m_shared->completions)
  {
    const auto it = std::ranges::find_if(m_submissions, [&completion](const Submission& s)
                                         { return s.seq == completion.seq; });
    if (it == m_submissions.end() || it->completed)
    {
      CloseFd(completion.presentFence);
      CloseFd(completion.releaseFence);
      continue;
    }
    it->completed = true;
    it->presentFence = completion.presentFence;
    it->releaseFence = completion.releaseFence;
  }
  m_shared->completions.clear();

  // In submission order: a completed transaction ends the hold of the buffer shown before it,
  // which may be written again once that buffer's release fence and the present fence of its
  // successor signalled.
  const int64_t now = Now();
  while (!m_submissions.empty())
  {
    Submission& submission = m_submissions.front();
    if (!submission.completed && now - submission.submitted < CALLBACK_TIMEOUT_NS)
      break;

    if (m_shownBuffer >= 0)
    {
      Buffer& previous = m_buffers[m_shownBuffer];
      previous.submissions = std::max(0, previous.submissions - 1);
      if (submission.releaseFence >= 0)
      {
        previous.fences.push_back(submission.releaseFence);
        submission.releaseFence = -1;
      }
      if (submission.presentFence >= 0)
      {
        const int fd = dup(submission.presentFence);
        if (fd >= 0)
          previous.fences.push_back(fd);
      }
    }
    CloseFd(submission.releaseFence);

    if (submission.presentFence >= 0)
      m_measurements.push_back({submission.slot, submission.epoch, submission.presentFence, now});
    else
      ++m_withoutPresentFence;

    m_shownBuffer = submission.buffer;
    m_submissions.pop_front();
  }
}

void CFrameSequentialPresenter::ProcessMeasurements()
{
  while (!m_measurements.empty())
  {
    Measurement& measurement = m_measurements.front();
    int64_t presentTime{0};
    const int result = FenceSignalTime(measurement.fence, presentTime);
    if (result == 0 && Now() - measurement.queued < CALLBACK_TIMEOUT_NS)
      break;

    CloseFd(measurement.fence);
    const int64_t slot = measurement.slot;
    const uint64_t epoch = measurement.epoch;
    m_measurements.pop_front();
    if (result == 1)
      Measure(slot, epoch, presentTime);
    else
      ++m_unreadableFence;
  }
}

void CFrameSequentialPresenter::Measure(int64_t slot, uint64_t epoch, int64_t presentTime)
{
  m_lastFeedback = Now();
  const int64_t landed = SlotAt(presentTime);

  if (m_gridSource == GridSource::CHOREOGRAPHER)
  {
    // Take the grid over from the display; the numbering carries on, so no eye changes here
    m_gridSource = GridSource::DISPLAY;
    m_gridCount = landed;
    m_gridTime = presentTime;
    m_refCount = landed;
    m_refTime = presentTime;
    m_offGrid = 0;
    m_hasChoreographerPosition = false;
    ++m_epoch;
    CLog::Log(LOGINFO, "Frame sequential: refreshes are now counted from the display's present "
                       "fences");
    return;
  }

  const int64_t expected = SlotTime(landed);
  const int64_t residual = presentTime - expected;
  if (static_cast<double>(std::llabs(residual)) * 4.0 < m_period)
  {
    m_offGrid = 0;
    m_gridCount = landed;
    m_gridTime = expected + residual / 8;
  }
  else if (++m_offGrid >= OFF_GRID_RESYNC)
  {
    CLog::Log(LOGWARNING, "Frame sequential: the display timing jumped by {:.2f} refreshes, "
                          "following it",
              static_cast<double>(residual) / m_period);
    m_offGrid = 0;
    m_gridCount = landed;
    m_gridTime = presentTime;
    m_refCount = landed;
    m_refTime = presentTime;
    m_hasChoreographerPosition = false;
    m_feedback = Feedback::CALIBRATE;
    m_calibration.clear();
    ++m_epoch;
    return;
  }

  if (landed - m_refCount >= PERIOD_BASELINE_SLOTS)
  {
    const double measured =
        static_cast<double>(presentTime - m_refTime) / static_cast<double>(landed - m_refCount);
    if (std::abs(measured - m_nominalPeriod) < m_nominalPeriod * PERIOD_TOLERANCE)
      m_period = measured;
  }

  if (epoch != m_epoch)
    return;

  const int64_t offset = landed - slot;
  TrackParity(offset);
  if (m_feedback == Feedback::CALIBRATE)
  {
    m_calibration.push_back(offset);
    if (static_cast<int>(m_calibration.size()) < CALIBRATION_FRAMES)
      return;

    auto middle = m_calibration.begin() + m_calibration.size() / 2;
    std::nth_element(m_calibration.begin(), middle, m_calibration.end());
    if (*middle != m_baseline)
      ++m_epoch;
    m_baseline = *middle;
    if (m_recentOffsets.size() < PARITY_MIN_FRAMES)
      m_parityOffset = m_baseline;
    m_calibration.clear();
    m_feedback = Feedback::RUN;
    m_adaptChecked = m_adaptLate = m_adaptEarly = 0;
    m_runDelta = 0;
    m_runLength = 0;
    CLog::Log(LOGINFO,
              "Frame sequential: buffers reach the display {} refreshes after the one asked for, "
              "asking {:.2f} refreshes ahead",
              m_baseline, m_presentAhead);
    return;
  }

  const int64_t delta = offset - m_baseline;
  if (delta == 0)
    m_drainTried = false;

  if (delta != 0 && delta == m_runDelta)
  {
    ++m_runLength;
  }
  else
  {
    m_runDelta = delta;
    m_runLength = delta != 0 ? 1 : 0;
  }

  if (m_runLength >= BASELINE_FOLLOW_FRAMES && delta > 0 && !m_drainTried)
  {
    CLog::Log(LOGINFO,
              "Frame sequential: SurfaceFlinger holds {} buffers back, leaving a gap to let it "
              "catch up",
              delta);
    m_nextSlot += 2 * ((delta + 1) / 2);
    m_drainTried = true;
    m_runDelta = 0;
    m_runLength = 0;
    m_adaptChecked = m_adaptLate = m_adaptEarly = 0;
    ++m_epoch;
    return;
  }

  if (m_runLength >= BASELINE_FOLLOW_FRAMES && (delta & 1) != 0)
  {
    CLog::Log(LOGINFO,
              "Frame sequential: buffers now reach the display {} refreshes after the one asked "
              "for (was {}), moving the eyes with them",
              offset, m_baseline);
    m_baseline = offset;
    m_drainTried = false;
    m_runDelta = 0;
    m_runLength = 0;
    m_adaptChecked = m_adaptLate = m_adaptEarly = 0;
    ++m_epoch;
    return;
  }

  ++m_checked;
  ++m_adaptChecked;
  if (offset > m_baseline)
  {
    ++m_late;
    ++m_adaptLate;
  }
  else if (offset < m_baseline)
  {
    ++m_early;
    ++m_adaptEarly;
  }

  if (m_adaptChecked >= ADAPT_FRAMES)
    Adapt();
}

void CFrameSequentialPresenter::Adapt()
{
  // A buffer in the wrong refresh now and then is SurfaceFlinger missing a frame. Many of them,
  // all one way, mean the desired present time sits on the edge of the window that leads to the
  // right refresh. The eyes stay on their refreshes meanwhile: they follow the refresh reached.
  const int limit = m_adaptChecked / 10;
  double ahead = m_presentAhead;
  int direction{0};
  if (m_adaptLate > limit && m_adaptLate > 2 * m_adaptEarly)
    direction = 1;
  else if (m_adaptEarly > limit && m_adaptEarly > 2 * m_adaptLate)
    direction = -1;

  // A single window is often just a hiccup of the compositor, the buffers queued behind it
  // reach the display late and the queue drains again. Changing the present time then only
  // makes the next calibration measure the drain. Two windows in a row have to agree.
  const bool confirmed = direction != 0 && direction == m_adaptDirection;
  m_adaptDirection = direction;
  if (confirmed)
  {
    if (direction > 0)
      ahead = std::min(m_presentAhead + PRESENT_AHEAD_STEP, PRESENT_AHEAD_MAX);
    else
      ahead = std::max(m_presentAhead - PRESENT_AHEAD_STEP, PRESENT_AHEAD_MIN);
    m_adaptDirection = 0;
  }

  if (ahead != m_presentAhead)
  {
    CLog::Log(LOGWARNING,
              "Frame sequential: {} late and {} early of {} buffers, now asking {:.2f} refreshes "
              "ahead",
              m_adaptLate, m_adaptEarly, m_adaptChecked, ahead);
    m_presentAhead = ahead;
    m_feedback = Feedback::CALIBRATE;
    m_calibration.clear();
    ++m_epoch;
  }

  m_adaptChecked = m_adaptLate = m_adaptEarly = 0;
}

void CFrameSequentialPresenter::LogStats()
{
  const double seconds = static_cast<double>(Now() - m_statsStart) * 1.0e-9;
  if (m_gridSource == GridSource::DISPLAY)
  {
    const bool clean = m_late == 0 && m_early == 0 && m_skippedPairs == 0;
    CLog::Log(clean ? LOGINFO : LOGWARNING,
              "Frame sequential: {} of {} buffers off their refresh ({} late, {} early), pairs new "
              "{} / repeated {} / skipped {} / dropped {}, baseline {}, asking {:.2f} refreshes "
              "ahead",
              m_late + m_early, m_checked, m_late, m_early, m_newPairs, m_repeatedPairs,
              m_skippedPairs, m_droppedPairs, m_baseline, m_presentAhead);
    if (m_hasChoreographerPosition && m_choreographerPeriod > 0.0)
      CLog::Log(LOGINFO,
                "Frame sequential: display {:.4f} Hz, Choreographer {:.4f} Hz, the Choreographer "
                "moved {:+.3f} refreshes against the display in {:.1f} s",
                1.0e9 / m_period, 1.0e9 / m_choreographerPeriod,
                m_choreographerPosition - m_choreographerPositionStart, seconds);
  }
  else
  {
    CLog::Log(LOGWARNING,
              "Frame sequential: no display feedback ({} transactions without present fence, {} "
              "present fences without time), eyes follow the Choreographer count; pairs new {} / "
              "repeated {} / skipped {} / dropped {}, Choreographer {:.4f} Hz",
              m_withoutPresentFence, m_unreadableFence, m_newPairs, m_repeatedPairs,
              m_skippedPairs, m_droppedPairs, 1.0e9 / m_period);
  }

  m_checked = m_late = m_early = 0;
  m_newPairs = m_repeatedPairs = m_skippedPairs = m_droppedPairs = 0;
  m_withoutPresentFence = m_unreadableFence = 0;
  m_choreographerPositionStart = m_choreographerPosition;
  m_statsStart = Now();
}

uint64_t CFrameSequentialPresenter::Record(int buffer, int64_t slot)
{
  const uint64_t seq = ++m_seq;
  m_submissions.push_back({seq, buffer, slot, m_epoch, Now()});
  ++m_buffers[buffer].submissions;
  return seq;
}

void CFrameSequentialPresenter::Apply(int buffer, int64_t desiredTime, uint64_t seq)
{
  const SurfaceControlApi& api = GetSurfaceControlApi();
  ASurfaceTransaction* transaction = api.create();
  api.setBuffer(transaction, m_surface, m_buffers[buffer].hardwareBuffer, -1);
  api.setDesiredPresentTime(transaction, desiredTime);
  api.setOnComplete(transaction, new CallbackContext{m_shared, m_surface, seq},
                    &CFrameSequentialPresenter::OnTransactionComplete);
  api.apply(transaction);
  api.destroy(transaction);
}

void CFrameSequentialPresenter::Run()
{
  const id_t tid = static_cast<id_t>(syscall(SYS_gettid));
  if (setpriority(PRIO_PROCESS, tid, PRESENTER_NICE) != 0)
    CLog::Log(LOGWARNING, "Frame sequential: could not raise the presenter priority ({})", errno);

  std::unique_lock lock(m_shared->mutex);
  while (!m_stop)
  {
    UpdateGrid();
    ProcessCompletions();
    ProcessMeasurements();

    const int64_t now = Now();
    if (now - m_statsStart >= STATS_INTERVAL_NS)
      LogStats();

    if (!m_latest.Valid() && !m_ready.Valid())
    {
      m_started = false;
      m_shared->cv.wait_for(lock, IDLE_WAIT);
      continue;
    }

    if (!m_started)
    {
      m_nextSlot = SlotAt(now);
      while (SlotTime(m_nextSlot) - LeadNanos() < now)
        ++m_nextSlot;
      m_started = true;
    }
    if ((m_nextSlot & 1) != LeftSlotParity())
      ++m_nextSlot;
    while (SlotTime(m_nextSlot) - MinLeadNanos() < now)
    {
      m_nextSlot += 2;
      if (m_latest.Valid())
        ++m_skippedPairs;
    }

    const int64_t deadline = SlotTime(m_nextSlot) - LeadNanos();
    if (now < deadline)
    {
      m_shared->cv.wait_until(lock, ToTimePoint(std::min(deadline, now + MAX_SLEEP_NS)));
      continue;
    }

    if (m_ready.Valid() && FenceSignaled(m_ready.leftFence) && FenceSignaled(m_ready.rightFence))
    {
      for (const int index : {m_latest.left, m_latest.right})
      {
        if (index >= 0)
          m_buffers[index].state = BufferState::FREE;
      }
      m_latest = m_ready;
      CloseFd(m_latest.leftFence);
      CloseFd(m_latest.rightFence);
      m_buffers[m_latest.left].state = BufferState::LATEST;
      m_buffers[m_latest.right].state = BufferState::LATEST;
      m_ready = {};
      ++m_newPairs;
      m_shared->cv.notify_all();
    }
    else if (m_latest.Valid())
    {
      ++m_repeatedPairs;
    }
    else
    {
      // The very first pair is still on the GPU
      m_shared->cv.wait_for(lock, 1ms);
      continue;
    }

    const int64_t slot = m_nextSlot;
    m_nextSlot += 2;

    const int64_t ahead = std::llround(m_presentAhead * m_period);
    const int left = m_latest.left;
    const int right = m_latest.right;
    const uint64_t leftSeq = Record(left, slot);
    const uint64_t rightSeq = Record(right, slot + 1);
    const int64_t leftTime = SlotTime(slot) - ahead;
    const int64_t rightTime = SlotTime(slot + 1) - ahead;

    lock.unlock();
    Apply(left, leftTime, leftSeq);
    Apply(right, rightTime, rightSeq);
    lock.lock();
  }
}

void CFrameSequentialPresenter::UpdateGrid()
{
  int64_t count{0};
  int64_t time{0};
  int64_t period{0};
  if (!CXBMCApp::Get().GetVsyncTimeline(count, time, period))
    return;
  m_choreographerPeriod = static_cast<double>(period);

  if (m_gridSource == GridSource::DISPLAY && Now() - m_lastFeedback > FEEDBACK_TIMEOUT_NS)
  {
    // The fences stopped: let the Choreographer stand in, numbering carried on
    const int64_t slot = SlotAt(time);
    m_countOffset = slot - count;
    m_choreographerLag = SlotTime(slot) - time;
    m_gridSource = GridSource::CHOREOGRAPHER;
    m_feedback = Feedback::CALIBRATE;
    m_calibration.clear();
    ++m_epoch;
    CLog::Log(LOGWARNING, "Frame sequential: present fences stopped, counting refreshes from the "
                          "Choreographer");
  }

  if (m_gridSource == GridSource::DISPLAY)
  {
    const double position = static_cast<double>(time - m_gridTime) / m_period +
                            static_cast<double>(m_gridCount - count);
    if (!m_hasChoreographerPosition)
    {
      m_choreographerPositionStart = position;
      m_choreographerPosition = position;
      m_hasChoreographerPosition = true;
    }
    else
    {
      // Unwrapped, so a steady drift adds up instead of folding back
      m_choreographerPosition = position + std::round(m_choreographerPosition - position);
    }
    return;
  }

  if (std::abs(static_cast<double>(period) - m_nominalPeriod) < m_nominalPeriod * PERIOD_TOLERANCE)
    m_period = static_cast<double>(period);
  m_gridCount = count + m_countOffset;
  m_gridTime = time + m_choreographerLag;
}

void CFrameSequentialPresenter::TrackParity(int64_t offset)
{
  // Offsets of another epoch were asked for with another present time, they say nothing about
  // the buffers still to be submitted
  if (m_recentEpoch != m_epoch)
  {
    m_recentOffsets.clear();
    m_recentEpoch = m_epoch;
  }

  m_recentOffsets.push_back(offset);
  if (m_recentOffsets.size() > PARITY_FRAMES)
    m_recentOffsets.erase(m_recentOffsets.begin());
  if (m_recentOffsets.size() < PARITY_MIN_FRAMES)
    return;

  std::vector<int64_t> sorted(m_recentOffsets);
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const int64_t median = sorted[sorted.size() / 2];
  if (median != m_parityOffset)
  {
    if (((median - m_parityOffset) & 1) != 0)
      CLog::Log(LOGINFO,
                "Frame sequential: buffers now reach the display {} refreshes after the one asked "
                "for (was {}), the eyes follow",
                median, m_parityOffset);
    m_parityOffset = median;
  }
}

int64_t CFrameSequentialPresenter::LeftSlotParity() const
{
  // The left eye goes to even refreshes (odd ones when swapped), counted where buffers arrive
  return ((m_swapEyes ? 1 : 0) - m_parityOffset) & 1;
}

int64_t CFrameSequentialPresenter::SlotTime(int64_t slot) const
{
  return m_gridTime + std::llround(static_cast<double>(slot - m_gridCount) * m_period);
}

int64_t CFrameSequentialPresenter::SlotAt(int64_t time) const
{
  return m_gridCount + std::llround(static_cast<double>(time - m_gridTime) / m_period);
}

int64_t CFrameSequentialPresenter::LeadNanos() const
{
  return std::llround((m_presentAhead + SUBMIT_LEAD_REFRESHES) * m_period);
}

int64_t CFrameSequentialPresenter::MinLeadNanos() const
{
  return std::llround((m_presentAhead + SUBMIT_MIN_LEAD_REFRESHES) * m_period);
}
