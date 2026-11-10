// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"
#include <QCoreApplication>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2 || !nova::diagnostics::start(QString::fromLocal8Bit(argv[1]))) return 2;
#ifdef Q_OS_WIN
    RaiseException(0xE0424E56, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
    return 3;
}
