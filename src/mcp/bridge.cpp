// SPDX-License-Identifier: GPL-3.0-or-later
#include "build_identity.h"
#include "handoff.h"
#include "protocol.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSemaphore>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <qt_windows.h>
#if !defined(_MSC_VER)
#include <pthread.h>
#endif

using namespace nova::mcp;

namespace {
// The Win32 handle of a running std::thread, for CancelSynchronousIo. With MSVC the native handle
// is that HANDLE; with GCC on MinGW-w64 (POSIX thread model) it is a winpthreads pthread_t that
// owns the HANDLE.
HANDLE win32ThreadHandle(std::thread &thread) {
#if defined(_MSC_VER)
    return thread.native_handle();
#else
    return static_cast<HANDLE>(pthread_gethandle(thread.native_handle()));
#endif
}
void print(const QByteArray &text) {
    std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
    std::fflush(stdout);
}
// Local check used by Nova's MCP diagnostics: the bridge starts, its libraries load and its
// protocol implementation answers initialize and tools/list. No connection, no project access.
int selfTest() {
    Protocol protocol;
    auto line = [](const QJsonObject &message) {
        return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    };
    const auto initialized = protocol.receive(line({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", QJsonObject{{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
                               {"clientInfo", QJsonObject{{"name", "nova-self-test"}, {"version", "1"}}}}}}));
    protocol.receive(line({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    const auto listed = protocol.receive(line({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}));
    const auto resources = protocol.receive(line({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "resources/list"}}));
    const auto result = initialized.response["result"].toObject();
    const auto tools = listed.response["result"].toObject()["tools"].toArray();
    const auto listedResources = resources.response["result"].toObject()["resources"].toArray();
    const bool instructions = !result["instructions"].toString().isEmpty();
    print(QJsonDocument(QJsonObject{{"version", nova::AppVersion}, {"revision", nova::BuildRevision},
        {"protocolVersion", result["protocolVersion"]}, {"server", result["serverInfo"]},
        {"tools", tools.size()}, {"resources", listedResources.size()}, {"instructions", instructions}})
              .toJson(QJsonDocument::Compact) + '\n');
    return tools.isEmpty() || listedResources.isEmpty() || !instructions ? 1 : 0;
}
QString offeredCode() {
    QString reason;
    const auto pending = handoff::read(&reason);
    if (!pending) {
        qWarning("Nova Studio has not offered a connection. In Nova Studio, open Resources > MCP Integration "
                 "and choose Allow read-only or Allow editing, then reconnect this server in your AI agent "
                 "within five minutes.");
        return {};
    }
    if (pending->expiresUtcMs && pending->expiresUtcMs < QDateTime::currentMSecsSinceEpoch()) {
        qWarning("The connection offered by Nova Studio has expired. Choose Allow again in "
                 "Resources > MCP Integration, then reconnect this server.");
        return {};
    }
    return pending->code;
}
}

int main(int argc, char **argv) {
    // MCP clients start the bridge without a console and read stderr into their logs. Qt would
    // send messages to the debugger in that case, so write them to stderr directly.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto text = message.toUtf8() + '\n';
        std::fwrite(text.constData(), 1, size_t(text.size()), stderr);
        std::fflush(stderr);
    });
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 2 && arguments[1] == "--version") {
        print(QByteArray("nova-mcp ") + nova::AppVersion + " (" + nova::BuildRevision + ")\n");
        return 0;
    }
    if (arguments.size() == 2 && arguments[1] == "--license") {
        print(QByteArray("nova-mcp ") + nova::AppVersion + "\n" + nova::ProductCopyright + "\n" +
              "License " + nova::ProductLicense + ": GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>.\n"
              "This is free software: you are free to change and redistribute it.\n"
              "There is NO WARRANTY, to the extent permitted by law.\n"
              "The license and third-party notices are in LICENSE.txt and licenses\\ next to this program.\n");
        return 0;
    }
    if (arguments.size() == 2 && arguments[1] == "--self-test")
        return selfTest();
    if (arguments.size() > 1) {
        qWarning("Usage: nova-mcp [--version | --license | --self-test]. MCP clients start it without arguments.");
        return 2;
    }
    // An explicit code from the environment takes precedence over the offered connection.
    auto code = qEnvironmentVariable("NOVA_MCP_CODE");
    if (code.isEmpty())
        code = offeredCode();
    else if (!QRegularExpression("^[a-f0-9]{64}$").match(code).hasMatch())
        qWarning("NOVA_MCP_CODE is not a connection code issued by Nova Studio.");
    const auto match = QRegularExpression("^[a-f0-9]{64}$").match(code);
    if (!match.hasMatch() || code.size() != 64)
        return 2;
    QLocalSocket socket;
    socket.setReadBufferSize(ResponseLimit + 1);
    socket.connectToServer("nova-mcp-" + code.left(32));
    if (!socket.waitForConnected(3000)) {
        qWarning("Nova Studio has no reachable grant. Open Nova and authorize a new connection.");
        return 2;
    }
    socket.write(code.mid(32).toLatin1() + '\n');
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered))
        return 2;
    QObject::connect(&socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
    QObject::connect(&socket, &QLocalSocket::errorOccurred, &app, &QCoreApplication::quit);
    QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&] {
        while (socket.bytesAvailable() > 0) {
            const auto data = socket.read(16384);
            if (data.isEmpty() || output.write(data) != data.size() || !output.flush()) {
                app.quit();
                return;
            }
        }
    });
    std::atomic_bool stop = false, finished = false;
    QSemaphore slotsAvailable(4);
    std::thread input([&] {
        const HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
        QByteArray line;
        char bytes[4096];
        DWORD count = 0;
        while (!stop.load() && ReadFile(stdinHandle, bytes, sizeof(bytes), &count, nullptr) && count) {
            for (DWORD i = 0; i < count && !stop.load(); ++i) {
                line.append(bytes[i]);
                if (bytes[i] != '\n' && line.size() <= RequestLimit)
                    continue;
                while (!stop.load() && !slotsAvailable.tryAcquire(1, 25)) {}
                if (stop.load())
                    break;
                const bool oversized = line.size() > RequestLimit;
                QMetaObject::invokeMethod(&app, [&, frame = std::move(line)] {
                    if (socket.bytesToWrite() + frame.size() > 4 * RequestLimit ||
                        socket.write(frame) != frame.size())
                        app.quit();
                    slotsAvailable.release();
                }, Qt::QueuedConnection);
                line.clear();
                // The editor sends a bounded framing error and disconnects.
                if (oversized) {
                    finished.store(true);
                    return;
                }
            }
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        finished.store(true);
    });
    const auto status = app.exec();
    stop.store(true);
    socket.abort();
    for (int attempt = 0; !finished.load() && attempt < 250; ++attempt) {
        CancelSynchronousIo(win32ThreadHandle(input));
        Sleep(1);
    }
    if (!finished.load()) {
        // A host may supply a stdin driver that cannot cancel its blocking read.
        // The transport is closed and stdout was flushed before leaving the loop.
        std::_Exit(status);
    }
    input.join();
    return status;
}
