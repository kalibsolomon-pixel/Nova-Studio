// SPDX-License-Identifier: GPL-3.0-or-later
#include "project_config.h"
#include "project.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
namespace nova {
namespace {
constexpr qint64 ConfigLimit = 64 * 1024;
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
bool hex(const QString &text, int length) {
    static const QRegularExpression rule("^[0-9a-f]+$");
    return text.size() == length && rule.match(text).hasMatch();
}
// One Windows-safe path component (no traversal, reserved characters or device names).
bool safeComponent(const QString &part) {
    static const QRegularExpression invalid(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    static const QRegularExpression device(
        QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[0-9\x{00b9}\x{00b2}\x{00b3}]|LPT[0-9\x{00b9}\x{00b2}\x{00b3}])(?:\.|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return !part.isEmpty() && part != "." && part != ".." && part.size() <= 255 &&
           !part.endsWith('.') && !part.endsWith(' ') && !invalid.match(part).hasMatch() &&
           !device.match(part).hasMatch();
}
} // namespace
bool reparsePoint(const QString &path) {
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction())
        return true;
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const auto attributes = GetFileAttributesW(reinterpret_cast<const wchar_t *>(native.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return false;
#endif
}
bool validProjectPath(const QString &relative) {
    if (relative.isEmpty() || relative.size() > 2048 || QDir::isAbsolutePath(relative) ||
        relative.contains('\\') || QDir::cleanPath(relative) != relative)
        return false;
    const auto parts = relative.split('/');
    if (parts.size() < 2 || parts.size() > 33)
        return false;
    for (const auto &part : parts)
        if (!safeComponent(part))
            return false;
    return true;
}
bool ensureProjectDirectory(const QString &root, const QString &relative, QString *error) {
    const auto base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty() || !QFileInfo(base).isDir())
        return fail(error, "The project folder is missing or inaccessible");
    if (!validProjectPath(relative))
        return fail(error, "Invalid or unsafe project-relative path: " + relative);
    const auto parts = relative.split('/');
    auto path = base;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        path += '/' + parts[i];
        if (reparsePoint(path))
            return fail(error, "Linked folders (symlinks, junctions, reparse points) are not followed: " +
                                   QDir(base).relativeFilePath(path));
        // Non-recursive: each missing level is created only after its parent was checked.
        if (!QFileInfo::exists(path) && !QDir().mkdir(path) && !QFileInfo(path).isDir())
            return fail(error, "Could not create project folder: " + QDir(base).relativeFilePath(path));
        const QFileInfo info(path);
        if (!info.isDir())
            return fail(error, "A file blocks the folder path: " + QDir(base).relativeFilePath(path));
        if (reparsePoint(path) ||
            !info.canonicalFilePath().startsWith(base + '/', Qt::CaseInsensitive))
            return fail(error, "Folder escapes the project: " + QDir(base).relativeFilePath(path));
    }
    const auto target = base + '/' + relative;
    if (reparsePoint(target) || QFileInfo(target).isDir())
        return fail(error, "The destination is a folder or a link: " + relative);
    return true;
}
bool ensureAssetDirectory(const QString &root, const QString &relative, QString *error) {
    const auto parts = relative.split('/');
    if (parts.size() < 3 || parts.first() != "assets")
        return fail(error, "Asset paths must be assets/<namespace>/...: " + relative);
    for (const auto &part : parts)
        if (part.compare(".nova", Qt::CaseInsensitive) == 0)
            return fail(error, "Asset paths cannot enter editor metadata: " + relative);
    return ensureProjectDirectory(root, relative, error);
}
QJsonObject BaselineRef::json() const {
    return {{"edition", edition},
            {"version", version},
            {"identity", identity},
            {"client_sha1", clientSha1},
            {"asset_index", QJsonObject{{"id", assetIndexId}, {"sha1", assetIndexSha1}}}};
}
std::optional<BaselineRef> BaselineRef::parse(const QJsonValue &value, QString *error) {
    auto reject = [&](const QString &message) {
        fail(error, message);
        return std::optional<BaselineRef>{};
    };
    if (!value.isObject())
        return reject("baseline must be an object");
    const auto o = value.toObject();
    BaselineRef ref;
    ref.edition = o["edition"].toString();
    ref.version = o["version"].toString();
    ref.identity = o["identity"].toString();
    ref.clientSha1 = o["client_sha1"].toString();
    const auto index = o["asset_index"].toObject();
    ref.assetIndexId = index["id"].toString();
    ref.assetIndexSha1 = index["sha1"].toString();
    if (ref.edition != "java")
        return reject("Only the Java edition baseline is supported");
    bool known = false;
    for (const auto &v : gameVersions())
        known = known || v.id == ref.version;
    if (!known)
        return reject("Baseline version is not in the verified version registry: " + ref.version);
    static const QRegularExpression indexId("^[A-Za-z0-9._-]{1,64}$");
    if (!hex(ref.identity, 64) || !hex(ref.clientSha1, 40) || !hex(ref.assetIndexSha1, 40) ||
        !o["asset_index"].isObject() || !indexId.match(ref.assetIndexId).hasMatch())
        return reject("Baseline identity, client SHA-1 or asset index is malformed");
    return ref;
}
bool StyleNotes::empty() const {
    return theme.isEmpty() && audience.isEmpty() && palette.isEmpty() && preserve.isEmpty() && notes.isEmpty();
}
QJsonObject StyleNotes::json() const {
    QJsonObject out;
    for (const auto &[key, value] : {std::pair{"theme", &theme}, {"audience", &audience}, {"palette", &palette},
                                     {"preserve", &preserve}, {"notes", &notes}})
        if (!value->isEmpty())
            out[key] = *value;
    return out;
}
StyleNotes StyleNotes::fromJson(const QJsonValue &value) {
    const auto o = value.toObject();
    auto field = [&](const char *key, int limit) { return o.value(key).toString().trimmed().left(limit); };
    return {field("theme", ShortLimit), field("audience", ShortLimit), field("palette", LongLimit),
            field("preserve", LongLimit), field("notes", LongLimit)};
}
QString ProjectConfig::path(const QString &root) { return root + "/.nova/project.json"; }
ProjectConfig ProjectConfig::load(const QString &root) {
    ProjectConfig config;
    const auto base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty())
        return config;
    const auto file = path(base);
    auto invalid = [&](const QString &why) {
        config.baseline.reset();
        config.diagnostic = ".nova/project.json " + why + "; the vanilla baseline is off and the "
                            "file is left unchanged";
        return config;
    };
    if (reparsePoint(base + "/.nova") || reparsePoint(file))
        return invalid("is linked and was not read");
    const QFileInfo info(file);
    if (!info.exists())
        return config;
    QString error;
    const auto bytes = info.isFile() ? readFileBytes(file, ConfigLimit, &error) : QByteArray();
    config.revision = Revision::read(file, ConfigLimit);
    if (!info.isFile() || !error.isEmpty() || !config.revision.readable)
        return invalid("is unreadable or exceeds 64 KiB");
    config.revision = {true, true, bytes.size(), config.revision.modified,
                       QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return invalid("is not a JSON object");
    config.json = document.object();
    const auto schema = config.json["schema"];
    if (!schema.isDouble() || schema.toDouble() != 1)
        return invalid("has an unknown schema");
    if (!config.json.contains("baseline"))
        return config;
    config.baseline = BaselineRef::parse(config.json["baseline"], &error);
    if (!config.baseline)
        return invalid("has an invalid baseline (" + error + ")");
    return config;
}
bool ProjectConfig::save(const QString &root, QString *error) {
    if (!diagnostic.isEmpty())
        return fail(error, "Project settings were not rewritten: " + diagnostic);
    const auto base = QFileInfo(root).canonicalFilePath();
    if (base.isEmpty())
        return fail(error, "The project folder is missing or inaccessible");
    auto out = json;
    out["schema"] = 1;
    if (baseline) {
        QString issue;
        if (!BaselineRef::parse(baseline->json(), &issue))
            return fail(error, issue);
        // Known fields are replaced; unknown keys in baseline and asset_index survive.
        auto merged = out["baseline"].toObject();
        auto index = merged["asset_index"].toObject();
        const auto known = baseline->json();
        for (auto it = known.begin(); it != known.end(); ++it)
            merged[it.key()] = it.value();
        const auto knownIndex = known["asset_index"].toObject();
        for (auto it = knownIndex.begin(); it != knownIndex.end(); ++it)
            index[it.key()] = it.value();
        merged["asset_index"] = index;
        out["baseline"] = merged;
    } else
        out.remove("baseline");
    if (!ensureProjectDirectory(base, ".nova/project.json", error))
        return false;
    const auto bytes = QJsonDocument(out).toJson(QJsonDocument::Indented);
    if (!atomicWrite(path(base), bytes, revision, error))
        return false;
    json = out;
    revision = {true, true, bytes.size(), 0, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
    return true;
}
} // namespace nova
