set(SOURCES OSScreenSaverAndroid.cpp
            WinEventsAndroid.cpp
            WinSystemAndroid.cpp
            AndroidUtils.cpp
            VideoSyncAndroid.cpp)

set(HEADERS OSScreenSaverAndroid.h
            WinEventsAndroid.h
            WinSystemAndroid.h
            AndroidUtils.h
            VideoSyncAndroid.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemAndroidGLESContext.cpp)
  list(APPEND HEADERS WinSystemAndroidGLESContext.h)
endif()

core_add_library(windowing_android)

if(TARGET ${APP_NAME_LC}::Edge264-MVC)
  # Pulls in HAS_EDGE264MVC too: FindEdge264-MVC.cmake attaches it as an
  # INTERFACE_COMPILE_DEFINITIONS on this target, so every consumer that
  # links against it - this one included - gets the #define for free via
  # ordinary CMake usage-requirement propagation. No manual mirroring of
  # the define between CMakeLists.txt files needed.
  target_link_libraries(windowing_android PRIVATE ${APP_NAME_LC}::Edge264-MVC)
endif()
