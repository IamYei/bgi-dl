#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QObject>
#include <QQueue>
#include <QUrl>

class HlsDownloader final : public QObject
{
    Q_OBJECT

public:
    explicit HlsDownloader(QObject *parent = nullptr);

    void setCookies(const QList<QNetworkCookie> &cookies);
    void setMaxHeight(int height);
    void start(const QUrl &manifestUrl, const QString &outputDirectory,
               const QString &referer);
    // Audio-only mode: no resolution probing/fallbacks, just fetch the media
    // playlist and its segments. Used for parallel _audio.m3u8 downloads so
    // audio uses the same robust segment fetcher as video (ffmpeg's HLS
    // reader is fragile against CloudFront range requests and can truncate).
    void startAudio(const QUrl &manifestUrl, const QString &outputDirectory,
                    const QString &referer);
    void cancel();
    bool isActive() const;

signals:
    void progressChanged(int completed, int total);
    void logMessage(const QString &message);
    void audioStreamDiscovered(const QUrl &url);
    void mediaPlaylistSelected(const QUrl &url, int expectedHeight);
    void completed(const QString &localPlaylistPath, int expectedHeight);
    void failed(const QString &message);

private:
    struct Resource
    {
        QUrl url;
        QString path;
        qint64 rangeStart = -1;
        qint64 rangeLength = -1;
        int retryCount = 0;
        // Bytes already received for this resource across prior attempts.
        // Used to resume with a Range header on retry instead of restarting
        // the segment from byte 0 (which is what made slow links hit the
        // transfer timeout over and over on the same first segment).
        qint64 receivedBytes = 0;
        // Spliced partial bodies accumulated before a successful finish.
        QByteArray partialBuffer;
    };

    struct ManifestCandidate
    {
        QUrl url;
        int height = 0;
        int depth = 0;
    };

    void fetchManifest(const QUrl &url, int depth, int hintedHeight);
    void startCommon(const QUrl &manifestUrl, const QString &outputDirectory,
                     const QString &referer);
    void handleManifestReply();
    bool parseMediaPlaylist(const QByteArray &payload, const QUrl &baseUrl,
                            QString *error);
    QString addResource(const QUrl &url, qint64 rangeStart = -1,
                        qint64 rangeLength = -1);
    void pumpDownloads();
    void scheduleRetry(const Resource &resource, const QString &reason);
    void handleResourceReply(QNetworkReply *reply);
    void finishIfReady();
    void fail(const QString &message);
    QNetworkRequest requestFor(const QUrl &url) const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_manifestReply = nullptr;
    QHash<QNetworkReply *, Resource> m_resourceReplies;
    QQueue<Resource> m_resourceQueue;
    QHash<QString, QString> m_resourcePaths;
    QList<ManifestCandidate> m_manifestFallbacks;
    QStringList m_playlistLines;
    QString m_outputDirectory;
    QString m_playlistPath;
    QString m_referer;
    QUrl m_manifestUrl;
    int m_maxHeight = 2160;
    int m_manifestDepth = 0;
    int m_expectedHeight = 0;
    int m_totalResources = 0;
    int m_completedResources = 0;
    bool m_audioMode = false;
    bool m_active = false;
    bool m_cancelRequested = false;
};
