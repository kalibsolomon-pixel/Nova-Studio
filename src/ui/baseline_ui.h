// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Vanilla baseline UI helpers shared by the project dialogs and the main window
// (docs/vanilla-baseline/DESIGN.md, USER_GUIDE.md).
#include "project_config.h"
#include "vanilla/installer.h"
#include <QWidget>
#include <functional>
#include <optional>
namespace nova::ui {
// Tests only: adjusts the installer options before every population (e.g. a loopback mock of
// Mojang's servers). Production never sets it and always uses vanillaPolicy() and the official URLs.
extern std::function<void(vanilla::InstallerOptions &)> baselineInstallerHook;
// The approximate client download shown before Mojang's metadata has been read.
inline constexpr auto baselineDownloadSize = "about 30 MB";
QString baselineConsentText(const QString &version);
QString baselineAttribution();
// The project.json reference for a complete cache entry.
BaselineRef baselineRef(const vanilla::BaselineEntry &entry);
// A complete entry for `version`: the cached one (no network) or, after the user agreed, a fresh
// download with a cancellable progress dialog. `expectedIdentity`/`repair` select Download/Repair.
// Returns nullopt on cancel or failure; *kind and *error explain why (the user has seen a message
// unless *kind is Cancelled or the consent was declined, which reports Cancelled too).
std::optional<vanilla::BaselineEntry> obtainBaseline(QWidget *parent, const QString &version,
                                                     const QString &expectedIdentity, bool repair,
                                                     vanilla::BaselineInstaller::Error *kind,
                                                     QString *error, bool askConsent = true);
// User-facing explanation of an installer failure category.
QString baselineErrorText(vanilla::BaselineInstaller::Error kind, const QString &version,
                          const QString &detail);
// "Java 1.21.11 · 812 files · 4.2 MB · downloaded 2026-11-07" for the newest complete entry, or "".
QString cachedBaselineSummary(const QString &version);
} // namespace nova::ui
