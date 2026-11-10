# Copies the Qt and compiler runtime DLLs that a build-tree nova-mcp.exe imports next to it.
# MCP clients start the bridge with their own PATH, which rarely contains Qt or the MinGW runtime,
# so a development build must not depend on the developer's PATH. Run as a POST_BUILD script:
#   cmake -D BRIDGE=<nova-mcp.exe> -D QT_BIN=<Qt bin> -D COMPILER_BIN=<compiler bin, may be empty>
#         -D QT_PREFIX=<Qt install prefix> -D QT_PATHS=<key=relative dir,...> -P ...
# (commas: build tools pass the command through cmd.exe, where | and ; are not inert)
# Only DLLs found in those two folders are copied; Windows DLLs stay out. Packages resolve their own
# payload, notices and qt.conf (cmake/NovaPackaging.cmake, cmake/NovaPackagingMinGW.cmake).
#
# The pinned Qt builds are relocatable: without a qt.conf, Qt derives its prefix from the folder of the
# Qt6Core.dll it loaded. A copy in this folder would make every Qt program here (nova-studio.exe, the
# tests) look for plugins under the build folder. So when Qt DLLs are copied, a qt.conf beside them
# names the Qt installation they came from, with Qt's own install-relative directories, giving the same
# paths as loading Qt6Core.dll from the Qt installation. Build trees only; never installed.
include("${CMAKE_CURRENT_LIST_DIR}/NovaMingwHelpers.cmake")
set(folders "${QT_BIN}")
if(COMPILER_BIN)
  list(APPEND folders "${COMPILER_BIN}")
endif()
nova_folder_file_regexes(included ${folders})
file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${BRIDGE}"
  RESOLVED_DEPENDENCIES_VAR resolved
  UNRESOLVED_DEPENDENCIES_VAR unresolved
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
  POST_INCLUDE_REGEXES ${included}
  POST_EXCLUDE_REGEXES ".*"
  DIRECTORIES ${folders})
get_filename_component(destination "${BRIDGE}" DIRECTORY)
set(copied_qt OFF)
foreach(library IN LISTS resolved)
  get_filename_component(name "${library}" NAME)
  file(COPY_FILE "${library}" "${destination}/${name}" ONLY_IF_DIFFERENT)
  if(name MATCHES "^[Qq]t6")
    set(copied_qt ON)
  endif()
endforeach()
if(copied_qt)
  if(NOT QT_PREFIX OR NOT IS_DIRECTORY "${QT_PREFIX}")
    message(FATAL_ERROR "NovaDeployBridge: QT_PREFIX must name the Qt installation")
  endif()
  if(EXISTS "${destination}/qt6.conf")
    message(FATAL_ERROR "NovaDeployBridge: ${destination}/qt6.conf would override the build-tree qt.conf")
  endif()
  file(TO_CMAKE_PATH "${QT_PREFIX}" prefix)
  string(REPLACE "," ";" QT_PATHS "${QT_PATHS}")
  set(conf "# Written by cmake/NovaDeployBridge.cmake for this build folder only; packages carry their own.\n")
  string(APPEND conf "[Paths]\nPrefix=${prefix}\n")
  foreach(entry IN LISTS QT_PATHS)
    if(NOT entry MATCHES "^[A-Za-z0-9]+=[^=]+$")
      message(FATAL_ERROR "NovaDeployBridge: malformed QT_PATHS entry '${entry}'")
    endif()
    string(APPEND conf "${entry}\n")
  endforeach()
  file(WRITE "${destination}/qt.conf.new" "${conf}")
  file(COPY_FILE "${destination}/qt.conf.new" "${destination}/qt.conf" ONLY_IF_DIFFERENT)
  file(REMOVE "${destination}/qt.conf.new")
endif()
