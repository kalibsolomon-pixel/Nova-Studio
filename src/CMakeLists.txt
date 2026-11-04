# SPDX-License-Identifier: GPL-3.0-or-later
add_library(nova-diagnostics STATIC app/diagnostics.cpp app/diagnostics.h)
target_link_libraries(nova-diagnostics PUBLIC Qt6::Core)
target_include_directories(nova-diagnostics PUBLIC app "${PROJECT_BINARY_DIR}/generated")
add_library(nova-core STATIC core/core.cpp core/core.h core/document.cpp core/native.cpp core/revision.cpp core/revision.h
  core/pixelwrite.h core/editing.cpp core/animation.cpp core/animation_native.cpp core/minecraft_animation.cpp core/minecraft_animation.h core/imageops.h core/ops_internal.h core/ops_basics.cpp core/ops_color.cpp
  core/ops_filter.cpp core/ops_transform.cpp core/ops_generate.cpp core/ops_registry.cpp core/palette.h core/palette.cpp)
target_link_libraries(nova-core PUBLIC Qt6::Gui)
target_include_directories(nova-core PUBLIC core)

add_library(nova-asset-index STATIC assets/index.cpp assets/index.h assets/monitor.cpp assets/monitor.h)
target_include_directories(nova-asset-index PUBLIC assets)
target_link_libraries(nova-asset-index PUBLIC Qt6::Core)

add_library(nova-models STATIC models/models.h models/models.cpp models/dispatch.cpp models/mesh.cpp models/resources.h models/resources.cpp models/item_transform.cpp models/generated.cpp models/edit.h models/edit.cpp models/authoring.cpp models/creation.h models/creation.cpp models/display.h models/display.cpp)
target_sources(nova-models PRIVATE models/validation.h models/validation.cpp)
target_include_directories(nova-models PUBLIC models)
target_link_libraries(nova-models PUBLIC nova-core nova-asset-index Qt6::Gui)

add_library(nova-application STATIC app/settings.cpp app/settings.h app/session.cpp app/session.h app/session_edit.cpp app/playback.cpp app/playback.h app/material_playback.cpp app/material_playback.h app/animation_export.cpp app/animation_export.h app/presets.cpp app/presets.h app/batch.cpp app/batch.h app/native_batch.cpp app/native_batch.h app/project.cpp app/project.h app/recovery.cpp app/recovery.h)
target_link_libraries(nova-application PUBLIC nova-core nova-asset-index)
target_include_directories(nova-application PUBLIC app)
target_link_libraries(nova-models PRIVATE nova-application)

# Phase 3-D: hand-off of the current pack to a launcher that signs in and
# starts Minecraft (Nova never handles Microsoft or Minecraft credentials).
add_library(nova-testing STATIC
  testing/discovery.cpp testing/discovery.h
  testing/packsync.cpp testing/packsync.h
  testing/gamelog.cpp testing/gamelog.h
  testing/installer.cpp testing/installer.h
  # Phase 3-D.1: Nova-managed Fabric Loom development runtime.
  testing/fabric_profile.cpp testing/fabric_profile.h
  testing/java_locator.cpp testing/java_locator.h
  testing/fabric_workspace.cpp testing/fabric_workspace.h
  testing/process_tree.cpp testing/process_tree.h
  testing/fabric_runtime.cpp testing/fabric_runtime.h)
target_include_directories(nova-testing PUBLIC testing)
target_link_libraries(nova-testing PUBLIC nova-application Qt6::Network Qt6::Concurrent)
# QZipReader (private, pinned Qt 6.12.0) reads mod jars and the JDK archive.
target_link_libraries(nova-testing PRIVATE Qt6::CorePrivate)
qt_add_resources(nova-testing "nova_fabric_runtime"
  PREFIX "/fabric-runtime"
  BASE "${PROJECT_SOURCE_DIR}/assets/fabric-runtime"
  FILES "${PROJECT_SOURCE_DIR}/assets/fabric-runtime/gradle-wrapper-9.8.2.jar")
if(WIN32)
  target_link_libraries(nova-testing PRIVATE wintrust crypt32)
endif()

add_library(nova-mcp-protocol STATIC mcp/protocol.cpp mcp/protocol.h mcp/handoff.cpp mcp/handoff.h)
target_include_directories(nova-mcp-protocol PUBLIC mcp "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(nova-mcp-protocol PUBLIC Qt6::Core)
if(WIN32)
  target_link_libraries(nova-mcp-protocol PRIVATE advapi32 shell32 ole32)
endif()
add_library(nova-mcp-service STATIC
  mcp/inspection.cpp mcp/inspection.h mcp/scope.cpp mcp/scope.h mcp/service.cpp mcp/service.h)
target_link_libraries(nova-mcp-service PUBLIC nova-mcp-protocol nova-models Qt6::Network Qt6::Concurrent
  PRIVATE nova-application)
# Phase 4-G: MCP client (AI agent) detection and configuration for onboarding.
add_library(nova-mcp-clients STATIC mcp/clients.cpp mcp/clients.h)
target_include_directories(nova-mcp-clients PUBLIC mcp)
target_link_libraries(nova-mcp-clients PUBLIC Qt6::Core PRIVATE nova-core)
add_executable(nova-mcp mcp/bridge.cpp)
target_link_libraries(nova-mcp PRIVATE nova-mcp-protocol Qt6::Network)
if(WIN32)
  enable_language(RC)
  configure_file(mcp/bridge.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bridge.rc" @ONLY)
  target_sources(nova-mcp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bridge.rc")
endif()

add_library(nova-workspace STATIC
  app/branding.cpp app/branding.h
  ui/theme.cpp ui/theme.h ui/workspace.cpp
  assets/assets.cpp assets/assets.h
  ui/modelpanel.cpp ui/modelpanel.h
  ui/uv_editor.cpp
  ui/display_editor.cpp
  ui/modelauthor.h ui/modelauthor.cpp
  ui/resourcetools.h ui/resourcetools.cpp ui/resourceworkspace.cpp
  ui/mcp_access.cpp ui/mcp_setup.cpp ui/mcp_setup.h
  ui/search.cpp ui/search.h
  ui/workflow_review.cpp
  ui/model_materials.cpp
  ui/refinements.cpp
  ui/support.cpp
  ui/legal.cpp ui/legal.h
  rendering/canvas.cpp rendering/canvas.h rendering/onion.cpp rendering/cube.cpp rendering/cube.h
  rendering/preview_state.cpp
  rendering/layer_floating.cpp
  ui/minecraft_testing.cpp ui/minecraft_testing.h
  ui/fabric_testing.cpp ui/fabric_testing.h
  ui/window.cpp ui/window.h ui/animation_ui.cpp ui/timeline.cpp ui/timeline.h ui/layers.cpp ui/layers.h ui/tools.cpp ui/tools.h ui/panels.cpp ui/panels.h ui/opdialog.cpp ui/opdialog.h ui/editor.cpp ui/batchdialog.cpp ui/batchdialog.h ui/safety.cpp ui/indexing.cpp ui/performance.cpp ui/projectdialogs.cpp ui/projectdialogs.h app/metrics.cpp app/metrics.h)
target_include_directories(nova-workspace PUBLIC app assets rendering ui)
target_link_libraries(nova-workspace PUBLIC nova-diagnostics nova-core nova-application nova-asset-index nova-models nova-testing Qt6::Widgets Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Concurrent)
target_link_libraries(nova-workspace PRIVATE nova-mcp-service nova-mcp-clients)
qt_add_resources(nova-workspace "nova_branding"
  PREFIX "/branding"
  BASE "${PROJECT_SOURCE_DIR}/assets/branding"
  FILES
    "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico"
    "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-mark.png")
# Phase 4-G.1: license texts shown by Help > Licenses and Source Code.
qt_add_resources(nova-workspace "nova_legal"
  PREFIX "/legal"
  BASE "${PROJECT_SOURCE_DIR}"
  FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/COPYRIGHT.md")
qt_add_resources(nova-workspace "nova_legal_notices"
  PREFIX "/legal"
  BASE "${PROJECT_SOURCE_DIR}/packaging"
  FILES "${PROJECT_SOURCE_DIR}/packaging/THIRD_PARTY_NOTICES.md")
add_executable(nova-studio WIN32 app/main.cpp)
if(WIN32)
  enable_language(RC)
  configure_file(app/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
  target_sources(nova-studio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
  set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/version.rc" APPEND PROPERTY
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/branding/nova-studio-icon.ico")
endif()
target_link_libraries(nova-studio PRIVATE nova-workspace)
if(MSVC)
  target_link_libraries(nova-workspace PRIVATE Psapi)
  foreach(target nova-core nova-application nova-asset-index nova-testing nova-workspace nova-studio
                 nova-mcp-protocol nova-mcp-service nova-mcp-clients nova-mcp)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  endforeach()
endif()
