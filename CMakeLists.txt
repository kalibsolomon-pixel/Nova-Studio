# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(NovaStudio VERSION 0.1.0 LANGUAGES CXX)
set(NOVA_DISPLAY_VERSION "${PROJECT_VERSION}-preview.1")
# Product license and copyright notice, used by the executables, the About and Licenses screens,
# the installer and the package README. Keep in step with COPYRIGHT.md.
set(NOVA_LICENSE "GPL-3.0-or-later")
set(NOVA_COPYRIGHT "Copyright (C) 2026 Nova Studio contributors")
# Upstream source repository, shown with the source-code notice. Empty until a public repository
# holds the source (a private address would send users to a page they cannot open); pass
# -DNOVA_SOURCE_URL=... then. Every release still ships its own source archive.
set(NOVA_SOURCE_URL "" CACHE STRING
  "URL of the Nova Studio source repository, or empty")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE NOVA_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(NOVA_SOURCE_DIRTY "")
if(NOVA_GIT_REVISION)
  execute_process(COMMAND git diff --quiet HEAD -- src CMakeLists.txt
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE NOVA_DIRTY_RESULT)
  if(NOT NOVA_DIRTY_RESULT EQUAL 0)
    set(NOVA_SOURCE_DIRTY "+modified")
  endif()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.nova-source-revision")
  # A corresponding-source archive (scripts/package-phase4g.py) records the revision it was
  # made from. It cannot prove that the files are unmodified, so the build says so.
  file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.nova-source-revision" NOVA_GIT_REVISION
       LIMIT_COUNT 1 REGEX "^[0-9a-f]+$")
  set(NOVA_SOURCE_DIRTY "+source-archive")
endif()
if(NOT NOVA_GIT_REVISION)
  set(NOVA_GIT_REVISION "unknown-source")
endif()
set(NOVA_SOURCE_SNAPSHOT "" CACHE STRING "Verified qualification source tree, without creating a commit")
if(NOVA_SOURCE_SNAPSHOT)
  string(LENGTH "${NOVA_SOURCE_SNAPSHOT}" NOVA_SNAPSHOT_LENGTH)
  if(NOT NOVA_SNAPSHOT_LENGTH EQUAL 40 OR NOT NOVA_SOURCE_SNAPSHOT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "NOVA_SOURCE_SNAPSHOT must be a full Git tree SHA")
  endif()
  set(NOVA_SOURCE_DIRTY "+snapshot.${NOVA_SOURCE_SNAPSHOT}")
endif()
configure_file(src/app/build_identity.h.in generated/build_identity.h @ONLY)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Concurrent Network Test CorePrivate GuiPrivate)
# Phase 4-I (experimental): GCC/MinGW-w64 builds target the Windows 10 API level explicitly, so
# FILE_ID_INFO, SetDefaultDllDirectories etc. never depend on the headers' default.
if(MINGW)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
if(WIN32 AND MINGW)
  # Phase 4-I experimental payload (GCC runtime instead of the Visual C++ runtime).
  include(cmake/NovaPackagingMinGW.cmake)
elseif(WIN32)
  include(cmake/NovaPackaging.cmake)
endif()
