// SPDX-License-Identifier: GPL-3.0-or-later
#include "branding.h"
#include <QResource>

// Explicit initialization keeps the resource object linked into the static
// workspace library, including tools and tests that construct a Window.
static void initializeBranding() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(nova_branding);
        return true;
    }();
    Q_UNUSED(initialized);
}

namespace nova {
QIcon applicationIcon() {
    initializeBranding();
    return QIcon(":/branding/nova-studio-icon.ico");
}

QIcon logoIcon() {
    initializeBranding();
    return QIcon(":/branding/nova-studio-mark.png");
}
}
