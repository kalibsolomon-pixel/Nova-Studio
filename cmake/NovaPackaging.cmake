# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 4-G: the release payload (install tree) and its packages.
#
#   cmake --install <build> --prefix <dir>    the payload, validated by scripts/package-phase4g.py
#   cpack -G NSIS -D NOVA_PAYLOAD=<dir>       the per-user installer, built from that exact payload
#   cpack -G ZIP  -D NOVA_PAYLOAD=<dir>       the portable ZIP, from the same payload
#
# The payload follows the accepted Phase 4-F portable recipe: both executables, the Qt DLLs
# from windeployqt, only the qwindows platform plugin, the app-local MSVC runtime, and the
# license material. Phase 4-G.1 adds Nova's own GPL-3.0-or-later license; the corresponding
# source archive is added to source/ by scripts/package-phase4g.py. Nothing here runs during a
# normal build or test.

set(NOVA_RELEASE_DEPENDENCIES "${PROJECT_SOURCE_DIR}/build/phase3-dependencies" CACHE PATH
    "Qt Base source archive, qt-source.json and qt-notices for the release payload")
set(NOVA_MAKENSIS "${PROJECT_SOURCE_DIR}/.tools/nsis/nsis-3.13/makensis.exe" CACHE FILEPATH
    "makensis.exe used by CPack's NSIS generator")

install(TARGETS nova-studio nova-mcp RUNTIME DESTINATION . COMPONENT runtime)

# Qt: windeployqt on the installed executables with the Phase 4-F options, then the single
# platform plugin and a qt.conf that keeps Qt inside the installation folder.
find_program(NOVA_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin" NO_DEFAULT_PATH)
install(CODE "
  foreach(exe nova-studio.exe nova-mcp.exe)
    execute_process(COMMAND \"${NOVA_WINDEPLOYQT}\" --release --no-plugins --no-translations
                            --no-compiler-runtime --no-opengl-sw \"\${CMAKE_INSTALL_PREFIX}/\${exe}\"
                    RESULT_VARIABLE deployed OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT deployed EQUAL 0)
      message(FATAL_ERROR \"windeployqt failed for \${exe}: \${log}\")
    endif()
  endforeach()" COMPONENT runtime)
install(FILES "${QT6_INSTALL_PREFIX}/plugins/platforms/qwindows.dll" DESTINATION plugins/platforms COMPONENT runtime)
file(WRITE "${PROJECT_BINARY_DIR}/packaging/qt.conf" "[Paths]\nPrefix=.\nPlugins=plugins\nLibraries=.\n")
install(FILES "${PROJECT_BINARY_DIR}/packaging/qt.conf" DESTINATION . COMPONENT runtime)

# MSVC runtime, app-local, only from the installed Visual Studio redistributable folder.
file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" NOVA_VC_REDIST)
file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" NOVA_VS_ROOT)
if(NOT NOVA_VC_REDIST OR NOT NOVA_VS_ROOT)
  message(STATUS "Release payload: configure from a Visual Studio developer environment to include the MSVC runtime")
endif()
file(GLOB NOVA_MSVC_RUNTIME "${NOVA_VC_REDIST}/x64/Microsoft.VC*.CRT/*.dll")
install(FILES ${NOVA_MSVC_RUNTIME} DESTINATION . COMPONENT runtime)

# Nova Studio's own license (GPL-3.0-or-later), then third-party notices and license material.
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt COMPONENT runtime)
install(FILES "${PROJECT_SOURCE_DIR}/COPYRIGHT.md" "${PROJECT_SOURCE_DIR}/packaging/THIRD_PARTY_NOTICES.md"
        DESTINATION licenses COMPONENT runtime)
install(FILES "${NOVA_VS_ROOT}/Licenses/1033/Redist.txt" DESTINATION licenses RENAME MICROSOFT-REDIST.txt
        COMPONENT runtime)
install(FILES "${NOVA_RELEASE_DEPENDENCIES}/qtbase-everywhere-src-6.12.0.tar.xz"
              "${NOVA_RELEASE_DEPENDENCIES}/qt-source.json"
              "${QT6_INSTALL_PREFIX}/sbom/qtbase-6.12.0.spdx.json"
        DESTINATION licenses COMPONENT runtime)
install(DIRECTORY "${NOVA_RELEASE_DEPENDENCIES}/qt-notices/" DESTINATION licenses/qt COMPONENT runtime)
# How The Qt Company configured the shipped Qt build (part of Qt's corresponding source).
install(FILES "${QT6_INSTALL_PREFIX}/config_qtbase.opt" DESTINATION licenses/qt-build COMPONENT runtime)
# The Gradle wrapper is embedded in nova-studio.exe; ship the license from inside the jar.
file(ARCHIVE_EXTRACT INPUT "${PROJECT_SOURCE_DIR}/assets/fabric-runtime/gradle-wrapper-9.8.2.jar"
     DESTINATION "${PROJECT_BINARY_DIR}/packaging/gradle-wrapper" PATTERNS "META-INF/LICENSE")
install(FILES "${PROJECT_BINARY_DIR}/packaging/gradle-wrapper/META-INF/LICENSE" DESTINATION licenses
        RENAME gradle-wrapper-9.8.2-LICENSE.txt COMPONENT runtime)

# User documentation shipped with the preview.
configure_file("${PROJECT_SOURCE_DIR}/packaging/README.txt.in" "${PROJECT_BINARY_DIR}/packaging/README.txt" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/packaging/README.txt" DESTINATION . COMPONENT runtime)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/release/" DESTINATION docs COMPONENT runtime
        FILES_MATCHING PATTERN "*.md")

# ---- CPack
set(CPACK_PACKAGE_NAME "Nova Studio")
set(CPACK_PACKAGE_VENDOR "Nova Studio")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nova Studio resource-pack editor for Minecraft Java Edition")
set(CPACK_PACKAGE_VERSION "${NOVA_DISPLAY_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Nova Studio")
set(CPACK_PACKAGE_FILE_NAME "nova-studio-${NOVA_DISPLAY_VERSION}-windows-x64")
set(CPACK_GENERATOR "NSIS;ZIP")
set(CPACK_NSIS_EXECUTABLE "${NOVA_MAKENSIS}")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Nova Studio")
# zlib, not LZMA: NSIS's LZMA module is CPL-1.0, which would add source-offer duties to the installer.
set(CPACK_NSIS_COMPRESSOR "/SOLID zlib")
set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico")
set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico")
# Installer license page: Nova's notice, then the unmodified GPL text. It is information, not an
# agreement: the GPL does not have to be accepted to install or run the program (section 9).
# One file(CONFIGURE) pass fills in the version and copyright and writes Windows line endings
# for the installer's rich-edit control (configure_file/file(WRITE) translate newlines here).
file(READ "${PROJECT_SOURCE_DIR}/packaging/nsis/license-page.txt.in" NOVA_LICENSE_PAGE)
file(READ "${PROJECT_SOURCE_DIR}/LICENSE" NOVA_GPL_TEXT)
string(REPLACE "\r\n" "\n" NOVA_LICENSE_PAGE "${NOVA_LICENSE_PAGE}${NOVA_GPL_TEXT}")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/packaging/installer-license.txt"
     CONTENT "${NOVA_LICENSE_PAGE}" @ONLY NEWLINE_STYLE CRLF)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_BINARY_DIR}/packaging/installer-license.txt")
# Read by the Nova template (packaging/nsis/NSIS.template.in).
set(CPACK_NOVA_REVISION "${NOVA_GIT_REVISION}${NOVA_SOURCE_DIRTY}")
set(CPACK_NOVA_VERSION_QUAD "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.1")
set(CPACK_NOVA_COPYRIGHT "${NOVA_COPYRIGHT}")
set(CPACK_NOVA_LICENSE "${NOVA_LICENSE}")
# The custom template replaces CPack's machine-wide default: per-user, no elevation, guarded
# upgrade and uninstall. CPack looks for NSIS.template.in on the module path.
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/packaging/nsis")
# Packages are built from a validated payload folder instead of reinstalling (see header).
set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_SOURCE_DIR}/packaging/CPackProjectConfig.cmake")
include(CPack)
