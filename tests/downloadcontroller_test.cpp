#include "../src/downloadcontroller.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

class DownloadControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void remuxesVideoAndAudioToMkv();
    void preservesForeignOutputOnCommitFailure();
    void selects1080AndDownloadsHlsSegmentsConcurrently();
    void derives2160FromCodecSuffixedVariant();
    void downloadsParallelHlsAndAudioAs1080Mkv();
    void fallsBackToHighestListedVariantAndDiscoversAudio();
    void expandsFilenameTemplate();
    void normalizesWindowsReservedNames();
    void discoversCaptionsFromVideoIdListEndpoint();
    void exportsAndEmbedsConcurrentCaptions();
};

namespace {
QString createFixture(const QString &ffmpeg, const QString &path)
{
    QProcess generator;
    generator.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("color=c=red:s=64x64:r=10"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=44100"),
        QStringLiteral("-t"), QStringLiteral("1"),
        QStringLiteral("-c:v"), QStringLiteral("mpeg4"),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        path,
    });
    if (!generator.waitForStarted(3000) || !generator.waitForFinished(15000)
        || generator.exitCode() != 0) {
        return QString::fromUtf8(generator.readAllStandardError());
    }
    return {};
}
}

void DownloadControllerTest::remuxesVideoAndAudioToMkv()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg is not available");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("fixture.mp4"));
    const QString fixtureError = createFixture(ffmpeg, sourcePath);
    QVERIFY2(fixtureError.isEmpty(), qPrintable(fixtureError));
    QVERIFY(QFileInfo::exists(sourcePath));

    MediaInfo media;
    media.pageUrl = QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303");
    media.videoId = QStringLiteral("69fc55cf40f458661f11a303");
    media.title = QStringLiteral("fixture output");
    media.videoUrl = QUrl::fromLocalFile(sourcePath).toString(QUrl::FullyEncoded);

    DownloadController controller;
    QSignalSpy completed(&controller, &DownloadController::completed);
    QSignalSpy failed(&controller, &DownloadController::failed);
    controller.start(media, directory.path(), false, false);

    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    const QString outputPath = completed.first().first().toString();
    QVERIFY(QFileInfo::exists(outputPath));
    QVERIFY(QFileInfo(outputPath).size() > 0);
}

void DownloadControllerTest::preservesForeignOutputOnCommitFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("ours.mkv"));
    const QString outputPath = directory.filePath(QStringLiteral("foreign.mkv"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("ours"), qint64(4));
    source.close();
    QFile output(outputPath);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("foreign"), qint64(7));
    output.close();

    DownloadController controller;
    controller.m_outputPath = outputPath;
    controller.m_outputOwned = false;
    QVERIFY(!controller.moveFileToOutput(sourcePath));
    controller.finishWithError(QStringLiteral("expected test failure"));

    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("foreign"));
    QVERIFY(QFileInfo::exists(sourcePath));
}

void DownloadControllerTest::selects1080AndDownloadsHlsSegmentsConcurrently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    int activeSegments = 0;
    int maximumActiveSegments = 0;
    bool requested720 = false;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, &activeSegments, &maximumActiveSegments, &requested720] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);
                QByteArray body;
                QByteArray contentType = "application/octet-stream";
                int delay = 0;
                if (target == "/master.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720\n"
                           "video_720pw.m3u8\n";
                } else if (target == "/video_1080pw.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
                           "#EXTINF:2,\nseg0.ts\n#EXTINF:2,\nseg1.ts\n"
                           "#EXTINF:2,\nseg2.ts\n#EXTINF:2,\nseg3.ts\n"
                           "#EXT-X-ENDLIST\n";
                } else if (target == "/video_720pw.m3u8") {
                    requested720 = true;
                    body = "unexpected";
                } else if (target.startsWith("/seg")) {
                    body = QByteArray(1024, 'v');
                    delay = 80;
                    ++activeSegments;
                    maximumActiveSegments = qMax(maximumActiveSegments, activeSegments);
                } else {
                    body = "not found";
                }
                QTimer::singleShot(delay, socket,
                    [socket, body, contentType, delay, &activeSegments] {
                    const QByteArray status = body == "not found"
                        ? QByteArray("HTTP/1.1 404 Not Found\r\n")
                        : QByteArray("HTTP/1.1 200 OK\r\n");
                    const QByteArray response = status
                        + "Content-Type: " + contentType + "\r\n"
                        + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                        + "Connection: close\r\n\r\n" + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                    if (delay > 0) --activeSegments;
                });
            });
        }
    });

    HlsDownloader downloader;
    QSignalSpy completed(&downloader, &HlsDownloader::completed);
    QSignalSpy failed(&downloader, &HlsDownloader::failed);
    downloader.start(QUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8")
                              .arg(server.serverPort())),
                     directory.filePath(QStringLiteral("video")),
                     QStringLiteral("https://www.mnetplus.world/"));
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(completed.first().at(1).toInt(), 1080);
    QVERIFY(maximumActiveSegments >= 2);
    QVERIFY(!requested720);
    QFile playlist(completed.first().at(0).toString());
    QVERIFY(playlist.open(QIODevice::ReadOnly));
    QCOMPARE(playlist.readAll().count("part_"), 4);
}

void DownloadControllerTest::derives2160FromCodecSuffixedVariant()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);
                QByteArray body;
                QByteArray contentType = "application/octet-stream";
                bool notFound = false;
                if (target == "/master.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720\n"
                           "video_720pw_h264.m3u8\n";
                } else if (target == "/video_2160pw_h264.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
                           "#EXTINF:2,\nseg0.ts\n#EXTINF:2,\nseg1.ts\n#EXT-X-ENDLIST\n";
                } else if (target.startsWith("/seg")) {
                    body = QByteArray(1024, 'v');
                } else {
                    notFound = true;
                }
                const QByteArray status = notFound
                    ? QByteArray("HTTP/1.1 404 Not Found\r\n")
                    : QByteArray("HTTP/1.1 200 OK\r\n");
                const QByteArray response = status
                    + "Content-Type: " + contentType + "\r\n"
                    + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n" + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    HlsDownloader downloader;
    QSignalSpy completed(&downloader, &HlsDownloader::completed);
    QSignalSpy failed(&downloader, &HlsDownloader::failed);
    downloader.start(QUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8")
                              .arg(server.serverPort())),
                     directory.filePath(QStringLiteral("video")),
                     QStringLiteral("https://www.mnetplus.world/"));
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(completed.first().at(1).toInt(), 2160);
}

void DownloadControllerTest::downloadsParallelHlsAndAudioAs1080Mkv()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) QSKIP("ffmpeg/ffprobe is not available");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString hlsDirectory = directory.filePath(QStringLiteral("origin"));
    QVERIFY(QDir().mkpath(hlsDirectory));
    const QString mediaPlaylist = QDir(hlsDirectory).filePath(QStringLiteral("video_1080pw.m3u8"));
    QProcess generator;
    generator.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("color=c=red:s=1920x1080:r=5"),
        QStringLiteral("-t"), QStringLiteral("1"), QStringLiteral("-an"),
        QStringLiteral("-c:v"), QStringLiteral("mpeg2video"), QStringLiteral("-q:v"), QStringLiteral("8"),
        QStringLiteral("-f"), QStringLiteral("hls"), QStringLiteral("-hls_time"), QStringLiteral("0.25"),
        QStringLiteral("-hls_playlist_type"), QStringLiteral("vod"),
        QStringLiteral("-hls_segment_filename"),
        QDir(hlsDirectory).filePath(QStringLiteral("segment_%03d.ts")), mediaPlaylist,
    });
    QVERIFY(generator.waitForStarted(3000));
    QVERIFY2(generator.waitForFinished(20000), qPrintable(generator.errorString()));
    QCOMPARE(generator.exitCode(), 0);
    const QString audioSource = directory.filePath(QStringLiteral("audio-source.mp4"));
    const QString fixtureError = createFixture(ffmpeg, audioSource);
    QVERIFY2(fixtureError.isEmpty(), qPrintable(fixtureError));
    QVERIFY(QFile::copy(audioSource,
                        QDir(hlsDirectory).filePath(QStringLiteral("video_audio.cmfa"))));

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, hlsDirectory] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QString target = QString::fromUtf8(
                    request.split('\n').constFirst().split(' ').value(1));
                QByteArray body;
                QByteArray contentType = "application/octet-stream";
                if (target == QStringLiteral("/master.m3u8")) {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1280x720\n"
                           "video_720pw.m3u8\n";
                } else {
                    const QString fileName = QFileInfo(target).fileName();
                    QFile file(QDir(hlsDirectory).filePath(fileName));
                    const bool exists = file.open(QIODevice::ReadOnly);
                    if (exists) {
                        body = file.readAll();
                        contentType = "application/vnd.apple.mpegurl";
                    }
                    if (!exists) {
                        const QByteArray notFound = QByteArray("HTTP/1.1 404 Not Found\r\n")
                            + "Content-Length: 0\r\nConnection: close\r\n\r\n";
                        socket->write(notFound);
                        socket->disconnectFromHost();
                        return;
                    }
                }
                const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\n")
                    + "Content-Type: " + contentType + "\r\n"
                    + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n" + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    MediaInfo media;
    media.pageUrl = QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303");
    media.videoId = QStringLiteral("69fc55cf40f458661f11a303");
    media.title = QStringLiteral("parallel 1080 output");
    media.videoUrl = QStringLiteral("http://127.0.0.1:%1/master.m3u8").arg(server.serverPort());

    DownloadController controller;
    QSignalSpy completed(&controller, &DownloadController::completed);
    QSignalSpy failed(&controller, &DownloadController::failed);
    controller.start(media, directory.path(), false, false);
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

    QProcess probe;
    probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                          QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                          QStringLiteral("-show_entries"), QStringLiteral("stream=width,height"),
                          QStringLiteral("-of"), QStringLiteral("csv=p=0:s=x"),
                          completed.first().first().toString()});
    QVERIFY(probe.waitForStarted(3000));
    QVERIFY(probe.waitForFinished(10000));
    QVERIFY(QString::fromUtf8(probe.readAllStandardOutput()).trimmed()
                .startsWith(QStringLiteral("1920x1080")));
}

void DownloadControllerTest::fallsBackToHighestListedVariantAndDiscoversAudio()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);
                QByteArray body;
                QByteArray contentType = "application/octet-stream";
                bool notFound = false;
                if (target == "/master.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=1900000,RESOLUTION=1280x720\n"
                           "video_720pw.m3u8\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=600000,RESOLUTION=854x480\n"
                           "video_480pw.m3u8\n"
                           "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"Audio\",DEFAULT=YES,"
                           "AUTOSELECT=YES,URI=\"audio.m3u8\"\n";
                } else if (target == "/video_720pw.m3u8") {
                    contentType = "application/vnd.apple.mpegurl";
                    body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
                           "#EXTINF:2,\nseg0.ts\n#EXTINF:2,\nseg1.ts\n#EXT-X-ENDLIST\n";
                } else if (target == "/video_480pw.m3u8") {
                    notFound = true;
                } else if (target.startsWith("/seg")) {
                    body = QByteArray(512, 'v');
                } else {
                    notFound = true;
                }
                const QByteArray status = notFound
                    ? QByteArray("HTTP/1.1 404 Not Found\r\n")
                    : QByteArray("HTTP/1.1 200 OK\r\n");
                const QByteArray response = status
                    + "Content-Type: " + contentType + "\r\n"
                    + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n" + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    HlsDownloader downloader;
    QSignalSpy audio(&downloader, &HlsDownloader::audioStreamDiscovered);
    QSignalSpy completed(&downloader, &HlsDownloader::completed);
    QSignalSpy failed(&downloader, &HlsDownloader::failed);
    downloader.start(QUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8")
                              .arg(server.serverPort())),
                     directory.filePath(QStringLiteral("video")),
                     QStringLiteral("https://www.mnetplus.world/"));
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(completed.first().at(1).toInt(), 720);
    QVERIFY(audio.count() >= 1);
    QVERIFY(audio.first().first().toUrl().toString().endsWith(QStringLiteral("audio.m3u8")));
}

void DownloadControllerTest::expandsFilenameTemplate()
{
    const QString name = DownloadController::expandFilenameTemplate(
        QStringLiteral("{date}.Mnet Plus.{title}.WEB-DL.{res}.{codec}.-{tag}.{ext}"),
        QStringLiteral("Sample | Title"),
        QStringLiteral("2026-05-01"),
        1080,
        QStringLiteral("h264"),
        QStringLiteral("buguibgib"));
    QCOMPARE(name, QStringLiteral(
        "260501.Mnet Plus.Sample Title.WEB-DL.1080p.H264.-buguibgib.mkv"));
}

void DownloadControllerTest::normalizesWindowsReservedNames()
{
#if defined(Q_OS_WIN)
    QCOMPARE(DownloadController::safeFileName(QStringLiteral("CON")), QStringLiteral("_CON"));
    QCOMPARE(DownloadController::safeFileName(QStringLiteral("LPT1.mkv")), QStringLiteral("_LPT1.mkv"));
#else
    QSKIP("Windows-only filename behavior");
#endif
}

void DownloadControllerTest::discoversCaptionsFromVideoIdListEndpoint()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) QSKIP("ffmpeg/ffprobe is not available");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("fixture.mp4"));
    const QString fixtureError = createFixture(ffmpeg, sourcePath);
    QVERIFY2(fixtureError.isEmpty(), qPrintable(fixtureError));

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);
                QByteArray body;
                if (target.endsWith("/captions")) {
                    body = R"({"captionId":"69fdf4951bbf77224c50aa50",
                               "captions":[{"language":"ja"},{"language":"zh_CN"}]})";
                } else {
                    const QUrl url(QStringLiteral("http://localhost") + QString::fromUtf8(target));
                    const QUrlQuery query(url);
                    const QString language = query.queryItemValue(QStringLiteral("language"));
                    const int second = query.queryItemValue(QStringLiteral("displaySecond")).toInt();
                    body = QStringLiteral(
                        R"({"captionIntervalSecond":20,"contentMap":{"%1":{"displaySecond":%1,"displayDurationSecond":2,"content":"%2 %1"}}})")
                        .arg(second).arg(language).toUtf8();
                }
                const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\n")
                    + "Content-Type: application/json\r\n"
                    + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                    + "Connection: close\r\n\r\n" + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    MediaInfo media;
    media.pageUrl = QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303");
    media.videoId = QStringLiteral("69fc55cf40f458661f11a303");
    media.title = QStringLiteral("list captions");
    media.filenameTemplate = QStringLiteral("{title}");
    media.videoUrl = QUrl::fromLocalFile(sourcePath).toString(QUrl::FullyEncoded);
    media.durationSecond = 41.0;

    DownloadController controller;
    controller.m_captionApiBaseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/media/v1/public/videos")
                                              .arg(server.serverPort()));
    QSignalSpy completed(&controller, &DownloadController::completed);
    QSignalSpy failed(&controller, &DownloadController::failed);
    controller.start(media, directory.path(), true, true);

    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("list captions.ja.srt"))));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("list captions.zh_CN.srt"))));
}

void DownloadControllerTest::exportsAndEmbedsConcurrentCaptions()
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) QSKIP("ffmpeg/ffprobe is not available");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.filePath(QStringLiteral("fixture.mp4"));
    const QString fixtureError = createFixture(ffmpeg, sourcePath);
    QVERIFY2(fixtureError.isEmpty(), qPrintable(fixtureError));

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        QSKIP("Local TCP listeners are unavailable in this environment");
    }
    int activeRequests = 0;
    int maximumActiveRequests = 0;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, &activeRequests, &maximumActiveRequests] {
                QByteArray request = socket->property("requestBuffer").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestBuffer", request);
                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);
                const QUrl url(QStringLiteral("http://localhost") + QString::fromUtf8(target));
                const QUrlQuery query(url);
                const QString language = query.queryItemValue(QStringLiteral("language"));
                const int second = query.queryItemValue(QStringLiteral("displaySecond")).toInt();
                const QByteArray body = QStringLiteral(
                    R"({"captionIntervalSecond":20,"contentMap":{"%1":{"displaySecond":%1,"displayDurationSecond":2,"content":"%2 %1"}}})")
                    .arg(second).arg(language).toUtf8();
                ++activeRequests;
                maximumActiveRequests = qMax(maximumActiveRequests, activeRequests);
                QTimer::singleShot(80, socket, [socket, body, &activeRequests] {
                    const QByteArray response = QByteArray("HTTP/1.1 200 OK\r\n")
                        + "Content-Type: application/json\r\n"
                        + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                        + "Connection: close\r\n\r\n" + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                    --activeRequests;
                });
            });
        }
    });

    MediaInfo media;
    media.pageUrl = QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303");
    media.videoId = QStringLiteral("69fc55cf40f458661f11a303");
    media.title = QStringLiteral("caption fixture");
    media.filenameTemplate = QStringLiteral("{title}");
    media.videoUrl = QUrl::fromLocalFile(sourcePath).toString(QUrl::FullyEncoded);
    media.captionId = QStringLiteral("69fdf4951bbf77224c50aa50");
    media.captionLanguages = {QStringLiteral("ja"), QStringLiteral("zh_CN")};
    media.durationSecond = 41.0;

    DownloadController controller;
    controller.m_captionApiBaseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/media/v1/public/videos")
                                              .arg(server.serverPort()));
    QSignalSpy completed(&controller, &DownloadController::completed);
    QSignalSpy failed(&controller, &DownloadController::failed);
    controller.start(media, directory.path(), true, true);

    QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 30000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QVERIFY(maximumActiveRequests >= 2);
    const QString outputPath = completed.first().first().toString();
    QVERIFY(QFileInfo::exists(outputPath));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("caption fixture.ja.srt"))));
    QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("caption fixture.zh_CN.srt"))));

    QProcess probe;
    probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"),
                          QStringLiteral("-select_streams"), QStringLiteral("s"),
                          QStringLiteral("-show_entries"), QStringLiteral("stream=index"),
                          QStringLiteral("-of"), QStringLiteral("csv=p=0"), outputPath});
    QVERIFY(probe.waitForStarted(3000));
    QVERIFY(probe.waitForFinished(10000));
    QCOMPARE(QString::fromUtf8(probe.readAllStandardOutput()).trimmed().split(QLatin1Char('\n')).size(), 2);
}

QTEST_GUILESS_MAIN(DownloadControllerTest)
#include "downloadcontroller_test.moc"
