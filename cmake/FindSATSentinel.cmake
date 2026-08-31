###############################################################################
# This file is part of the cvc5 project.
#
# Copyright (c) 2009-2026 by the authors listed in the file AUTHORS
# in the top-level source directory and their institutional affiliations.
# All rights reserved.  See the file COPYING in the top-level source
# directory for licensing information.
# #############################################################################
#
# Find SATSentinel
# SATSentinel_FOUND - found SATSentinel lib
# SATSentinel_INCLUDE_DIR - the SATSentinel include directory
# SATSentinel_LIBRARIES - Libraries needed to use SATSentinel
##

include(deps-helper)

# SATSentinel is developed alongside cvc5 and is not released as tarballs, so
# it is pulled directly from its git repository. The repository and the branch
# can be overridden, e.g. to test a feature branch:
#   cmake -DSATSentinel_GIT_REPOSITORY=... -DSATSentinel_GIT_TAG=...
set(SATSentinel_GIT_REPOSITORY "git@github.com:RobCoutel/SATSentinel.git"
    CACHE STRING "Git repository to fetch SATSentinel from")
set(SATSentinel_GIT_TAG "main" CACHE STRING "Git branch/tag of SATSentinel")

find_path(SATSentinel_INCLUDE_DIR NAMES Sentinel-API.hpp PATH_SUFFIXES SATSentinel)
find_library(SATSentinel_LIBRARIES NAMES SATSentinel)

set(SATSentinel_FOUND_SYSTEM FALSE)
if(SATSentinel_INCLUDE_DIR AND SATSentinel_LIBRARIES)
  set(SATSentinel_FOUND_SYSTEM TRUE)
  # SATSentinel does not expose a version number.
  set(SATSentinel_VERSION "")
endif()

if(NOT SATSentinel_FOUND_SYSTEM)
  check_ep_downloaded("SATSentinel-EP")
  if(NOT SATSentinel-EP_DOWNLOADED)
    check_auto_download("SATSentinel" "--no-sat-sentinel")
  endif()

  include(ExternalProject)

  find_package(Git REQUIRED)
  # SATSentinel is built with a plain makefile. Do not rely on
  # CMAKE_MAKE_PROGRAM here, since that is the build tool of the *generator*
  # (e.g. ninja) rather than make.
  find_program(SATSentinel_MAKE NAMES gmake make REQUIRED)

  set(SATSentinel_VERSION "${SATSentinel_GIT_TAG}")

  # The GUI frontend pulls in GLFW/OpenGL, which we do not want to depend on
  # unconditionally. GUI=0 builds the terminal-only sentinel with no additional
  # dependencies; GUI=1 additionally compiles the vendored Dear ImGui sources
  # and the src/gui/ frontend.
  if(USE_SATSENTINEL_GUI)
    set(SATSentinel_GUI 1)
  else()
    set(SATSentinel_GUI 0)
  endif()

  # SATSentinel's makefile defaults to BUILD_MODE=release (-O3 -DNDEBUG, no
  # debug info) regardless of cvc5's own CMAKE_BUILD_TYPE. Forward it explicitly
  # so a --debug cvc5 build gets a libSATSentinel.a with symbols too.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SATSentinel_BUILD_MODE debug)
  else()
    set(SATSentinel_BUILD_MODE release)
  endif()

  # SATSentinel's makefile keys its object files on the source path only, not on
  # the value of GUI or BUILD_MODE, so flipping either flag in an existing
  # checkout would link stale objects compiled with the other setting. Wiping
  # the build directory first is cheap (the sentinel is a handful of
  # translation units) and makes the flags safe to toggle in place.
  ExternalProject_Add(
    SATSentinel-EP
    ${COMMON_EP_CONFIG}
    BUILD_IN_SOURCE ON
    GIT_REPOSITORY ${SATSentinel_GIT_REPOSITORY}
    GIT_TAG ${SATSentinel_GIT_TAG}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${SATSentinel_MAKE} clean
    COMMAND ${SATSentinel_MAKE} lib GUI=${SATSentinel_GUI}
            CC=${CMAKE_CXX_COMPILER} BUILD_MODE=${SATSentinel_BUILD_MODE}
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/build/SATSentinel.a
      <INSTALL_DIR>/lib/libSATSentinel.a
    # The public headers live in include/, but Sentinel-API.hpp also pulls in
    # Sentinel-invariants.hpp, which is kept next to the implementation.
    COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/include
            <INSTALL_DIR>/include/SATSentinel
    COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/Sentinel-invariants.hpp
            <INSTALL_DIR>/include/SATSentinel/Sentinel-invariants.hpp
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libSATSentinel.a
  )

  set(SATSentinel_INCLUDE_DIR "${DEPS_BASE}/include/SATSentinel")
  set(SATSentinel_LIBRARIES "${DEPS_BASE}/lib/libSATSentinel.a")
endif()

set(SATSentinel_FOUND TRUE)

add_library(SATSentinel STATIC IMPORTED GLOBAL)
set_target_properties(
  SATSentinel PROPERTIES IMPORTED_LOCATION "${SATSentinel_LIBRARIES}"
)
set_target_properties(
  SATSentinel PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
  "${SATSentinel_INCLUDE_DIR}"
)

# The GUI objects inside the archive reference GLFW and OpenGL, so whoever links
# the archive has to provide them. This mirrors the LINK_FLAGS the sentinel's own
# makefile uses for its GUI=1 executable.
if(USE_SATSENTINEL_GUI)
  find_package(glfw3 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(Threads REQUIRED)
  set_target_properties(
    SATSentinel PROPERTIES INTERFACE_LINK_LIBRARIES
    "glfw;OpenGL::GL;${CMAKE_DL_LIBS};Threads::Threads"
  )
endif()

mark_as_advanced(SATSentinel_FOUND)
mark_as_advanced(SATSentinel_FOUND_SYSTEM)
mark_as_advanced(SATSentinel_INCLUDE_DIR)
mark_as_advanced(SATSentinel_LIBRARIES)
mark_as_advanced(SATSentinel_MAKE)

if(SATSentinel_FOUND_SYSTEM)
  message(STATUS "Found SATSentinel: ${SATSentinel_LIBRARIES}")
else()
  message(STATUS "Building SATSentinel ${SATSentinel_VERSION}: ${SATSentinel_LIBRARIES}")
  add_dependencies(SATSentinel SATSentinel-EP)
  # Install static library only if it is a static build.
  if(NOT BUILD_SHARED_LIBS)
    install(FILES ${SATSentinel_LIBRARIES} TYPE ${LIB_BUILD_TYPE})
  endif()
endif()
