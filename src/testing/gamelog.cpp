// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamelog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

namespace nova::testing {
LogFinding classifyFullLine(const QString &line, const QString &packEntry);
namespace {
constexpr int maxWarnings = 200;
constexpr qint64 maxInitialRead = 4 * 1024 * 1024;
constexpr qint64 maxReadPerPoll = 1024 * 1024;
// Two layouts reach Nova, both verified against real 1.21.11 logs:
//   launcher:              "[12:00:00] [Render thread/WARN]: text"
//   Fabric Loom dev runs:  "[12:00:00] [Render thread/WARN] (Minecraft) text"
struct Parsed {
    QString level, logger, text;
};
Parsed parse(const QString &line) {
    static const QRegularExpression rule(
        "^\\[[^\\]]*\\] \\[[^\\]]*/([A-Z]+)\\](?:: | \\(([^)]*)\\) )(.*)$");
    const auto match = rule.match(line);
    if (!match.hasMatch())
        return {{}, {}, line};
    return {match.captured(1), match.captured(2), match.captured(3)};
}
} // namespace

QString redactLogLine(QString line) {
    static const QRegularExpression jwt(
        "eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]*");
    static const QRegularExpression named("((?:--)?(?:accessToken|access_token|session|refresh_"
                                          "token)[\"']?\\s*[=: ]\\s*[\"']?)[^\\s\"',}]+",
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression blob("[A-Za-z0-9+/_=-]{160,}");
    line.replace(jwt, "[redacted token]");
    line.replace(named, "\\1[redacted]");
    line.replace(blob, "[redacted]");
    return line;
}

LogFinding classifyLogLine(const QString &raw, const QString &packEntry) {
    // Classify the whole line (a 52-mod reload list is ~1500 characters and the
    // test pack comes last); only stored text is shortened.
    auto finding = classifyFullLine(redactLogLine(raw.left(64 * 1024)), packEntry);
    finding.text = finding.text.left(600);
    return finding;
}

LogFinding classifyFullLine(const QString &line, const QString &packEntry) {
    const auto parsed = parse(line);
    const auto &text = parsed.text;
    const auto &severity = parsed.level;
    if (text.startsWith("Setting user: "))
        return {LogEvent::PlayerSet, text.mid(14).trimmed().left(16)};
    if (text.startsWith("Reloading ResourceManager: ")) {
        const auto packs = text.mid(27).split(", ");
        return {packs.contains(packEntry) ? LogEvent::PackActive : LogEvent::ResourceReload, text};
    }
    if (text.startsWith("Created: ") && text.contains("textures/atlas/blocks.png-atlas"))
        return {LogEvent::ResourcesLoaded, text};
    // Remove Reloading Screen reports the end of the reload it hid.
    if (parsed.logger == "rrls" && text == "Overlay is removed!")
        return {LogEvent::ReloadFinished, text};
    if (text == "Stopping!")
        return {LogEvent::Stopped, text};
    if (severity == "WARN" || severity == "ERROR" || severity == "FATAL") {
        if (!packEntry.isEmpty() && text.contains(packEntry))
            return {LogEvent::PackWarning, severity + ": " + text};
        // Verified against real 1.21.11 sessions: player-skin signature checks
        // mention "textures" and mods report their own bundled packs; neither
        // concerns the pack under test.
        static const QRegularExpression unrelated(
            "Property\\[name=textures|signature for textures property|in mod resource-pack",
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression resource(
            "model|texture|blockstate|atlas|mcmeta|sprite|resource ?pack|font|animation|"
            "item definition|equipment",
            QRegularExpression::CaseInsensitiveOption);
        if (resource.match(text).hasMatch() && !unrelated.match(text).hasMatch())
            return {LogEvent::PackWarning, severity + ": " + text};
        // Mods log many unrelated errors; only fatal ones are surfaced.
        if (severity == "FATAL")
            return {LogEvent::Error, severity + ": " + text};
    }
    return {};
}

GameLogMonitor::GameLogMonitor(QObject *parent) : QObject(parent), timer(new QTimer(this)) {
    timer->setInterval(1000);
    timer->setTimerType(Qt::VeryCoarseTimer);
    connect(timer, &QTimer::timeout, this, &GameLogMonitor::poll);
}

bool GameLogMonitor::active() const { return timer->isActive(); }

void GameLogMonitor::reset() {
    offset = 0;
    partial.clear();
    head.clear();
    birth = {};
    current = {};
}

void GameLogMonitor::watch(const QString &gameDirectory, const QString &packEntry) {
    directory = gameDirectory;
    entry = packEntry;
    reset();
    knownCrashes = QDir(directory + "/crash-reports").entryList({"*.txt"}, QDir::Files);
    poll();
    timer->start();
}

void GameLogMonitor::stop() { timer->stop(); }

void GameLogMonitor::consume(const QString &line) {
    const auto finding = classifyLogLine(line, entry);
    switch (finding.event) {
    case LogEvent::PlayerSet:
        current = {};
        current.running = true;
        current.player = finding.text;
        break;
    case LogEvent::ResourceReload:
    case LogEvent::PackActive:
        current.running = true;
        current.packActive = finding.event == LogEvent::PackActive;
        current.resourcesLoaded = false;
        current.reloadFinished = false;
        current.lastReload = finding.text;
        current.warnings.clear(); // warnings belong to the latest reload
        ++current.reloads;
        break;
    case LogEvent::ResourcesLoaded:
        current.resourcesLoaded = true;
        break;
    case LogEvent::ReloadFinished:
        current.reloadFinished = true;
        break;
    case LogEvent::PackWarning:
    case LogEvent::Error:
        if (current.warnings.size() < maxWarnings && !current.warnings.contains(finding.text))
            current.warnings.append(finding.text);
        break;
    case LogEvent::Stopped:
        current.running = false;
        break;
    case LogEvent::None:
        break;
    }
}

void GameLogMonitor::poll() {
    if (directory.isEmpty())
        return;
    const auto path = directory + "/logs/latest.log";
    const QFileInfo info(path);
    bool notify = false;
    if (!info.isFile() || info.isSymLink()) {
        if (offset || current.running) {
            reset();
            notify = true;
        }
    } else {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) { // binary: offsets must be byte-exact
            // A new game session replaces latest.log. Windows may give the new
            // file the old creation time (file-system tunnelling) and it may
            // already be longer than our offset, so the head is compared too.
            if (info.size() < offset || (birth.isValid() && info.birthTime() != birth) ||
                (!head.isEmpty() && file.read(head.size()) != head)) {
                reset();
                notify = true;
            }
            if (info.size() > offset) {
                const bool first = !birth.isValid();
                if (first)
                    birth = info.birthTime();
                if (head.isEmpty()) {
                    file.seek(0);
                    head = file.read(256);
                }
                if (offset == 0 && info.size() > maxInitialRead)
                    offset = info.size() - maxInitialRead;
                file.seek(offset);
                const auto bytes = file.read(first ? maxInitialRead : maxReadPerPoll);
                offset += bytes.size();
                auto lines = (partial + bytes).split('\n');
                partial = lines.takeLast().right(64 * 1024);
                for (const auto &line : lines)
                    consume(QString::fromUtf8(line).trimmed());
                notify = !lines.isEmpty() || notify;
            }
        }
    }
    const auto crashes = QDir(directory + "/crash-reports").entryList({"*.txt"}, QDir::Files);
    for (const auto &crash : crashes)
        if (!knownCrashes.contains(crash)) {
            knownCrashes.append(crash);
            current.crashed = true;
            current.running = false;
            current.warnings.append("Minecraft wrote a crash report: crash-reports/" + crash);
            notify = true;
        }
    if (notify)
        emit changed();
}

} // namespace nova::testing
