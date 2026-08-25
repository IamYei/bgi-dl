// Integration test: stall a segment mid-body, verify resume+retry succeeds.
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <cstdio>
#include <QTemporaryDir>
#include <QThread>
#include "../src/hlsdownloader.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) { qWarning("no listener"); return 1; }

    const QByteArray segBody(2048, 'v'); // 2KB segment
    int segRequests = 0;
    qint64 lastRangeStart = -1;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                [socket, &segRequests, &lastRangeStart, segBody] {
                QByteArray request = socket->property("buf").toByteArray();
                request += socket->readAll();
                socket->setProperty("buf", request);

                if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                socket->setProperty("handled", true);
                const QByteArray target = request.split('\n').constFirst().split(' ').value(1);

                const int firstRange = request.indexOf("Range: bytes=");
                if (target == "/master.m3u8") {
                    const QByteArray body = "#EXTM3U\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=1920x1080\n"
                           "video_1080pw.m3u8\n";
                    QTimer::singleShot(0, socket, [socket, body] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n"
                                      "Content-Length: " + QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                } else if (target == "/video_1080pw.m3u8") {
                    const QByteArray body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n"
                           "#EXTINF:2,\nseg0.ts\n#EXT-X-ENDLIST\n";
                    QTimer::singleShot(0, socket, [socket, body] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\n"
                                      "Content-Length: " + QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                } else if (target == "/seg0.ts") {
                    ++segRequests;

                    if (firstRange >= 0) {
                        lastRangeStart = QByteArray(request.mid(firstRange + 13)).split('-')
                                             .value(0).trimmed().toLongLong();
                    }

                    if (segRequests == 1) {
                        // First attempt: send only half the body, then drop the
                        // connection (simulates a stalled/timed-out transfer).
                        const QByteArray partial = segBody.left(segBody.size() / 2);

                        QTimer::singleShot(30, [socket, partial, segBody] {
                            const qint64 written = socket->write("HTTP/1.1 200 OK\r\nContent-Length: "
                                          + QByteArray::number(segBody.size())
                                          + "\r\nConnection: close\r\n\r\n" + partial);
                            socket->flush();

                            socket->disconnectFromHost(); // premature close
                        });
                    } else {
                        // Retry: honor the Range resume point.
                        const qint64 from = lastRangeStart >= 0 ? lastRangeStart : 0;
                        const QByteArray rest = segBody.mid(int(from));
                        QTimer::singleShot(20, socket, [socket, rest, from, segBody] {
                            socket->write("HTTP/1.1 206 Partial Content\r\n"
                                          "Content-Range: bytes " + QByteArray::number(from) + "-"
                                          + QByteArray::number(segBody.size() - 1) + "/"
                                          + QByteArray::number(segBody.size()) +
                                          "\r\nContent-Length: " + QByteArray::number(rest.size()) +
                                          "\r\nConnection: close\r\n\r\n" + rest);
                            socket->disconnectFromHost();
                        });
                    }
                } else {
                    QTimer::singleShot(0, socket, [socket] {
                        socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
                        socket->disconnectFromHost();
                    });
                }
            });
        }
    });

    HlsDownloader downloader;
    QSignalSpy completed(&downloader, &HlsDownloader::completed);
    QSignalSpy failed(&downloader, &HlsDownloader::failed);
    downloader.start(QUrl(QStringLiteral("http://127.0.0.1:%1/master.m3u8")
                              .arg(server.serverPort())),
                     dir.filePath(QStringLiteral("video")),
                     QStringLiteral("https://www.mnetplus.world/"));

    // Allow up to 60s (retry backoff included).
    for (int i = 0; i < 600 && completed.isEmpty() && failed.isEmpty(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }

    if (!failed.isEmpty()) {
        qWarning("FAILED: %s", qPrintable(failed.first().first().toString()));
        return 1;
    }
    if (completed.isEmpty()) {
        qWarning("TIMEOUT: neither completed nor failed");
        return 1;
    }
    if (segRequests < 2) {
        qWarning("NO RETRY HAPPENED (requests=%d)", segRequests);
        return 1;
    }
    // Verify the merged stream is the full segment body.
    QFile merged(dir.filePath(QStringLiteral("video/stream.mp4")));
    if (!merged.open(QIODevice::ReadOnly)) { qWarning("no merged stream"); return 1; }
    const QByteArray result = merged.readAll();
    if (result != segBody) {
        qWarning("MERGED MISMATCH: got %lld bytes, want %lld; lastRangeStart=%lld",
                 (long long)result.size(), (long long)segBody.size(), lastRangeStart);
        return 1;
    }
    printf("RESUME TEST PASSED: %d requests, resumed from byte %lld, merged %lld bytes OK\n",
           segRequests, lastRangeStart, (long long)result.size());
    return 0;
}
