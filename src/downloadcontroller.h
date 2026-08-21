#pragma once

#include "hlsdownloader.h"
#include "mnetresolver.h"

#include <QHash>
#include <QMultiMap>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QTimer>

class DownloadControllerTest;

struct CaptionCue
{
    double startSecond = 0.0;
    double durationSecond = 0.0;
    QString content;
};

struct CaptionTrack
{
    QString language;
    QString path;
};

class DownloadController final : public QObject
{
    Q_OBJECT

public:
    explicit DownloadController(QObject *parent = nullptr);

    void setCookies(const QList<QNetworkCookie> &cookies);
    void setMaxVideoHeight(int height);
    void start(const MediaInfo &media,
               const QString &outputDirectory,
               bool includeCaptions,
               bool exportCaptions);
    void cancel();
    bool isActive() const;

    static QString defaultFilenameTemplate();
    static QString expandFilenameTemplate(const QString &templateString,
                                          const QString &title,
                                          const QString &isoDate,
                                          int height,
                                          const QString &codec,
                                          const QString &tag);

signals:
    void stageChanged(const QString &stage);
    void progressTextChanged(const QString &text);
    void logMessage(const QString &message);
    void completed(const QString &outputPath);
    void failed(const QString &message);
    void canceled();

private:
    friend class DownloadControllerTest;

    struct CaptionJob
    {
        QString language;
        QString path;
        QMultiMap<qint64, CaptionCue> cues;
        QSet<QString> cueFingerprints;
        int interval = 20;
        bool failed = false;
        bool initialized = false;
    };

    struct CaptionRequest
    {
        QString language;
        int second = 0;
        int retryCount = 0;
    };

    enum class FfmpegPhase
    {
        Idle,
        MediaDownload,
        FinalMux,
    };

    void refreshMediaCookies();
    void handleMediaCookieReply();
    void preflightVideo();
    void handlePreflightReply();
    void beginTransfers();
    void startMediaDownload();
    void startDirectMediaDownload();
    void startParallelHlsDownload();
    void startAudioDownload();
    void handleAudioOutput();
    void handleAudioFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void maybeStartMediaAssembly();
    void startMediaAssembly();
    void beginCaptionDownloads();
    void fetchCaptionList();
    void handleCaptionListReply();
    void startCaptionDownloads();
    void enqueueCaptionRequest(const CaptionRequest &request);
    void enqueueRemainingCaptionWindowsIfReady();
    void pumpCaptionRequests();
    void handleCaptionReply(QNetworkReply *reply);
    bool parseCaptionPayload(CaptionJob &job, const QByteArray &payload, int *interval);
    void discardCaptionLanguage(const QString &language, const QString &message);
    void finishCaptionDownloadsIfReady();
    bool writeSrt(const CaptionJob &job);
    bool exportSrtFiles();
    void maybeFinalize();
    void startFinalMux();
    void commitMediaFile(const QString &sourcePath);
    bool resolveFinalOutputPath();
    QString buildOutputFileName() const;
    void finalizeCompleted();
    bool moveFileToOutput(const QString &sourcePath);
    void handleFfmpegOutput();
    void handleFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus);
    QString validateOutput(const QString &path);
    void finishWithError(const QString &message);
    void cleanup();

    static QString safeFileName(QString title);
    static QString srtTimestamp(double second);
    static QString normalizeCaption(const QString &content);
    QByteArray accessToken() const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_mediaCookieReply = nullptr;
    QNetworkReply *m_preflightReply = nullptr;
    QNetworkReply *m_captionListReply = nullptr;
    QHash<QNetworkReply *, CaptionRequest> m_captionReplies;
    QQueue<CaptionRequest> m_captionQueue;
    QHash<QString, CaptionJob> m_captionJobs;
    HlsDownloader m_hlsDownloader;
    QProcess m_ffmpeg;
    QProcess m_audioProcess;
    MediaInfo m_media;
    QList<CaptionTrack> m_captionTracks;
    QList<QNetworkCookie> m_cookies;
    QTimer m_ffmpegWatchdog;
    QTimer m_audioWatchdog;
    QString m_outputDirectory;
    QString m_outputPath;
    QString m_workDirectory;
    QString m_mediaPath;
    QString m_muxPath;
    QString m_videoPlaylistPath;
    QString m_audioPath;
    QStringList m_exportedSrtPaths;
    QStringList m_captionLanguages;
    QString m_ffmpegTail;
    QString m_audioTail;
    QUrl m_captionApiBaseUrl = QUrl(
        QStringLiteral("https://api.mnetplus.world/media/v1/public/videos"));
    FfmpegPhase m_ffmpegPhase = FfmpegPhase::Idle;
    int m_captionResponsesFinished = 0;
    int m_captionResponsesTotal = 0;
    int m_requiredVideoHeight = 0;
    int m_actualVideoWidth = 0;
    int m_actualVideoHeight = 0;
    QString m_actualVideoCodec;
    int m_preflightRetryCount = 0;
    bool m_includeCaptions = false;
    bool m_exportCaptions = false;
    bool m_mediaFinished = false;
    bool m_videoDownloadFinished = false;
    bool m_audioDownloadFinished = false;
    bool m_mediaAssemblyStarted = false;
    bool m_captionsFinished = false;
    bool m_captionWindowsQueued = false;
    bool m_outputOwned = false;
    bool m_finalizing = false;
    bool m_active = false;
    bool m_cancelRequested = false;
};
