# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(NovaStudio VERSION 0.1.0 LANGUAGES CXX)
set(NOVA_DISPLAY_VERSION "${PROJECT_VERSION}-preview.1")
# Product license and copyright notice, used by the executables, the About and Licenses screens,
# the installer and the package README. Keep in step with COPYRIGHT.md.
set(NOVA_LICENSE "GPL-3.0-or-later")
set(NOVA_COPYRIGHT "Copyright (C) 2026 Nova Studio contributors")
# Upstream source repository, shown with the source-code notice. Forks may set their own
# (-DNOVA_SOURCE_URL=...) or clear it. Every release still ships its own source archive.
set(NOVA_SOURCE_URL "https://github.com/kalibsolomon-pixel/Nova-Studio" CACHE STRING
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
configure_file(src/app/build_identity.h.in generated/build_identity.h @ONLY)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Concurrent Network Test CorePrivate GuiPrivate)
enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
if(WIN32)
  include(cmake/NovaPackaging.cmake)
endif()
