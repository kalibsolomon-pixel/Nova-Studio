#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QJsonObject result;
    result["qt"] = qVersion();
    if (application.arguments().contains("--tls")) {
        result["tls_backend"] = QSslSocket::activeBackend();
        QNetworkAccessManager manager;
        QJsonArray rows;
        const QStringList urls = {"https://launchermeta.mojang.com/mc/game/version_manifest_v2.json",
                                  "https://expired.badssl.com/", "https://self-signed.badssl.com/",
                                  "https://wrong.host.badssl.com/"};
        for (const auto &url : urls) {
            QNetworkReply *reply = manager.get(QNetworkRequest(QUrl(url)));
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            timeout.start(30000);
            loop.exec();
            rows.append(QJsonObject{{"url", url}, {"error", int(reply->error())},
                                     {"status", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
                                     {"bytes", reply->bytesAvailable()}});
            reply->deleteLater();
        }
        result["requests"] = rows;
    } else {
        const QRegularExpression valid("^[a-z0-9_.-]+$");
        const QRegularExpression forbidden("[<>:\"\\\\|?*\\x00-\\x1f]");
        const QString subject = "resource_pack_component_123";
        QJsonArray timings;
        qint64 matches = 0;
        for (int trial = 0; trial < 7; ++trial) {
            QElapsedTimer timer;
            timer.start();
            for (int iteration = 0; iteration < 500000; ++iteration) {
                matches += valid.match(subject).hasMatch();
                matches += !forbidden.match(subject).hasMatch();
            }
            timings.append(double(timer.nsecsElapsed()) / 1000000.0);
        }
        QRegularExpression recursive("^(a(?1)?a)$");
        recursive.optimize();
        const auto deep = recursive.match(QString(20000, QLatin1Char('a')));
        result["matching_ns_per_call"] = timings;
        result["matches"] = matches;
        result["deep_match"] = deep.hasMatch();
        result["deep_valid"] = deep.isValid();
        result["deep_capture_size"] = deep.capturedLength();
    }
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, size_t(output.size()), stdout);
    std::fputc('\n', stdout);
    return 0;
}
