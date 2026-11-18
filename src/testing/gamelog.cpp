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

// This accepts invalid Minecraft names (e.g. a PNG with a space before its
// extension) while only resolving ordinary, contained pack files on disk.
bool packFile(const QString &root, const QString &path) {
    if (root.isEmpty())
        return false;
    QString target = root;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." ||
            part.compare(".nova", Qt::CaseInsensitive) == 0)
            return false;
        target += '/' + part;
        const QFileInfo info(target);
        if (info.isSymLink() || info.isJunction())
            return false;
    }
    const QFileInfo file(target);
    return file.isFile() && file.canonicalFilePath() == target;
}
} // namespace

bool ResourceWarning::isError() const {
    return text.startsWith("ERROR: ") || text.startsWith("FATAL: ") ||
           text.startsWith("Minecraft wrote a crash report");
}

ResourceWarning resolveResourceWarning(const QString &warning, const QString &projectRoot,
                                       const QString &packEntry) {
    ResourceWarning result{warning, {}};
    if (projectRoot.isEmpty())
        return result;
    const auto root = QFileInfo(projectRoot).canonicalFilePath();
    const auto add = [&](const QString &path) {
        if (!result.assets.contains(path) && packFile(root, path))
            result.assets.append(path);
    };

    static const QRegularExpression metadata(R"(\b(file/.+?) pack metadata\b)");
    if (const auto match = metadata.match(warning); match.hasMatch()) {
        if (match.captured(1) == packEntry)
            add("pack.mcmeta");
        return result;
    }
    static const QRegularExpression origin(R"(\(from pack ([^)]+)\))");
    if (const auto match = origin.match(warning); match.hasMatch() &&
                                                  match.captured(1) != packEntry)
        return result;

    // Physical paths take precedence over the shorter logical ID syntax.
    static const QRegularExpression references(
        R"((?<![\w./\\-])(?:(?<relative>(?:[\w.-]+/)*assets/[\w.-]+/[\w./ -]+?\.(?:png\.mcmeta|png|json|mcmeta)\b|pack\.(?:mcmeta|png)\b)|(?<ns>[a-z0-9_.-]+):(?:(?<file>[a-z0-9_.-]+/[\w./ -]+?\.(?:png\.mcmeta|png|json|mcmeta)\b)|(?<id>[a-z0-9_./-]+)(?<variant>#[^\s'"\])]+)?)))");
    static const QRegularExpression kinds(
        R"(\b(item (?:model )?definitions?|block[ -]?states?|models?|textures?|sprites?)\b)",
        QRegularExpression::CaseInsensitiveOption);
    const QStringList folders{"models", "textures", "blockstates", "items"};
    QString kind;
    qsizetype previous = 0;
    for (auto iter = references.globalMatch(warning); iter.hasNext();) {
        const auto ref = iter.next();
        for (auto hints = kinds.globalMatch(warning.mid(previous, ref.capturedStart() - previous));
             hints.hasNext();) {
            const auto hint = hints.next().captured().toLower();
            kind = hint.startsWith("item")   ? "items"
                   : hint.startsWith("block") ? "blockstates"
                   : hint.startsWith("model") ? "models"
                                               : "textures";
        }
        previous = ref.capturedEnd();
        if (const auto relative = ref.captured("relative"); !relative.isEmpty()) {
            add(relative);
            continue;
        }
        const auto prefix = "assets/" + ref.captured("ns") + '/';
        if (const auto file = ref.captured("file"); !file.isEmpty()) {
            add(prefix + file);
            continue;
        }
        auto id = ref.captured("id");
        // Some warnings put sentence punctuation immediately after a bare ID.
        while (id.endsWith('.'))
            id.chop(1);
        if (id.isEmpty())
            continue;
        const auto variant = ref.captured("variant");
        auto folder = kind;
        if (!variant.isEmpty() && folder != "items")
            folder = variant == "#inventory" ? "models" : "blockstates";
        const auto addId = [&](const QString &category) {
            add(prefix + category + '/' + id + (category == "textures" ? ".png" : ".json"));
        };
        if (folder.isEmpty()) {
            for (const auto &category : folders)
                addId(category);
        } else {
            addId(folder);
        }
    }
    return result;
}

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
            "item (?:model )?definition|equipment|pack metadata|invalid path in pack",
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
