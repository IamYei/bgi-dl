#include "../src/browsercookieloader.h"
#include "../src/mnetresolver.h"

#include <QtTest>

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>

namespace {
constexpr auto kPageUrl = "https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303";
constexpr auto kVideoUrl = "https://video.cdn.mnetplus.world/mnetplus/videos/2026/05/11/"
                           "F1A7982634084DC7BA4D15A3730E1B7C/3/converted/"
                           "85B102380A1143A49930C7097082C03D_1080pw.m3u8";
constexpr auto kAudioUrl = "https://video.cdn.mnetplus.world/mnetplus/videos/2026/05/11/"
                           "F1A7982634084DC7BA4D15A3730E1B7C/3/converted/"
                           "85B102380A1143A49930C7097082C03D_audio.cmfa";
#if defined(Q_OS_WIN)
constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/140.0.0.0 Safari/537.36";
#else
constexpr auto kUserAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/140.0.0.0 Safari/537.36";
#endif

struct Response
{
    int status = 0;
    QByteArray body;
    QString error;
};

Response waitForReply(QNetworkReply *reply)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(30000);
    loop.exec();
    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        return {0, {}, QStringLiteral("request timed out")};
    }
    timer.stop();
    const Response response {
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        reply->readAll(),
        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString(),
    };
    reply->deleteLater();
    return response;
}

QString cookieLines(const QList<QNetworkCookie> &cookies)
{
    QByteArrayList lines;
    for (const QNetworkCookie &cookie : cookies) {
        QByteArray line = cookie.toRawForm(QNetworkCookie::Full);
        line.replace('\r', QByteArray{});
        line.replace('\n', QByteArray{});
        lines.append(line);
    }
    return QString::fromUtf8(lines.join('\n'));
}

bool probeInput(const QString &url, const QString &cookies, QString *error)
{
    QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) ffprobe = QStringLiteral("/opt/homebrew/bin/ffprobe");
    QProcess process;
    process.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-protocol_whitelist"), QStringLiteral("http,https,tcp,tls,crypto"),
        QStringLiteral("-user_agent"), QString::fromLatin1(kUserAgent),
        QStringLiteral("-referer"), QString::fromLatin1(kPageUrl),
        QStringLiteral("-cookies"), cookies,
        QStringLiteral("-read_intervals"), QStringLiteral("0%+0.1"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        url,
    });
    if (!process.waitForStarted(3000) || !process.waitForFinished(30000)) {
        process.kill();
        *error = QStringLiteral("ffprobe timed out");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    return !process.readAllStandardOutput().trimmed().isEmpty();
}
}

class LiveSessionTest final : public QObject
{
    Q_OBJECT

private slots:
    void verifiesAuthenticatedMediaAndCaptions();
};

void LiveSessionTest::verifiesAuthenticatedMediaAndCaptions()
{
    if (qEnvironmentVariable("MNETPLUS_LIVE_TEST") != QStringLiteral("1")) {
        QSKIP("Set MNETPLUS_LIVE_TEST=1 to use the local Edge session");
    }

    BrowserCookieLoader loader;
    QSignalSpy loaded(&loader, &BrowserCookieLoader::loaded);
    QSignalSpy unavailable(&loader, &BrowserCookieLoader::unavailable);
    loader.load(QUrl(QString::fromLatin1(kPageUrl)), QStringLiteral("edge"));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() + unavailable.count() == 1, 30000);
    QVERIFY2(unavailable.isEmpty(),
             unavailable.isEmpty() ? "" : qPrintable(unavailable.first().first().toString()));
    const QList<QNetworkCookie> cookies =
        qvariant_cast<QList<QNetworkCookie>>(loaded.first().first());

    MnetResolver resolver;
    resolver.setCookies(cookies);
    QSignalSpy resolved(&resolver, &MnetResolver::resolved);
    QSignalSpy resolveFailed(&resolver, &MnetResolver::failed);
    resolver.resolve(QUrl(QString::fromLatin1(kPageUrl)));
    QTRY_VERIFY_WITH_TIMEOUT(resolved.count() + resolveFailed.count() == 1, 30000);
    QVERIFY2(resolveFailed.isEmpty(), resolveFailed.isEmpty()
        ? "" : qPrintable(resolveFailed.first().first().toString()));
    const MediaInfo media = qvariant_cast<MediaInfo>(resolved.first().first());
    QVERIFY(media.videoUrl.contains(QStringLiteral("/converted/")));
    QVERIFY(!media.captionId.isEmpty());
    if (!media.captionLanguages.isEmpty()) {
        QVERIFY(media.captionLanguages.contains(QStringLiteral("zh_CN")));
    }

    QByteArray accessToken;
    for (const QNetworkCookie &cookie : cookies) {
        if (cookie.name() == QByteArray("_mnet_atk")) accessToken = cookie.value();
    }
    QVERIFY2(!accessToken.isEmpty(), "Edge session does not contain _mnet_atk");

    QNetworkAccessManager network;
    auto *jar = new QNetworkCookieJar(&network);
    network.setCookieJar(jar);
    for (const QNetworkCookie &cookie : cookies) {
        QString host = cookie.domain();
        if (host.startsWith(QLatin1Char('.'))) host.remove(0, 1);
        jar->setCookiesFromUrl({cookie}, QUrl(QStringLiteral("https://%1/").arg(host)));
    }

    QNetworkRequest refreshRequest(QUrl(QStringLiteral(
        "https://api.mnetplus.world/media/v2/public/videos/69fc55cf40f458661f11a303/cookies")));
    refreshRequest.setRawHeader("User-Agent", kUserAgent);
    refreshRequest.setRawHeader("Accept", "application/json");
    refreshRequest.setRawHeader("Origin", "https://www.mnetplus.world");
    refreshRequest.setRawHeader("Referer", kPageUrl);
    refreshRequest.setRawHeader("X-Optional-Authentication", "true");
    refreshRequest.setRawHeader("Authorization", "Bearer " + accessToken);
    const Response refresh = waitForReply(network.post(refreshRequest, QByteArray{}));
    QVERIFY2(refresh.status >= 200 && refresh.status < 300,
             qPrintable(QStringLiteral("cookie refresh: HTTP %1 %2")
                            .arg(refresh.status).arg(refresh.error)));

    const QList<QNetworkCookie> mediaCookies = jar->cookiesForUrl(QUrl(QString::fromLatin1(kVideoUrl)));
    QSet<QByteArray> mediaCookieNames;
    for (const QNetworkCookie &cookie : mediaCookies) mediaCookieNames.insert(cookie.name());
    QVERIFY(mediaCookieNames.contains(QByteArray("CloudFront-Key-Pair-Id")));
    QVERIFY(mediaCookieNames.contains(QByteArray("CloudFront-Policy")));
    QVERIFY(mediaCookieNames.contains(QByteArray("CloudFront-Signature")));

    const QString scopedCookies = cookieLines(mediaCookies);
    QString probeError;
    QVERIFY2(probeInput(QString::fromLatin1(kVideoUrl), scopedCookies, &probeError),
             qPrintable(QStringLiteral("video ffprobe: %1").arg(probeError)));
    QVERIFY2(probeInput(QString::fromLatin1(kAudioUrl), scopedCookies, &probeError),
             qPrintable(QStringLiteral("audio ffprobe: %1").arg(probeError)));

    QNetworkRequest videoRequest(QUrl(QString::fromLatin1(kVideoUrl)));
    videoRequest.setRawHeader("User-Agent", kUserAgent);
    videoRequest.setRawHeader("Referer", kPageUrl);
    const Response video = waitForReply(network.get(videoRequest));
    QVERIFY2(video.status >= 200 && video.status < 300,
             qPrintable(QStringLiteral("video playlist: HTTP %1 %2")
                            .arg(video.status).arg(video.error)));
    qInfo().noquote() << "1080_PLAYLIST_BEGIN\n"
                      << QString::fromUtf8(video.body.left(12000))
                      << "\n1080_PLAYLIST_END";

    QUrl captionUrl(QStringLiteral(
        "https://api.mnetplus.world/media/v1/public/videos/69fc55cf40f458661f11a303/"
        "captions/69fdf4951bbf77224c50aa50/cues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("language"), QStringLiteral("zh_CN"));
    query.addQueryItem(QStringLiteral("displaySecond"), QStringLiteral("21"));
    captionUrl.setQuery(query);
    QNetworkRequest captionRequest(captionUrl);
    captionRequest.setRawHeader("User-Agent", kUserAgent);
    captionRequest.setRawHeader("Accept", "application/json");
    captionRequest.setRawHeader("Origin", "https://www.mnetplus.world");
    captionRequest.setRawHeader("Referer", kPageUrl);
    captionRequest.setRawHeader("Authorization", "Bearer " + accessToken);
    const Response caption = waitForReply(network.get(captionRequest));
    QVERIFY2(caption.status >= 200 && caption.status < 300,
             qPrintable(QStringLiteral("caption: HTTP %1 %2")
                            .arg(caption.status).arg(caption.error)));
    const QJsonDocument captionJson = QJsonDocument::fromJson(caption.body);
    QVERIFY(captionJson.isObject());
    QVERIFY(captionJson.object().contains(QStringLiteral("contentMap")));
}

QTEST_GUILESS_MAIN(LiveSessionTest)
#include "live_session_test.moc"
