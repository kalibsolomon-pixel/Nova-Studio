# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 4-H.2: the app-local Microsoft Visual C++ runtime of the release payload, as an explicit
# list instead of a folder glob, so a different Build Tools layout cannot add or drop files
# silently. Used by cmake/NovaPackaging.cmake; also runs in script mode for its own check:
#   cmake -DNOVA_TEST_REDIST=<VCToolsRedistDir> -P cmake/NovaMsvcRuntime.cmake

set(NOVA_MSVC_RUNTIME_NAMES
    concrt140 msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait msvcp140_codecvt_ids
    vccorlib140 vcruntime140 vcruntime140_1 vcruntime140_threads)

# nova_select_msvc_runtime(<VCToolsRedistDir> <out files> <out problems>)
# <out files>: full paths of the runtime DLLs to ship, empty if any problem was found.
# <out problems>: human-readable problems (wrong folder count, missing or unexpected DLLs).
function(nova_select_msvc_runtime redist out_files out_problems)
  set(problems "")
  set(files "")
  file(GLOB crt_dirs LIST_DIRECTORIES true "${redist}/x64/Microsoft.VC*.CRT")
  list(LENGTH crt_dirs count)
  if(NOT count EQUAL 1)
    list(APPEND problems "expected one x64/Microsoft.VC*.CRT folder in '${redist}', found ${count}")
  else()
    foreach(name IN LISTS NOVA_MSVC_RUNTIME_NAMES)
      if(EXISTS "${crt_dirs}/${name}.dll")
        list(APPEND files "${crt_dirs}/${name}.dll")
      else()
        list(APPEND problems "missing ${name}.dll in ${crt_dirs}")
      endif()
    endforeach()
    file(GLOB present RELATIVE "${crt_dirs}" "${crt_dirs}/*.dll")
    foreach(dll IN LISTS present)
      string(TOLOWER "${dll}" lower)
      string(REGEX REPLACE "\\.dll$" "" stem "${lower}")
      if(NOT stem IN_LIST NOVA_MSVC_RUNTIME_NAMES)
        list(APPEND problems "unexpected ${dll} in ${crt_dirs} (not shipped, review the runtime list)")
      endif()
    endforeach()
  endif()
  if(problems)
    set(files "")
  endif()
  set(${out_files} "${files}" PARENT_SCOPE)
  set(${out_problems} "${problems}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED NOVA_TEST_REDIST)
  nova_select_msvc_runtime("${NOVA_TEST_REDIST}" files problems)
  list(LENGTH files count)
  message(STATUS "selected ${count} runtime DLLs")
  foreach(f IN LISTS files)
    message(STATUS "  ${f}")
  endforeach()
  if(problems)
    foreach(p IN LISTS problems)
      message(STATUS "problem: ${p}")
    endforeach()
    message(FATAL_ERROR "MSVC runtime selection failed")
  endif()
endif()
