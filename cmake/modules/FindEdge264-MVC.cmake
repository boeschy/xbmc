# FindEdge264MVC
# -------
# Finds the edge264-mvc software H.264/MVC decoder library
# (https://github.com/jens-duttke/edge264-mvc), built directly from its
# own hand-written Makefile via ExternalProject_Add.
#
# PoC status: only wired up for Android/aarch64. find_package(Edge264MVC)
# elsewhere is a no-op that leaves the target undefined, which is exactly
# what cores/VideoPlayer/DVDCodecs/Video/CMakeLists.txt already guards
# against with its `if(TARGET ${APP_NAME_LC}::Edge264MVC)` check.
#
# This will define the following target:
#
#   ${APP_NAME_LC}::Edge264MVC   - The edge264-mvc library

if(NOT CORE_SYSTEM_NAME STREQUAL android)
  return()
endif()

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  include(cmake/scripts/common/ModuleHelpers.cmake)

  set(${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC edge264-mvc)

  SETUP_BUILD_VARS()

  set(patches "${CORE_SOURCE_DIR}/tools/depends/target/${${CMAKE_FIND_PACKAGE_NAME}_MODULE_LC}/01-android-portability.patch")
  generate_patchcommand("${patches}")
  unset(patches)

  set(CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true)
  set(BUILD_COMMAND make OS=android STATIC=yes BUILD_TEST=no PREFIX=${DEPENDS_PATH})
  set(INSTALL_COMMAND make OS=android STATIC=yes BUILD_TEST=no PREFIX=${DEPENDS_PATH} install)
  set(BUILD_IN_SOURCE 1)

  BUILD_DEP_TARGET()

  SETUP_BUILD_TARGET()

  add_dependencies(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ${${${CMAKE_FIND_PACKAGE_NAME}_MODULE}_BUILD_NAME})

  set_target_properties(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} PROPERTIES
                                                                   INTERFACE_COMPILE_DEFINITIONS HAS_EDGE264MVC
                                                                   FOLDER "External Projects")
endif()
