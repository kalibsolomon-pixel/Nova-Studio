# SPDX-License-Identifier: GPL-3.0-or-later
# Phase 4-G: package the validated payload folder as-is (cpack -D NOVA_PAYLOAD=<folder>), so the
# installer and the ZIP contain exactly the files that scripts/package-phase4g.py checked.
if(NOVA_PAYLOAD)
  set(CPACK_INSTALL_CMAKE_PROJECTS "")
  set(CPACK_INSTALLED_DIRECTORIES "${NOVA_PAYLOAD};.")
else()
  message(FATAL_ERROR "Package from a validated payload: cpack -D NOVA_PAYLOAD=<folder> (see scripts/package-phase4g.py)")
endif()
# Upgrade tests only (scripts/verify-phase4g-installer.py): an older-numbered installer.
if(NOVA_VERSION_OVERRIDE)
  set(CPACK_PACKAGE_VERSION "${NOVA_VERSION_OVERRIDE}")
  set(CPACK_PACKAGE_FILE_NAME "nova-studio-${NOVA_VERSION_OVERRIDE}-windows-x64")
endif()
