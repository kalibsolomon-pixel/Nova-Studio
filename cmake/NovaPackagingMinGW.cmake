# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 4-I (EXPERIMENTAL): release payload and packages for a GCC/MinGW-w64 (MSYS2 UCRT64) build.
# Included instead of cmake/NovaPackaging.cmake only when MINGW is true; the MSVC payload, its
# Visual C++ runtime handling and its installer text are unchanged. Not for publication.
#
#   cmake --install <build> --prefix <dir>    the payload, validated by scripts/check-mingw-payload.py
#   cpack -G NSIS -D NOVA_PAYLOAD=<dir>       the per-user installer, from that exact payload
#   cpack -G ZIP  -D NOVA_PAYLOAD=<dir>       the portable ZIP, from the same payload
#
# Differences from the MSVC payload:
#   * DLLs are collected by CMake's runtime-dependency resolver from the PE import tables of both
#     executables and the qwindows plugin, restricted to the Qt and compiler bin folders. That
#     yields the Qt DLLs plus the GCC runtime (libstdc++-6, libgcc_s_seh-1, libwinpthread-1) and,
#     with a prebuilt MSYS2 Qt, whatever that Qt links. scripts/check-mingw-payload.py decides
#     whether the result is acceptable; this file does not hide extra DLLs.
#   * No Visual C++ runtime, no Microsoft redistribution terms; GCC runtime and winpthreads
#     licences instead.

set(NOVA_RELEASE_DEPENDENCIES "${PROJECT_SOURCE_DIR}/build/phase3-dependencies" CACHE PATH
    "Qt Base source archive, qt-source.json and qt-notices for the release payload")
set(NOVA_MAKENSIS "${PROJECT_SOURCE_DIR}/.tools/nsis/nsis-3.13/makensis.exe" CACHE FILEPATH
    "makensis.exe used by CPack's NSIS generator")
get_filename_component(NOVA_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(NOVA_MINGW_PREFIX "${NOVA_MINGW_BIN}" DIRECTORY)
set(NOVA_MINGW_LICENSES "${NOVA_MINGW_PREFIX}/share/licenses" CACHE PATH
    "share/licenses of the MSYS2 prefix that provides the GCC runtime")

# Binaries and their runtime closure.
install(TARGETS nova-studio nova-mcp RUNTIME_DEPENDENCY_SET nova_runtime
        RUNTIME DESTINATION . COMPONENT runtime)
install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QWindowsIntegrationPlugin RUNTIME_DEPENDENCY_SET nova_runtime
        LIBRARY DESTINATION plugins/platforms RUNTIME DESTINATION plugins/platforms COMPONENT runtime)
# Deviation from the MSVC payload (Phase 4-I finding): Qt 6 loads TLS backends as plugins, and the
# Fabric/Temurin/Modrinth downloads use HTTPS through QNetworkAccessManager. The MSVC payload ships
# no plugins/tls folder; the experimental payload ships Qt's Schannel backend (Windows' own TLS).
if(TARGET Qt6::QSchannelBackendPlugin)
  install(IMPORTED_RUNTIME_ARTIFACTS Qt6::QSchannelBackendPlugin RUNTIME_DEPENDENCY_SET nova_runtime
          LIBRARY DESTINATION plugins/tls RUNTIME DESTINATION plugins/tls COMPONENT runtime)
else()
  message(WARNING "Qt6::QSchannelBackendPlugin not found: the payload would have no TLS backend for HTTPS downloads")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/NovaMingwHelpers.cmake")
nova_folder_file_regexes(nova_bin_regexes "${QT6_INSTALL_PREFIX}/bin" "${NOVA_MINGW_BIN}")
install(RUNTIME_DEPENDENCY_SET nova_runtime
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
        # Only DLLs from the Qt and compiler bin folders are copied; Windows DLLs and anything found
        # elsewhere stay out (and a missing one then fails scripts/check-mingw-payload.py).
        POST_INCLUDE_REGEXES ${nova_bin_regexes}
        POST_EXCLUDE_REGEXES ".*"
        DIRECTORIES "${QT6_INSTALL_PREFIX}/bin" "${NOVA_MINGW_BIN}"
        RUNTIME DESTINATION . COMPONENT runtime)
file(WRITE "${PROJECT_BINARY_DIR}/packaging/qt.conf" "[Paths]\nPrefix=.\nPlugins=plugins\nLibraries=.\n")
install(FILES "${PROJECT_BINARY_DIR}/packaging/qt.conf" DESTINATION . COMPONENT runtime)

# Nova Studio's own licence, then third-party notices and licence material.
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt COMPONENT runtime)
install(FILES "${PROJECT_SOURCE_DIR}/COPYRIGHT.md" "${PROJECT_SOURCE_DIR}/packaging/mingw/THIRD_PARTY_NOTICES.md"
              "${PROJECT_SOURCE_DIR}/assets/fonts/MaterialSymbolsOutlined-LICENSE.txt"
        DESTINATION licenses COMPONENT runtime)
install(FILES "${NOVA_MINGW_LICENSES}/libstdc++/COPYING3" "${NOVA_MINGW_LICENSES}/libstdc++/COPYING.RUNTIME"
        DESTINATION licenses/gcc-runtime COMPONENT runtime)
install(FILES "${NOVA_MINGW_LICENSES}/libwinpthread/COPYING" DESTINATION licenses/winpthreads COMPONENT runtime)
# Parts of the MinGW-w64 runtime (startup code, libmingwex) are linked statically into every
# GCC-built executable and DLL; their licences (ZPL-2.1 and others) require these notices in
# binary distributions. MinGW-w64 provides this file for exactly that purpose.
install(FILES "${NOVA_MINGW_LICENSES}/crt/COPYING.MinGW-w64-runtime.txt" DESTINATION licenses/mingw-w64-runtime
        COMPONENT runtime)
install(FILES "${NOVA_RELEASE_DEPENDENCIES}/qtbase-everywhere-src-6.12.0.tar.xz"
              "${NOVA_RELEASE_DEPENDENCIES}/qt-source.json"
        DESTINATION licenses COMPONENT runtime)
install(FILES "${QT6_INSTALL_PREFIX}/sbom/qtbase-6.12.0.spdx.json" DESTINATION licenses OPTIONAL COMPONENT runtime)
# A Qt built without the SBOM tools' Python packages writes only the SPDX 2.3 tag-value document;
# it covers the complete Qt build, including the shipped stripped files and build-only tools.
install(FILES "${QT6_INSTALL_PREFIX}/sbom/qtbase-6.12.0.spdx" DESTINATION licenses OPTIONAL COMPONENT runtime)
install(DIRECTORY "${NOVA_RELEASE_DEPENDENCIES}/qt-notices/" DESTINATION licenses/qt COMPONENT runtime)
# How the shipped Qt was configured and with which compiler (written by scripts/build-qtbase-mingw.ps1).
install(FILES "${QT6_INSTALL_PREFIX}/nova-qtbase-build.json" "${QT6_INSTALL_PREFIX}/config_qtbase.summary"
        DESTINATION licenses/qt-build OPTIONAL COMPONENT runtime)
install(FILES "${QT6_INSTALL_PREFIX}/qtbase-6.12.0-pcre2-jit-stack.patch"
        DESTINATION licenses/qt-build COMPONENT runtime)
file(ARCHIVE_EXTRACT INPUT "${PROJECT_SOURCE_DIR}/assets/fabric-runtime/gradle-wrapper-9.8.2.jar"
     DESTINATION "${PROJECT_BINARY_DIR}/packaging/gradle-wrapper" PATTERNS "META-INF/LICENSE")
install(FILES "${PROJECT_BINARY_DIR}/packaging/gradle-wrapper/META-INF/LICENSE" DESTINATION licenses
        RENAME gradle-wrapper-9.8.2-LICENSE.txt COMPONENT runtime)

configure_file("${PROJECT_SOURCE_DIR}/packaging/mingw/README.txt.in" "${PROJECT_BINARY_DIR}/packaging/README.txt" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/packaging/README.txt" DESTINATION . COMPONENT runtime)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/release/" DESTINATION docs COMPONENT runtime
        FILES_MATCHING PATTERN "*.md" PATTERN "BUILDING.md" EXCLUDE
        PATTERN "RELEASE_NOTES.md" EXCLUDE PATTERN "KNOWN_LIMITATIONS.md" EXCLUDE)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/packaging/mingw/docs/" DESTINATION docs COMPONENT runtime
        FILES_MATCHING PATTERN "*.md")

# ---- CPack (same product identity and installer behaviour as the MSVC packages)
set(CPACK_PACKAGE_NAME "Nova Studio")
set(CPACK_PACKAGE_VENDOR "Nova Studio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nova Studio resource-pack editor for Minecraft Java Edition")
set(CPACK_PACKAGE_VERSION "${NOVA_DISPLAY_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Nova Studio")
# A distinct file name, so an experimental package can never be mistaken for an MSVC candidate.
set(CPACK_PACKAGE_FILE_NAME "nova-studio-${NOVA_DISPLAY_VERSION}-windows-x64-mingw-experimental")
set(CPACK_GENERATOR "NSIS;ZIP")
set(CPACK_NSIS_EXECUTABLE "${NOVA_MAKENSIS}")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Nova Studio")
set(CPACK_NSIS_COMPRESSOR "/SOLID zlib")
set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico")
set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico")
# Installer licence page: Nova's notice (GCC runtime paragraph), then the unmodified GPL text.
file(READ "${PROJECT_SOURCE_DIR}/packaging/mingw/license-page.txt.in" NOVA_LICENSE_PAGE)
file(READ "${PROJECT_SOURCE_DIR}/LICENSE" NOVA_GPL_TEXT)
string(REPLACE "\r\n" "\n" NOVA_LICENSE_PAGE "${NOVA_LICENSE_PAGE}${NOVA_GPL_TEXT}")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/packaging/installer-license.txt"
     CONTENT "${NOVA_LICENSE_PAGE}" @ONLY NEWLINE_STYLE CRLF)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/packaging/installer-license.txt")
set(CPACK_NOVA_REVISION "${NOVA_GIT_REVISION}${NOVA_SOURCE_DIRTY}")
set(CPACK_NOVA_VERSION_QUAD "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.1")
set(CPACK_NOVA_COPYRIGHT "${NOVA_COPYRIGHT}")
set(CPACK_NOVA_LICENSE "${NOVA_LICENSE}")
# The production NSIS template, with its one Visual C++ runtime sentence replaced. The template
# file itself is not modified; the derived copy lives in the build tree.
file(READ "${PROJECT_SOURCE_DIR}/packaging/nsis/NSIS.template.in" nova_nsis_template)
set(nova_msvc_sentence "You do not have to accept the GPL to install Nova Studio. The Microsoft Visual C++ runtime terms at the end of the text apply to the Microsoft files: by choosing Next and installing, you agree to them.")
string(FIND "${nova_nsis_template}" "${nova_msvc_sentence}" nova_sentence_at)
if(nova_sentence_at EQUAL -1)
  message(FATAL_ERROR "packaging/nsis/NSIS.template.in changed: update cmake/NovaPackagingMinGW.cmake")
endif()
string(REPLACE "${nova_msvc_sentence}" "You do not have to accept the GPL to install Nova Studio."
       nova_nsis_template "${nova_nsis_template}")
file(WRITE "${PROJECT_BINARY_DIR}/packaging/nsis-mingw/NSIS.template.in" "${nova_nsis_template}")
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_BINARY_DIR}/packaging/nsis-mingw")
set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_SOURCE_DIR}/packaging/CPackProjectConfig.cmake")
include(CPack)
