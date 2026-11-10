// SPDX-License-Identifier: GPL-3.0-or-later
#include "cache.h"
#include "revision.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace nova::vanilla {
namespace {
constexpr qint64 maxManifestBytes = 32LL * 1024 * 1024;

bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
#ifdef Q_OS_WIN
std::wstring native(const QString &path) { return QDir::toNativeSeparators(path).toStdWString(); }
#endif
enum class Kind { Missing, File, Directory, Unsafe };
// One metadata query: kind (reparse points and links are Unsafe) and size.
Kind stat(const QString &path, qint64 *size = nullptr) {
#ifdef Q_OS_WIN
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &data)) {
        const auto code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? Kind::Missing
                                                                             : Kind::Unsafe;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return Kind::Unsafe;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return Kind::Directory;
    if (size)
        *size = (qint64(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return Kind::File;
#else
    const QFileInfo info(path);
    if (info.isSymLink())
        return Kind::Unsafe;
    if (!info.exists())
        return Kind::Missing;
    if (info.isDir())
        return Kind::Directory;
    if (!info.isFile())
        return Kind::Unsafe;
    if (size)
        *size = info.size();
    return Kind::File;
#endif
}
QString token() {
    return QString::number(QRandomGenerator::global()->generate64(), 16).rightJustified(16, '0');
}
QString objectExtension(const QString &relative) {
    return relative.endsWith(".png.mcmeta") ? ".mcmeta" : ".png";
}
const QRegularExpression &folderRule() {
    static const QRegularExpression rule("^[0-9a-f]{32}$");
    return rule;
}
const QRegularExpression &objectRule() {
    static const QRegularExpression rule("^([0-9a-f]{64})\\.(png|mcmeta)$");
    return rule;
}
const QRegularExpression &objectTempRule() {
    static const QRegularExpression rule("^[0-9a-f]{64}\\.[0-9a-f]{16}\\.tmp$");
    return rule;
}
QStringList childNames(const QString &folder, QDir::Filters filters) {
    return QDir(folder).entryList(filters | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                  QDir::Name);
}
// Directory components already proven safe within one operation, so ~11k objects do not
// re-query the same 256 folders.
struct SafeDirs {
    const BaselineCache &cache;
    QSet<QString> known;
    bool dir(const QString &path) {
        if (known.contains(path))
            return true;
        if (!cache.safePath(path))
            return false;
        known.insert(path);
        return true;
    }
    bool object(const QString &path, qint64 *size) {
        return dir(QFileInfo(path).path()) && stat(path, size) == Kind::File;
    }
};
bool readSize(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *out) {
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!(d >= double(minimum)) || d > double(maximum) || std::floor(d) != d)
        return false;
    *out = qint64(d);
    return true;
}
} // namespace

namespace fs {
bool isReparsePoint(const QString &path) { return stat(path) == Kind::Unsafe; }
bool setReadOnly(const QString &path, bool readOnly) {
#ifdef Q_OS_WIN
    const auto name = native(path);
    const DWORD attributes = GetFileAttributesW(name.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    const DWORD wanted = readOnly ? attributes | FILE_ATTRIBUTE_READONLY
                                  : attributes & ~DWORD(FILE_ATTRIBUTE_READONLY);
    return wanted == attributes || SetFileAttributesW(name.c_str(), wanted);
#else
    auto permissions = QFile::permissions(path);
    const auto write = QFileDevice::WriteOwner | QFileDevice::WriteUser;
    return QFile::setPermissions(path, readOnly ? permissions & ~write : permissions | write);
#endif
}
bool removeRegularFile(const QString &path) {
    const auto kind = stat(path);
    if (kind == Kind::Missing)
        return true;
    if (kind != Kind::File || !setReadOnly(path, false))
        return false;
#ifdef Q_OS_WIN
    return DeleteFileW(native(path).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    return QFile::remove(path);
#endif
}
} // namespace fs

QString BaselineEntry::objectPath(const QString &relative) const {
    const auto it = files.find(relative);
    return it == files.end() ? QString() : BaselineCache(root).objectPath(it->sha256, relative);
}

BaselineCache::BaselineCache(QString root)
    : base(QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(root)).absoluteFilePath())) {}

QString BaselineCache::defaultRoot() {
    const auto overridden = qEnvironmentVariable("NOVA_VANILLA_BASELINE_ROOT");
    if (!overridden.isEmpty())
        return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(overridden)).absoluteFilePath());
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
           "/vanilla-baseline";
}

QString BaselineCache::entryFolder(const QString &version, const QString &identity) const {
    return base + "/entries/" + version + '/' + identity.left(32);
}
QString BaselineCache::manifestPath(const QString &version, const QString &identity) const {
    return entryFolder(version, identity) + "/manifest.json";
}
QString BaselineCache::objectPath(const QString &sha256, const QString &relative) const {
    return base + "/objects/" + sha256.left(2) + '/' + sha256 + objectExtension(relative);
}

bool BaselineCache::safePath(const QString &path) const {
    const auto clean = QDir::cleanPath(path);
    if (clean != path || (clean != base && !clean.startsWith(base + '/')))
        return false;
    QString current = base;
    if (stat(current) == Kind::Unsafe)
        return false;
    const auto rest = clean.mid(base.size() + 1);
    if (rest.isEmpty())
        return true;
    for (const auto &part : rest.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || part.contains(':'))
            return false;
        current += '/' + part;
        const auto kind = stat(current);
        if (kind == Kind::Unsafe)
            return false;
        if (kind == Kind::Missing)
            return true; // nothing below can exist yet
    }
    return true;
}

QByteArray BaselineCache::manifestBytes(const BaselineEntry &entry) {
    QJsonObject files;
    for (auto it = entry.files.cbegin(); it != entry.files.cend(); ++it)
        files.insert(it.key(), QJsonObject{{"sha256", it->sha256},
                                           {"size", it->size},
                                           {"origin", it->origin}});
    QJsonObject o{{"schema", 1},
                  {"version", entry.version},
                  {"identity", entry.identity},
                  {"created", entry.created.toUTC().toString(Qt::ISODateWithMs)},
                  {"source", entry.source},
                  {"files", files}};
    o.insert("digest", sha256Hex(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

std::optional<BaselineEntry> BaselineCache::parseManifest(const QByteArray &bytes,
                                                          const QString &root, QString *error) {
    auto bad = [&](const QString &why) {
        fail(error, "The cached baseline manifest is damaged (" + why + ")");
        return std::nullopt;
    };
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return bad("not valid JSON");
    auto o = document.object();
    const auto digest = o.take("digest").toString();
    if (!validSha256(digest) ||
        digest != sha256Hex(QJsonDocument(o).toJson(QJsonDocument::Compact)))
        return bad("digest mismatch");
    if (o.value("schema").toInt(-1) != 1 || !o.value("schema").isDouble())
        return bad("unsupported schema");
    BaselineEntry e;
    e.root = root;
    e.version = o.value("version").toString();
    e.identity = o.value("identity").toString();
    e.created = QDateTime::fromString(o.value("created").toString(), Qt::ISODateWithMs);
    e.source = o.value("source").toObject();
    if (!validVersionId(e.version) || !validSha256(e.identity) || !e.created.isValid())
        return bad("version, identity or date");
    // The identity must follow from the recorded source metadata.
    VersionMetadata m;
    m.id = e.version;
    const auto client = e.source.value("client").toObject();
    const auto index = e.source.value("asset_index").toObject();
    m.client.sha1 = client.value("sha1").toString();
    m.assetIndexId = index.value("id").toString();
    m.assetIndex.sha1 = index.value("sha1").toString();
    if (!validSha1(m.client.sha1) || !validSha1(m.assetIndex.sha1) || m.assetIndexId.isEmpty() ||
        !readSize(client.value("size"), 1, maxClientBytes, &m.client.size) ||
        !readSize(index.value("size"), 1, maxAssetIndexBytes, &m.assetIndex.size) ||
        !validSha1(e.source.value("version_sha1").toString()) ||
        !e.source.value("manifest_url").isString() || !e.source.value("version_url").isString() ||
        !client.value("url").isString() || !index.value("url").isString())
        return bad("source metadata");
    if (identityKey(m) != e.identity)
        return bad("identity does not match its source");
    const auto files = o.value("files").toObject();
    if (files.isEmpty())
        return bad("no files");
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto f = it.value().toObject();
        BaselineFile file;
        file.sha256 = f.value("sha256").toString();
        file.origin = f.value("origin").toString();
        if (!validBaselinePath(it.key()) || !it.value().isObject() || !validSha256(file.sha256) ||
            !readSize(f.value("size"), 1, maxTextureBytes, &file.size) ||
            (file.origin != "jar" && file.origin != "asset-index"))
            return bad("file entry " + it.key().left(160));
        e.files.insert(it.key(), file);
    }
    return e;
}

std::optional<BaselineEntry> BaselineCache::load(const QString &version, const QString &identity,
                                                 Check check, QString *error,
                                                 const std::shared_ptr<std::atomic_bool> &cancel) const {
    if (!validVersionId(version) || !validSha256(identity))
        return fail(error, "Not a valid baseline version or identity"), std::nullopt;
    const auto path = manifestPath(version, identity);
    qint64 size = 0;
    if (!safePath(path))
        return fail(error, "The vanilla baseline cache contains an unexpected link"), std::nullopt;
    const auto kind = stat(path, &size);
    if (kind != Kind::File)
        return fail(error, QString("Vanilla Java %1 (%2) is not in the baseline cache")
                               .arg(version, identity.left(12))),
               std::nullopt;
    if (size > maxManifestBytes)
        return fail(error, "The cached baseline manifest is too large"), std::nullopt;
    const auto bytes = readFileBytes(path, maxManifestBytes, error);
    if (bytes.isEmpty())
        return std::nullopt;
    auto entry = parseManifest(bytes, base, error);
    if (!entry)
        return std::nullopt;
    if (entry->version != version || entry->identity != identity)
        return fail(error, "The cached baseline manifest is damaged (wrong entry)"), std::nullopt;
    if (check == Check::Manifest)
        return entry;
    SafeDirs dirs{*this, {}};
    QSet<QString> seen;
    for (auto it = entry->files.cbegin(); it != entry->files.cend(); ++it) {
        if (cancel && cancel->load())
            return fail(error, "Cancelled"), std::nullopt;
        const auto object = objectPath(it->sha256, it.key());
        if (seen.contains(object))
            continue;
        seen.insert(object);
        qint64 actual = -1;
        if (!dirs.object(object, &actual) || actual != it->size ||
            (check == Check::Full && sha256File(object, cancel) != it->sha256))
            return fail(error, QString("The cached Vanilla Java %1 baseline is damaged: %2 is "
                                       "missing or changed")
                                   .arg(version, it.key())),
                   std::nullopt;
    }
    return entry;
}

std::optional<BaselineEntry> BaselineCache::latest(const QString &version, Check check,
                                                   QString *error) const {
    if (!validVersionId(version))
        return fail(error, "Not a valid baseline version"), std::nullopt;
    const auto folder = base + "/entries/" + version;
    std::optional<BaselineEntry> best;
    QString lastError = QString("Vanilla Java %1 is not in the baseline cache").arg(version);
    if (!safePath(folder) || stat(folder) != Kind::Directory)
        return fail(error, lastError), std::nullopt;
    for (const auto &name : childNames(folder, QDir::Dirs)) {
        if (!folderRule().match(name).hasMatch())
            continue;
        const auto path = folder + '/' + name + "/manifest.json";
        if (!safePath(path))
            continue;
        QString why;
        const auto parsed = parseManifest(readFileBytes(path, maxManifestBytes), base, &why);
        if (!parsed || parsed->identity.left(32) != name) {
            lastError = why;
            continue;
        }
        auto entry = load(version, parsed->identity, check, &why);
        if (!entry)
            lastError = why;
        else if (!best || entry->created > best->created)
            best = std::move(entry);
    }
    if (!best)
        fail(error, lastError);
    return best;
}

bool BaselineCache::readVerified(const BaselineEntry &entry, const QString &relative,
                                 QByteArray *bytes, QString *error) const {
    const auto it = entry.files.find(relative);
    if (it == entry.files.end())
        return fail(error, relative + " is not part of the vanilla baseline");
    const auto path = objectPath(it->sha256, relative);
    qint64 size = -1;
    if (!safePath(path) || stat(path, &size) != Kind::File || size != it->size)
        return fail(error, "The vanilla baseline copy of " + relative + " is missing or changed");
    auto data = readFileBytes(path, it->size, error);
    if (data.size() != it->size || sha256Hex(data) != it->sha256)
        return fail(error, "The vanilla baseline copy of " + relative + " is damaged");
    if (bytes)
        *bytes = std::move(data);
    return true;
}

QStringList BaselineCache::verify(const BaselineEntry &entry,
                                  const std::shared_ptr<std::atomic_bool> &cancel) const {
    QStringList bad;
    QHash<QString, bool> checked; // object path -> good
    SafeDirs dirs{*this, {}};
    for (auto it = entry.files.cbegin(); it != entry.files.cend(); ++it) {
        if (cancel && cancel->load())
            break;
        const auto object = objectPath(it->sha256, it.key());
        auto known = checked.find(object);
        if (known == checked.end()) {
            qint64 size = -1;
            const bool good = dirs.object(object, &size) && size == it->size &&
                              sha256File(object, cancel) == it->sha256;
            known = checked.insert(object, good);
        }
        if (!*known)
            bad.append(it.key());
    }
    return bad;
}

QList<BaselineSummary> BaselineCache::list() const {
    struct Found {
        BaselineSummary summary;
        std::optional<BaselineEntry> entry;
    };
    QList<Found> found;
    const auto entries = base + "/entries";
    if (!safePath(entries) || stat(entries) != Kind::Directory)
        return {};
    for (const auto &version : childNames(entries, QDir::Dirs)) {
        if (!validVersionId(version) || !safePath(entries + '/' + version))
            continue;
        for (const auto &name : childNames(entries + '/' + version, QDir::Dirs)) {
            const auto folder = entries + '/' + version + '/' + name;
            const auto path = folder + "/manifest.json";
            if (!folderRule().match(name).hasMatch() || !safePath(path) || stat(path) == Kind::Missing)
                continue; // not an entry (a failed publish leaves no manifest)
            Found f;
            f.summary.version = version;
            f.summary.identity = name;
            QString why;
            auto parsed = parseManifest(readFileBytes(path, maxManifestBytes, &why), base, &why);
            if (parsed && (parsed->version != version || parsed->identity.left(32) != name)) {
                parsed.reset();
                why = "The cached baseline manifest is damaged (wrong entry)";
            }
            if (parsed) {
                f.summary.identity = parsed->identity;
                f.summary.created = parsed->created;
                f.summary.files = int(parsed->files.size());
                if (load(version, parsed->identity, Check::Sizes, &why))
                    f.summary.status = BaselineSummary::Status::Complete;
                f.entry = std::move(parsed);
            }
            if (f.summary.status != BaselineSummary::Status::Complete)
                f.summary.error = why;
            found.append(std::move(f));
        }
    }
    // Byte counts per object (each object once per entry), unique vs shared across entries.
    QHash<QString, int> references;
    QList<QHash<QString, qint64>> objects;
    for (const auto &f : found) {
        QHash<QString, qint64> own;
        if (f.entry)
            for (auto it = f.entry->files.cbegin(); it != f.entry->files.cend(); ++it)
                own.insert(objectPath(it->sha256, it.key()), it->size);
        for (auto it = own.cbegin(); it != own.cend(); ++it)
            ++references[it.key()];
        objects.append(std::move(own));
    }
    QList<BaselineSummary> result;
    for (qsizetype i = 0; i < found.size(); ++i) {
        auto summary = found[i].summary;
        for (auto it = objects[i].cbegin(); it != objects[i].cend(); ++it) {
            summary.bytes += it.value();
            (references.value(it.key()) > 1 ? summary.sharedBytes : summary.uniqueBytes) +=
                it.value();
        }
        result.append(summary);
    }
    std::sort(result.begin(), result.end(), [](const BaselineSummary &a, const BaselineSummary &b) {
        return a.version != b.version ? a.version < b.version : a.created > b.created;
    });
    return result;
}

bool BaselineCache::remove(const QString &version, const QString &identity, QString *error,
                           RemoveReport *report, int lockTimeoutMs) const {
    RemoveReport local;
    auto &r = report ? *report : local;
    r = {};
    if (!validVersionId(version) ||
        !(validSha256(identity) || folderRule().match(identity).hasMatch()))
        return fail(error, "Not a valid baseline version or identity");
    const auto folder = entryFolder(version, identity);
    if (!safePath(folder) || stat(folder) != Kind::Directory)
        return fail(error, QString("Vanilla Java %1 (%2) is not in the baseline cache")
                               .arg(version, identity.left(12)));
    // Same lock as population. A stale lock is recognised by Qt (dead owner); on Windows a
    // live owner's lock file cannot be deleted, so an old but held lock is never broken.
    QLockFile lock(lockPath());
    if (!lock.tryLock(lockTimeoutMs))
        return fail(error, "Another Nova window is changing the vanilla baseline cache. Try "
                           "again when it has finished.");
    if (!safePath(folder))
        return fail(error, "The vanilla baseline cache contains an unexpected link");
    // 1. The manifest first: from here on the entry no longer exists for readers.
    if (!fs::removeRegularFile(folder + "/manifest.json"))
        return fail(error, "The cached baseline manifest could not be removed");
    for (const auto &name : childNames(folder, QDir::Files))
        fs::removeRegularFile(folder + '/' + name); // leftover temp files of the entry only
    QDir().rmdir(folder);                           // non-recursive: only if now empty
    QDir().rmdir(QFileInfo(folder).path());
    // 2. Objects no remaining valid manifest references. Any unreadable manifest might
    // reference anything, so collection is skipped rather than guessing.
    QSet<QString> referenced;
    const auto entries = base + "/entries";
    for (const auto &v : childNames(entries, QDir::Dirs)) {
        const auto versionFolder = entries + '/' + v;
        if (!safePath(versionFolder)) {
            r.collectionSkipped = true;
            break;
        }
        for (const auto &name : childNames(versionFolder, QDir::Dirs)) {
            const auto path = versionFolder + '/' + name + "/manifest.json";
            if (!safePath(path)) {
                r.collectionSkipped = true;
                continue;
            }
            if (stat(path) == Kind::Missing)
                continue;
            const auto parsed = parseManifest(readFileBytes(path, maxManifestBytes), base);
            if (!parsed) {
                r.collectionSkipped = true;
                continue;
            }
            for (auto it = parsed->files.cbegin(); it != parsed->files.cend(); ++it)
                referenced.insert(it->sha256 + objectExtension(it.key()));
        }
    }
    if (r.collectionSkipped)
        return true;
    const auto objects = base + "/objects";
    if (!safePath(objects) || stat(objects) != Kind::Directory)
        return true;
    static const QRegularExpression shard("^[0-9a-f]{2}$");
    for (const auto &dir : childNames(objects, QDir::Dirs)) {
        const auto shardFolder = objects + '/' + dir;
        if (!shard.match(dir).hasMatch() || !safePath(shardFolder))
            continue;
        for (const auto &name : childNames(shardFolder, QDir::Files | QDir::System)) {
            const auto match = objectRule().match(name);
            const bool object = match.hasMatch() && match.captured(1).startsWith(dir);
            if ((!object || referenced.contains(name)) && !objectTempRule().match(name).hasMatch())
                continue; // unknown names are never touched
            const auto path = shardFolder + '/' + name;
            qint64 size = 0;
            if (stat(path, &size) != Kind::File || !fs::removeRegularFile(path))
                continue;
            if (object) {
                ++r.objectsRemoved;
                r.bytesRemoved += size;
            }
        }
        QDir().rmdir(shardFolder);
    }
    return true;
}

qint64 BaselineCache::totalBytes() const {
    qint64 total = 0;
    QStringList todo{base};
    while (!todo.isEmpty()) {
        const auto folder = todo.takeLast();
        if (stat(folder) != Kind::Directory)
            continue;
        for (const auto &name : childNames(folder, QDir::AllEntries)) {
            const auto path = folder + '/' + name;
            qint64 size = 0;
            const auto kind = stat(path, &size);
            if (kind == Kind::File)
                total += size;
            else if (kind == Kind::Directory && todo.size() < 4096)
                todo.append(path); // reparse points are Unsafe and never followed
        }
    }
    return total;
}

bool BaselineCache::publishObject(const QByteArray &bytes, const QString &sha256,
                                  const QString &relative, QString *error) const {
    const auto path = objectPath(sha256, relative);
    const auto folder = QFileInfo(path).path();
    if (!validSha256(sha256) || !safePath(folder) || !QDir().mkpath(folder) || !safePath(path))
        return fail(error, "The vanilla baseline cache folder could not be prepared");
    auto valid = [&] {
        qint64 size = -1;
        return stat(path, &size) == Kind::File && size == bytes.size() &&
               sha256File(path) == sha256;
    };
    const auto existing = stat(path);
    if (existing == Kind::File && valid())
        return fs::setReadOnly(path, true), true; // dedupe: same content already published
    if (existing != Kind::File && existing != Kind::Missing)
        return fail(error, "The vanilla baseline cache contains an unexpected entry");
    // Temp beside the target, verified, then one rename: create-only, or replacing a bad
    // object (read-only cleared first; the replacement is atomic).
    const auto temp = folder + '/' + sha256 + '.' + token() + ".tmp";
    {
        QFile file(temp);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(bytes) != bytes.size() || !file.flush())
            return file.remove(), fail(error, "A vanilla texture could not be written to the cache");
    }
    qint64 size = -1;
    if (stat(temp, &size) != Kind::File || size != bytes.size() || sha256File(temp) != sha256) {
        fs::removeRegularFile(temp);
        return fail(error, "A vanilla texture was not written to the cache correctly");
    }
    const bool replace = existing == Kind::File;
    if (replace)
        fs::setReadOnly(path, false);
#ifdef Q_OS_WIN
    const bool renamed = MoveFileExW(native(temp).c_str(), native(path).c_str(),
                                     replace ? MOVEFILE_REPLACE_EXISTING : 0);
#else
    const bool renamed = replace ? QFile::remove(path) && QFile::rename(temp, path)
                                 : QFile::rename(temp, path);
#endif
    if (!renamed) {
        fs::removeRegularFile(temp);
        if (valid()) // published concurrently with the same content
            return fs::setReadOnly(path, true), true;
        return fail(error, "A vanilla texture could not be published in the cache");
    }
    if (!valid())
        return fail(error, "A vanilla texture was not stored correctly in the cache");
    fs::setReadOnly(path, true);
    return true;
}

bool BaselineCache::writeManifest(const BaselineEntry &entry, QString *error) const {
    const auto folder = entryFolder(entry.version, entry.identity);
    if (!validVersionId(entry.version) || !validSha256(entry.identity) || !safePath(folder) ||
        !QDir().mkpath(folder) || !safePath(folder + "/manifest.json"))
        return fail(error, "The vanilla baseline entry folder could not be prepared");
    const auto path = folder + "/manifest.json";
    fs::setReadOnly(path, false);
    return atomicWrite(path, manifestBytes(entry), Revision::read(path), error);
}

} // namespace nova::vanilla
