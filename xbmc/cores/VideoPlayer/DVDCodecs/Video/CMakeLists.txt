set(SOURCES AddonVideoCodec.cpp
            DVDVideoCodec.cpp
            DVDVideoCodecFFmpeg.cpp)

set(HEADERS AddonVideoCodec.h
            DVDVideoCodec.h
            DVDVideoCodecFFmpeg.h
            DVDVideoPP.h)

if(TARGET ffmpeg::libpostproc)
  list(APPEND SOURCES DVDVideoPPFFmpeg.cpp)
  list(APPEND HEADERS DVDVideoPPFFmpeg.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL windows OR CORE_SYSTEM_NAME STREQUAL windowsstore)
  list(APPEND SOURCES DXVA.cpp)
  list(APPEND HEADERS DXVA.h)
endif()

if(TARGET ${APP_NAME_LC}::VDPAU)
  list(APPEND SOURCES VDPAU.cpp)
  list(APPEND HEADERS VDPAU.h)
endif()

if(TARGET ${APP_NAME_LC}::VAAPI)
  list(APPEND SOURCES VAAPI.cpp)
  list(APPEND HEADERS VAAPI.h)
endif()

if(APPLE)
  list(APPEND SOURCES VTB.cpp)
  list(APPEND HEADERS VTB.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND SOURCES DVDVideoCodecAndroidMediaCodec.cpp)
  list(APPEND HEADERS DVDVideoCodecAndroidMediaCodec.h)

  # PoC: software H.264/MVC (Stereo High Profile) decoder, see
  # DVDVideoCodecMVCSoftware.h for scope/rationale. Requires the
  # edge264-mvc static library to be built as a dependency and exposed as
  # the ${APP_NAME_LC}::Edge264-MVC target. NOT wired into tools/depends
  # or a FindEdge264-MVC.cmake module by this PoC - that packaging work
  # (cross-compiling edge264-mvc for android/aarch64 as a depends
  # package, the way e.g. libbluray is handled) is a separate task.
  find_package(Edge264-MVC)
  if(TARGET ${APP_NAME_LC}::Edge264-MVC)
    list(APPEND SOURCES DVDVideoCodecMVCSoftware.cpp)
    list(APPEND HEADERS DVDVideoCodecMVCSoftware.h)
  else()
    message(STATUS "Edge264-MVC not found - skipping MVC software decoder PoC")
  endif()
endif()

if(("gbm" IN_LIST CORE_PLATFORM_NAME_LC OR "wayland" IN_LIST CORE_PLATFORM_NAME_LC) AND
   TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES DVDVideoCodecDRMPRIME.cpp)
  list(APPEND HEADERS DVDVideoCodecDRMPRIME.h)
endif()

core_add_library(dvdvideocodecs)

if(TARGET ${APP_NAME_LC}::Edge264-MVC)
  target_link_libraries(dvdvideocodecs PRIVATE ${APP_NAME_LC}::Edge264-MVC)
endif()
