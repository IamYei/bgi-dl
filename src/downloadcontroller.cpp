#include "downloadcontroller.h"
#include "localization.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextStream>
#include <QUrlQuery>
#include <QUuid>

#include <utility>
#include <functional>

namespace {
#if defined(Q_OS_WIN)
constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/140.0.0.0 Safari/537.36";
#else
constexpr auto kUserAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                            "AppleWebKit/537.36 (KHTML, like Gecko) "
                            "Chrome/140.0.0.0 Safari/537.36";
#endif
constexpr int kMaxCaptionConcurrency = 12;
constexpr int kMaxCaptionWindowsPerLanguage = 2200;
constexpr auto kDefaultFilenameTemplate = "{date}.Mnet Plus.{title}.WEB-DL.{res}.{codec}.-{tag}.{ext}";
constexpr auto kDefaultReleaseTag = "buguibgib";

QString yyMMddFromIsoDate(const QString &isoDate)
{
    QString digits = isoDate;
    digits.remove(QLatin1Char('-'));
    if (digits.size() >= 8) return digits.mid(2, 6);
    return QDate::currentDate().toString(QStringLiteral("yyMMdd"));
}

QString codecLabel(const QString &codecName)
{
    const QString lower = codecName.toLower();
    if (lower == QStringLiteral("h264") || lower == QStringLiteral("avc")) return QStringLiteral("H264");
    if (lower == QStringLiteral("hevc") || lower == QStringLiteral("h265")) return QStringLiteral("HEVC");
    if (lower == QStringLiteral("av1")) return QStringLiteral("AV1");
    if (lower == QStringLiteral("mpeg4") || lower == QStringLiteral("mpeg2video")) return QStringLiteral("H264");
    return codecName.toUpper();
}

QString ffmpegCookieLines(const QList<QNetworkCookie> &cookies)
{
    QList<QByteArray> lines;
    for (const QNetworkCookie &cookie : cookies) {
        QString domain = cookie.domain().toLower();
        if (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
        if (domain != QStringLiteral("mnetplus.world")
            && !domain.endsWith(QStringLiteral(".mnetplus.world"))) {
            continue;
        }
        QByteArray line = cookie.toRawForm(QNetworkCookie::Full);
        line.replace('\r', QByteArray{});
        line.replace('\n', QByteArray{});
        lines.append(line);
    }
    return QString::fromUtf8(QByteArrayList(lines).join('\n'));
}

void appendNetworkInputOptions(QStringList &arguments,
                               const QString &url,
                               const QString &cookieLines,
                               const QString &referer)
{
    const QUrl mediaUrl(url);
    if (mediaUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0
        || mediaUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
        arguments << QStringLiteral("-protocol_whitelist")
                  << QStringLiteral("http,https,tcp,tls,crypto")
                  << QStringLiteral("-user_agent") << QString::fromLatin1(kUserAgent)
                  << QStringLiteral("-referer") << referer;
        if (!cookieLines.isEmpty()) arguments << QStringLiteral("-cookies") << cookieLines;
        return;
    }
    if (mediaUrl.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
        arguments << QStringLiteral("-protocol_whitelist") << QStringLiteral("file,crypto");
    }
}

bool isMnetNetworkUrl(const QString &url)
{
    const QUrl parsed(url);
    const QString host = parsed.host().toLower();
    return parsed.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && (host == QStringLiteral("mnetplus.world")
            || host.endsWith(QStringLiteral(".mnetplus.world")));
}

int mediaHeightFromUrl(const QString &url)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(_(\d{3,4})pw)"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(url).captured(1).toInt();
}

QString siblingAudioUrl(const QUrl &videoUrl)
{
    QUrl audioUrl(videoUrl);
    QString path = audioUrl.path();
    static const QRegularExpression expression(
        QStringLiteral(R"(_\d{3,4}pw.*$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (!path.contains(expression)) return {};
    path.replace(expression, QStringLiteral("_audio.cmfa"));
    audioUrl.setPath(path);
    return audioUrl.toString(QUrl::FullyEncoded);
}

double clockDuration(const QString &value)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^(\d+):(\d{2}):(\d{2}(?:\.\d+)?)$)"));
    const auto match = expression.match(value);
    if (!match.hasMatch()) return 0.0;
    return match.captured(1).toDouble() * 3600.0
        + match.captured(2).toDouble() * 60.0
        + match.captured(3).toDouble();
}

QString matroskaLanguage(QString language)
{
    language = language.left(2).toLower();
    const QMap<QString, QString> codes = {
        {QStringLiteral("ja"), QStringLiteral("jpn")},
        {QStringLiteral("en"), QStringLiteral("eng")},
        {QStringLiteral("ko"), QStringLiteral("kor")},
        {QStringLiteral("zh"), QStringLiteral("chi")},
    };
    return codes.value(language, language);
}

QString normalizedCaptionLanguage(QString language)
{
    language.replace(QLatin1Char('-'), QLatin1Char('_'));
    if (language.compare(QStringLiteral("zh_cn"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("zh_CN");
    }
    if (language.compare(QStringLiteral("zh_tw"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("zh_TW");
    }
    return language.toLower();
}

QString captionTrackTitle(const QString &language)
{
    const QMap<QString, QString> titles = {
        {QStringLiteral("ja"), QStringLiteral("日本語")},
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("ko"), QStringLiteral("한국어")},
        {QStringLiteral("zh_CN"), QStringLiteral("简体中文")},
        {QStringLiteral("zh_TW"), QStringLiteral("繁體中文")},
    };
    return titles.value(language, language);
}

QString findMediaTool(const QString &name)
{
    QString executable = QStandardPaths::findExecutable(name);
    if (!executable.isEmpty()) return executable;
    QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(name),
        QStringLiteral("/opt/homebrew/bin/%1").arg(name),
        QStringLiteral("/usr/local/bin/%1").arg(name),
    };
#if defined(Q_OS_WIN)
    const QString executableName = name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)
        ? name : name + QStringLiteral(".exe");
    candidates.prepend(QDir(QCoreApplication::applicationDirPath()).filePath(executableName));
#endif
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return candidate;
    }
    return {};
}
}

DownloadController::DownloadController(QObject *parent)
    : QObject(parent), m_hlsDownloader(this)
{
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));
    m_ffmpeg.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_ffmpeg, &QProcess::readyReadStandardError,
            this, &DownloadController::handleFfmpegOutput);
    connect(&m_ffmpeg, &QProcess::finished,
            this, &DownloadController::handleFfmpegFinished);
    m_audioProcess.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_audioProcess, &QProcess::readyReadStandardError,
            this, &DownloadController::handleAudioOutput);
    connect(&m_audioProcess, &QProcess::finished,
            this, &DownloadController::handleAudioFinished);
    connect(&m_hlsDownloader, &HlsDownloader::logMessage,
            this, &DownloadController::logMessage);
    connect(&m_hlsDownloader, &HlsDownloader::progressChanged, this,
            [this](int completed, int total) {
        emit progressTextChanged(MNET_TEXT("视频分片 %1/%2 · 音频同时下载")
                                     .arg(completed).arg(total));
    });
    connect(&m_hlsDownloader, &HlsDownloader::audioStreamDiscovered, this,
            [this](const QUrl &audioUrl) {
        if (!m_active || m_cancelRequested) return;
        if (!m_media.audioUrl.isEmpty()) return;
        m_media.audioUrl = audioUrl.toString(QUrl::FullyEncoded);
        m_media.audioDerived = true;
        m_audioDownloadFinished = false;
        emit logMessage(MNET_TEXT("已从 HLS master 解析到独立音轨：%1")
                            .arg(m_media.audioUrl));
        startAudioDownload();
    });
    connect(&m_hlsDownloader, &HlsDownloader::mediaPlaylistSelected, this,
            [this](const QUrl &playlistUrl, int expectedHeight) {
        if (!m_active || m_cancelRequested) return;
        m_requiredVideoHeight = qMax(m_requiredVideoHeight, expectedHeight);
        if (!m_media.audioUrl.isEmpty()) return;
        const QString audioUrl = siblingAudioUrl(playlistUrl);
        if (audioUrl.isEmpty()) return;
        m_media.audioUrl = audioUrl;
        m_media.audioDerived = true;
        m_audioDownloadFinished = false;
        emit logMessage(MNET_TEXT("已从 variant 匹配独立音频流：%1")
                            .arg(audioUrl));
        startAudioDownload();
    });
    connect(&m_hlsDownloader, &HlsDownloader::completed, this,
            [this](const QString &playlistPath, int expectedHeight) {
        if (!m_active || m_cancelRequested) return;
        m_videoPlaylistPath = playlistPath;
        m_requiredVideoHeight = qMax(m_requiredVideoHeight, expectedHeight);
        m_videoDownloadFinished = true;
        emit logMessage(MNET_TEXT("视频分片并发下载完成"));
        maybeStartMediaAssembly();
    });
    connect(&m_hlsDownloader, &HlsDownloader::failed, this,
            [this](const QString &message) {
        if (m_active && !m_cancelRequested) {
            finishWithError(MNET_TEXT("视频下载失败：%1").arg(message));
        }
    });
    m_ffmpegWatchdog.setSingleShot(true);
    m_ffmpegWatchdog.setInterval(90000);
    connect(&m_ffmpegWatchdog, &QTimer::timeout, this, [this] {
        if (m_ffmpeg.state() == QProcess::NotRunning) return;
        m_ffmpegTail += QStringLiteral("\n媒体服务器或本地合并进程 90 秒内没有响应");
        m_ffmpeg.kill();
    });
    m_audioWatchdog.setSingleShot(true);
    m_audioWatchdog.setInterval(90000);
    connect(&m_audioWatchdog, &QTimer::timeout, this, [this] {
        if (m_audioProcess.state() == QProcess::NotRunning) return;
        m_audioTail += QStringLiteral("\n音频服务器 90 秒内没有响应");
        m_audioProcess.kill();
    });
}

void DownloadController::setCookies(const QList<QNetworkCookie> &cookies)
{
    m_cookies = cookies;
    auto *jar = new QNetworkCookieJar(&m_network);
    m_network.setCookieJar(jar);
    for (const QNetworkCookie &cookie : cookies) {
        QString host = cookie.domain();
        if (host.startsWith(QLatin1Char('.'))) host.remove(0, 1);
        jar->setCookiesFromUrl({cookie}, QUrl(QStringLiteral("https://%1/").arg(host)));
    }
}

void DownloadController::setMaxVideoHeight(int height)
{
    m_hlsDownloader.setMaxHeight(height);
}

QString DownloadController::defaultFilenameTemplate()
{
    return QString::fromLatin1(kDefaultFilenameTemplate);
}

QString DownloadController::expandFilenameTemplate(const QString &templateString,
                                                   const QString &title,
                                                   const QString &isoDate,
                                                   int height,
                                                   const QString &codec,
                                                   const QString &tag)
{
    QString output = templateString;
    if (output.isEmpty()) output = QString::fromLatin1(kDefaultFilenameTemplate);
    output.replace(QStringLiteral("{date}"), yyMMddFromIsoDate(isoDate));
    output.replace(QStringLiteral("{title}"), safeFileName(title));
    output.replace(QStringLiteral("{res}"), QStringLiteral("%1p").arg(height));
    output.replace(QStringLiteral("{resolution}"), QString::number(height));
    output.replace(QStringLiteral("{codec}"), codecLabel(codec));
    output.replace(QStringLiteral("{tag}"),
                   tag.isEmpty() ? QString::fromLatin1(kDefaultReleaseTag) : tag);
    output.replace(QStringLiteral("{ext}"), QStringLiteral("mkv"));
    return safeFileName(output);
}

void DownloadController::start(const MediaInfo &media,
                               const QString &outputDirectory,
                               bool includeCaptions,
                               bool exportCaptions)
{
    if (m_active) return;
    if (media.videoUrl.isEmpty()) {
        emit failed(MNET_TEXT("没有可下载的视频流"));
        return;
    }

    m_media = media;
    m_outputDirectory = outputDirectory;
    m_includeCaptions = includeCaptions;
    m_exportCaptions = exportCaptions;
    m_captionLanguages.clear();
    if (includeCaptions || exportCaptions) {
        QSet<QString> seen;
        for (const QString &language : media.captionLanguages) {
            const QString normalized = normalizedCaptionLanguage(language);
            if (!normalized.isEmpty() && !seen.contains(normalized)) {
                seen.insert(normalized);
                m_captionLanguages.append(normalized);
            }
        }
    }

    m_captionReplies.clear();
    m_captionQueue.clear();
    m_captionJobs.clear();
    m_captionTracks.clear();
    m_exportedSrtPaths.clear();
    m_captionResponsesFinished = 0;
    m_captionResponsesTotal = 0;
    m_captionWindowsQueued = false;
    m_mediaFinished = false;
    m_videoDownloadFinished = false;
    m_audioDownloadFinished = false;
    m_mediaAssemblyStarted = false;
    m_captionsFinished = false;
    m_finalizing = false;
    m_outputOwned = false;
    m_requiredVideoHeight = mediaHeightFromUrl(media.videoUrl);
    m_actualVideoWidth = 0;
    m_actualVideoHeight = 0;
    m_actualVideoCodec.clear();
    m_preflightRetryCount = 0;
    m_cancelRequested = false;
    m_ffmpegTail.clear();
    m_audioTail.clear();
    m_ffmpegPhase = FfmpegPhase::Idle;

    if (!QDir().mkpath(m_outputDirectory)) {
        emit failed(MNET_TEXT("无法创建输出目录：%1").arg(m_outputDirectory));
        return;
    }

    m_outputPath.clear();

    m_workDirectory = QDir(m_outputDirectory).filePath(
        QStringLiteral(".mnetplus-%1-%2")
            .arg(media.videoId, QUuid::createUuid().toString(QUuid::Id128).left(8)));
    if (!QDir().mkpath(m_workDirectory)) {
        emit failed(MNET_TEXT("无法创建临时目录"));
        return;
    }
    m_mediaPath = QDir(m_workDirectory).filePath(QStringLiteral("media.mkv"));
    m_muxPath = QDir(m_workDirectory).filePath(QStringLiteral("final.mkv"));
    m_audioPath = QDir(m_workDirectory).filePath(QStringLiteral("audio.mka"));
    m_active = true;

    if (isMnetNetworkUrl(media.videoUrl)) {
        refreshMediaCookies();
    } else {
        beginTransfers();
    }
}

void DownloadController::refreshMediaCookies()
{
    if (!m_active || m_cancelRequested) return;
    emit stageChanged(MNET_TEXT("正在刷新媒体权限"));
    emit progressTextChanged(MNET_TEXT("正在请求 CloudFront 签名"));

    const QUrl url(QStringLiteral("https://api.mnetplus.world/media/v2/public/videos/%1/cookies")
                       .arg(m_media.videoId));
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Origin", "https://www.mnetplus.world");
    request.setRawHeader("Referer", m_media.pageUrl.toUtf8());
    request.setRawHeader("X-Optional-Authentication", "true");
    const QByteArray token = accessToken();
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    request.setTransferTimeout(30000);
    m_mediaCookieReply = m_network.post(request, QByteArray{});
    connect(m_mediaCookieReply, &QNetworkReply::finished,
            this, &DownloadController::handleMediaCookieReply);
}

void DownloadController::handleMediaCookieReply()
{
    if (!m_mediaCookieReply) return;
    QNetworkReply *reply = m_mediaCookieReply;
    m_mediaCookieReply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    reply->readAll();
    reply->deleteLater();

    if (!m_active || m_cancelRequested) return;
    if (networkError == QNetworkReply::NoError && status >= 200 && status < 300) {
        emit logMessage(MNET_TEXT("CloudFront 媒体签名已刷新"));
    } else if (status == 401 || status == 403) {
        emit logMessage(MNET_TEXT("媒体权限刷新被拒绝（HTTP %1），将检查现有签名")
                            .arg(status));
    } else {
        emit logMessage(MNET_TEXT("媒体权限刷新失败：%1；将检查现有签名")
                            .arg(networkMessage));
    }
    preflightVideo();
}

void DownloadController::preflightVideo()
{
    if (!m_active || m_cancelRequested) return;
    emit progressTextChanged(MNET_TEXT("正在验证视频流权限"));
    QNetworkRequest request{QUrl(m_media.videoUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "application/vnd.apple.mpegurl,application/x-mpegURL,*/*");
    request.setRawHeader("Referer", m_media.pageUrl.toUtf8());
    request.setTransferTimeout(30000);
    m_preflightReply = m_network.get(request);
    connect(m_preflightReply, &QNetworkReply::finished,
            this, &DownloadController::handlePreflightReply);
}

void DownloadController::handlePreflightReply()
{
    if (!m_preflightReply) return;
    QNetworkReply *reply = m_preflightReply;
    m_preflightReply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    reply->readAll();
    reply->deleteLater();

    if (!m_active || m_cancelRequested) return;
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 400) {
        if (m_preflightRetryCount < 2) {
            ++m_preflightRetryCount;
            emit logMessage(MNET_TEXT("视频清单读取失败，正在重试（%1/2）")
                                .arg(m_preflightRetryCount));
            preflightVideo();
            return;
        }
        if (status == 401 || status == 403) {
            finishWithError(MNET_TEXT("视频流权限验证失败（HTTP %1）。请确认所选浏览器已登录并可播放该视频")
                                .arg(status));
        } else {
            finishWithError(MNET_TEXT("视频流权限验证失败：%1").arg(networkMessage));
        }
        return;
    }

    m_preflightRetryCount = 0;
    m_cookies = m_network.cookieJar()->cookiesForUrl(QUrl(m_media.videoUrl));
    emit logMessage(MNET_TEXT("视频流权限验证通过，分辨率将在下载时自动探测"));
    if (m_media.audioDerived) {
        emit logMessage(MNET_TEXT("已自动匹配独立音频流：%1").arg(m_media.audioUrl));
    }
    beginTransfers();
}

void DownloadController::beginTransfers()
{
    if (!m_active || m_cancelRequested) return;
    emit stageChanged(MNET_TEXT("正在并行下载"));
    emit logMessage(MNET_TEXT("媒体与字幕将同时下载"));
    emit logMessage(MNET_TEXT("输出目录：%1").arg(m_outputDirectory));
    startMediaDownload();
    if (m_active) beginCaptionDownloads();
}

void DownloadController::startMediaDownload()
{
    const QUrl videoUrl(m_media.videoUrl);
    if ((videoUrl.scheme() == QStringLiteral("http")
         || videoUrl.scheme() == QStringLiteral("https"))
        && videoUrl.path().endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        startParallelHlsDownload();
        return;
    }
    startDirectMediaDownload();
}

void DownloadController::startDirectMediaDownload()
{
    const QString ffmpeg = findMediaTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        finishWithError(MNET_TEXT("未找到 ffmpeg，请先安装并确保它位于 PATH 中"));
        return;
    }

    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
        QStringLiteral("-stats_period"), QStringLiteral("1"),
    };
    const QString cookieLines = ffmpegCookieLines(m_cookies);
    appendNetworkInputOptions(arguments, m_media.videoUrl, cookieLines, m_media.pageUrl);
    arguments << QStringLiteral("-i") << m_media.videoUrl;

    if (!m_media.audioUrl.isEmpty()) {
        appendNetworkInputOptions(arguments, m_media.audioUrl, cookieLines, m_media.pageUrl);
        arguments << QStringLiteral("-i") << m_media.audioUrl
                  << QStringLiteral("-map") << QStringLiteral("0:v:0")
                  << QStringLiteral("-map") << QStringLiteral("1:a:0");
    } else {
        arguments << QStringLiteral("-map") << QStringLiteral("0:v:0")
                  << QStringLiteral("-map") << QStringLiteral("0:a:0");
    }
    arguments << QStringLiteral("-c:v") << QStringLiteral("copy")
              << QStringLiteral("-c:a") << QStringLiteral("copy")
              << QStringLiteral("-max_muxing_queue_size") << QStringLiteral("4096")
              << QStringLiteral("-metadata") << QStringLiteral("title=%1").arg(m_media.title)
              << m_mediaPath;

    m_ffmpegPhase = FfmpegPhase::MediaDownload;
    m_ffmpegTail.clear();
    m_ffmpeg.start(ffmpeg, arguments);
    if (!m_ffmpeg.waitForStarted(5000)) {
        finishWithError(MNET_TEXT("ffmpeg 启动失败：%1").arg(m_ffmpeg.errorString()));
    } else {
        m_ffmpegWatchdog.start();
    }
}

void DownloadController::startParallelHlsDownload()
{
    m_videoDownloadFinished = false;
    m_audioDownloadFinished = m_media.audioUrl.isEmpty();
    m_mediaAssemblyStarted = false;
    const QString videoDirectory = QDir(m_workDirectory).filePath(QStringLiteral("video"));
    m_hlsDownloader.setCookies(m_cookies);
    m_hlsDownloader.start(QUrl(m_media.videoUrl), videoDirectory, m_media.pageUrl);
    if (!m_media.audioUrl.isEmpty() && m_active) startAudioDownload();
}

void DownloadController::startAudioDownload()
{
    const QString ffmpeg = findMediaTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        finishWithError(MNET_TEXT("未找到 ffmpeg，无法下载独立音频"));
        return;
    }
    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
        QStringLiteral("-stats_period"), QStringLiteral("1"),
    };
    appendNetworkInputOptions(arguments, m_media.audioUrl,
                              ffmpegCookieLines(m_cookies), m_media.pageUrl);
    arguments << QStringLiteral("-i") << m_media.audioUrl
              << QStringLiteral("-map") << QStringLiteral("0:a:0")
              << QStringLiteral("-c:a") << QStringLiteral("copy")
              << m_audioPath;
    m_audioTail.clear();
    m_audioProcess.start(ffmpeg, arguments);
    if (!m_audioProcess.waitForStarted(5000)) {
        finishWithError(MNET_TEXT("独立音频下载进程启动失败：%1")
                            .arg(m_audioProcess.errorString()));
    } else {
        m_audioWatchdog.start();
        emit logMessage(MNET_TEXT("独立音频已与视频分片同时开始下载"));
    }
}

void DownloadController::handleAudioOutput()
{
    const QString chunk = QString::fromUtf8(m_audioProcess.readAllStandardError());
    m_audioWatchdog.start();
    m_audioTail += chunk;
    if (m_audioTail.size() > 12000) m_audioTail = m_audioTail.right(12000);
}

void DownloadController::handleAudioFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_audioWatchdog.stop();
    if (!m_active || m_cancelRequested) return;
    if (exitStatus != QProcess::NormalExit || exitCode != 0
        || !QFileInfo::exists(m_audioPath)) {
        QString detail = m_audioTail.trimmed().section(QLatin1Char('\n'), -1).trimmed();
        if (detail.isEmpty()) detail = MNET_TEXT("ffmpeg 返回代码 %1").arg(exitCode);
        finishWithError(MNET_TEXT("独立音频下载失败：%1").arg(detail));
        return;
    }
    m_audioDownloadFinished = true;
    emit logMessage(MNET_TEXT("独立音频下载完成"));
    maybeStartMediaAssembly();
}

void DownloadController::maybeStartMediaAssembly()
{
    if (!m_active || m_cancelRequested || m_mediaAssemblyStarted
        || !m_videoDownloadFinished || !m_audioDownloadFinished) {
        return;
    }
    m_mediaAssemblyStarted = true;
    startMediaAssembly();
}

void DownloadController::startMediaAssembly()
{
    const QString ffmpeg = findMediaTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        finishWithError(MNET_TEXT("未找到 ffmpeg，无法合并视频与音频"));
        return;
    }
    emit stageChanged(MNET_TEXT("正在本地合并音视频"));
    emit progressTextChanged(MNET_TEXT("并发下载完成，正在进行本地流复制"));
    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
        QStringLiteral("-protocol_whitelist"), QStringLiteral("file,crypto,data"),
        QStringLiteral("-allowed_extensions"), QStringLiteral("ALL"),
        QStringLiteral("-i"), m_videoPlaylistPath,
    };
    if (!m_audioPath.isEmpty() && QFileInfo::exists(m_audioPath)) {
        arguments << QStringLiteral("-i") << m_audioPath
                  << QStringLiteral("-map") << QStringLiteral("0:v:0")
                  << QStringLiteral("-map") << QStringLiteral("1:a:0");
    } else {
        arguments << QStringLiteral("-map") << QStringLiteral("0:v:0")
                  << QStringLiteral("-map") << QStringLiteral("0:a:0");
    }
    arguments << QStringLiteral("-c:v") << QStringLiteral("copy")
              << QStringLiteral("-c:a") << QStringLiteral("copy")
              << QStringLiteral("-max_muxing_queue_size") << QStringLiteral("4096")
              << QStringLiteral("-metadata") << QStringLiteral("title=%1").arg(m_media.title)
              << m_mediaPath;

    m_ffmpegPhase = FfmpegPhase::MediaDownload;
    m_ffmpegTail.clear();
    m_ffmpeg.start(ffmpeg, arguments);
    if (!m_ffmpeg.waitForStarted(5000)) {
        finishWithError(MNET_TEXT("本地音视频合并启动失败：%1").arg(m_ffmpeg.errorString()));
    } else {
        m_ffmpegWatchdog.start();
    }
}

void DownloadController::beginCaptionDownloads()
{
    if (!m_active || m_cancelRequested) return;
    if (!m_includeCaptions && !m_exportCaptions) {
        m_captionsFinished = true;
        maybeFinalize();
        return;
    }
    if (m_media.captionId.isEmpty() || m_media.captionLanguages.isEmpty()) {
        emit logMessage(MNET_TEXT("页面未内嵌字幕配置，正在通过视频 ID 查询字幕列表"));
        fetchCaptionList();
        return;
    }
    startCaptionDownloads();
}

void DownloadController::fetchCaptionList()
{
    if (!m_active || m_cancelRequested) return;
    emit stageChanged(MNET_TEXT("正在查询字幕列表"));
    QUrl url(m_captionApiBaseUrl);
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    path += QStringLiteral("%1/captions").arg(m_media.videoId);
    url.setPath(path);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Origin", "https://www.mnetplus.world");
    request.setRawHeader("Referer", m_media.pageUrl.toUtf8());
    const QByteArray token = accessToken();
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    request.setTransferTimeout(30000);
    m_captionListReply = m_network.get(request);
    connect(m_captionListReply, &QNetworkReply::finished,
            this, &DownloadController::handleCaptionListReply);
}

void DownloadController::handleCaptionListReply()
{
    if (!m_captionListReply) return;
    QNetworkReply *reply = m_captionListReply;
    m_captionListReply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    reply->deleteLater();

    if (!m_active || m_cancelRequested) return;
    if (networkError != QNetworkReply::NoError || status >= 400) {
        emit logMessage(MNET_TEXT("字幕列表查询失败（HTTP %1）：%2；将跳过字幕")
                            .arg(status).arg(networkMessage));
        m_captionsFinished = true;
        maybeFinalize();
        return;
    }

    QString discoveredCaptionId;
    QSet<QString> discoveredLanguages;
    const std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &value) {
        if (value.isObject()) {
            const QJsonObject object = value.toObject();
            const QString captionId = object.value(QStringLiteral("captionId")).toString();
            if (!captionId.isEmpty()) discoveredCaptionId = captionId;
            const QString language = object.value(QStringLiteral("language")).toString();
            if (!language.isEmpty()) discoveredLanguages.insert(normalizedCaptionLanguage(language));
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) walk(it.value());
        } else if (value.isArray()) {
            for (const QJsonValue &item : value.toArray()) walk(item);
        }
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error == QJsonParseError::NoError) walk(document.object());

    if (!discoveredCaptionId.isEmpty()) m_media.captionId = discoveredCaptionId;
    if (!discoveredLanguages.isEmpty()) {
        QStringList ordered = discoveredLanguages.values();
        ordered.sort(Qt::CaseInsensitive);
        m_media.captionLanguages = ordered;
        m_captionLanguages = ordered;
    }

    if (m_media.captionId.isEmpty()) {
        emit logMessage(MNET_TEXT("视频 ID 查询不到字幕配置，将继续下载媒体"));
        m_captionsFinished = true;
        maybeFinalize();
        return;
    }
    startCaptionDownloads();
}

void DownloadController::startCaptionDownloads()
{
    if (!m_active || m_cancelRequested) return;
    if (m_media.durationSecond <= 0.0) {
        emit logMessage(MNET_TEXT("页面没有提供可靠视频时长，为避免截断将跳过字幕"));
        m_captionsFinished = true;
        maybeFinalize();
        return;
    }

    emit logMessage(MNET_TEXT("开始并行读取 %1 种字幕，最大并发数 %2")
                        .arg(m_captionLanguages.size()).arg(kMaxCaptionConcurrency));
    for (const QString &language : std::as_const(m_captionLanguages)) {
        CaptionJob job;
        job.language = language;
        job.path = QDir(m_workDirectory).filePath(
            QStringLiteral("captions_%1.srt").arg(language));
        m_captionJobs.insert(language, job);
        enqueueCaptionRequest(CaptionRequest{language, 0, 0});
    }
    m_captionResponsesTotal = m_captionLanguages.size();
    pumpCaptionRequests();
}

void DownloadController::enqueueCaptionRequest(const CaptionRequest &request)
{
    if (!m_captionJobs.contains(request.language)
        || m_captionJobs.value(request.language).failed) {
        return;
    }
    m_captionQueue.enqueue(request);
}

void DownloadController::pumpCaptionRequests()
{
    if (!m_active || m_cancelRequested) return;
    while (m_captionReplies.size() < kMaxCaptionConcurrency && !m_captionQueue.isEmpty()) {
        const CaptionRequest captionRequest = m_captionQueue.dequeue();
        if (!m_captionJobs.contains(captionRequest.language)
            || m_captionJobs.value(captionRequest.language).failed) {
            continue;
        }

        QUrl url(m_captionApiBaseUrl);
        QString captionPath = url.path();
        if (!captionPath.endsWith(QLatin1Char('/'))) captionPath += QLatin1Char('/');
        captionPath += QStringLiteral("%1/captions/%2/cues")
                           .arg(m_media.videoId, m_media.captionId);
        url.setPath(captionPath);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("language"), captionRequest.language);
        query.addQueryItem(QStringLiteral("displaySecond"),
                           QString::number(captionRequest.second));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", kUserAgent);
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Origin", "https://www.mnetplus.world");
        request.setRawHeader("Referer", m_media.pageUrl.toUtf8());
        const QByteArray token = accessToken();
        if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
        request.setTransferTimeout(30000);
        QNetworkReply *reply = m_network.get(request);
        m_captionReplies.insert(reply, captionRequest);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply] { handleCaptionReply(reply); });
    }
    finishCaptionDownloadsIfReady();
}

void DownloadController::handleCaptionReply(QNetworkReply *reply)
{
    if (!m_captionReplies.contains(reply)) {
        reply->deleteLater();
        return;
    }
    const CaptionRequest request = m_captionReplies.take(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    reply->deleteLater();

    if (!m_active || m_cancelRequested || !m_captionJobs.contains(request.language)) return;
    CaptionJob &job = m_captionJobs[request.language];
    if (job.failed) {
        pumpCaptionRequests();
        return;
    }

    if (networkError != QNetworkReply::NoError || status >= 400) {
        const bool authorizationError = status == 401 || status == 403;
        if (!authorizationError && request.retryCount < 2) {
            CaptionRequest retry = request;
            ++retry.retryCount;
            emit logMessage(MNET_TEXT("%1 字幕窗口 %2 暂时失败，正在重试（%3/2）")
                                .arg(request.language).arg(request.second).arg(retry.retryCount));
            m_captionQueue.prepend(retry);
        } else if (authorizationError) {
            discardCaptionLanguage(request.language,
                QStringLiteral("%1 字幕授权失败（HTTP %2），已丢弃该语言")
                    .arg(request.language).arg(status));
        } else {
            discardCaptionLanguage(request.language,
                QStringLiteral("%1 字幕请求失败：%2；已丢弃该语言")
                    .arg(request.language, networkMessage));
        }
        enqueueRemainingCaptionWindowsIfReady();
        pumpCaptionRequests();
        return;
    }

    int interval = 20;
    if (!parseCaptionPayload(job, payload, &interval)) {
        discardCaptionLanguage(request.language,
            QStringLiteral("%1 字幕响应格式无效，已丢弃该语言").arg(request.language));
        enqueueRemainingCaptionWindowsIfReady();
        pumpCaptionRequests();
        return;
    }
    ++m_captionResponsesFinished;

    if (request.second == 0 && !job.initialized) {
        job.initialized = true;
        job.interval = interval;
    }

    enqueueRemainingCaptionWindowsIfReady();

    emit progressTextChanged(MNET_TEXT("字幕窗口 %1/%2 · 已获取 %3 条")
                                 .arg(m_captionResponsesFinished)
                                 .arg(qMax(m_captionResponsesTotal, m_captionResponsesFinished))
                                 .arg(job.cues.size()));
    pumpCaptionRequests();
}

void DownloadController::enqueueRemainingCaptionWindowsIfReady()
{
    if (m_captionWindowsQueued) return;
    for (const QString &language : std::as_const(m_captionLanguages)) {
        if (!m_captionJobs.contains(language)) continue;
        const CaptionJob &job = m_captionJobs.value(language);
        if (!job.failed && !job.initialized) return;
    }

    m_captionWindowsQueued = true;
    const int duration = qCeil(m_media.durationSecond);
    for (int windowIndex = 1; ; ++windowIndex) {
        bool addedWindow = false;
        for (const QString &language : std::as_const(m_captionLanguages)) {
            if (!m_captionJobs.contains(language)) continue;
            CaptionJob &job = m_captionJobs[language];
            if (job.failed || !job.initialized) continue;
            if (windowIndex >= kMaxCaptionWindowsPerLanguage) {
                const int second = windowIndex * job.interval;
                if (second < duration) {
                    discardCaptionLanguage(language,
                        MNET_TEXT("%1 字幕超过安全读取上限，已丢弃该语言")
                            .arg(language));
                }
                continue;
            }
            const int second = windowIndex * job.interval;
            if (second >= duration) continue;
            enqueueCaptionRequest(CaptionRequest{language, second, 0});
            ++m_captionResponsesTotal;
            addedWindow = true;
        }
        if (!addedWindow) break;
    }
}

bool DownloadController::parseCaptionPayload(CaptionJob &job,
                                             const QByteArray &payload,
                                             int *interval)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;

    const QJsonObject root = document.object();
    *interval = qMax(1, root.value(QStringLiteral("captionIntervalSecond")).toInt(20));
    const QJsonObject contentMap = root.value(QStringLiteral("contentMap")).toObject();
    for (auto it = contentMap.constBegin(); it != contentMap.constEnd(); ++it) {
        const QJsonObject object = it.value().toObject();
        bool keyIsNumber = false;
        const double keySecond = it.key().toDouble(&keyIsNumber);
        const double start = object.value(QStringLiteral("displaySecond")).toDouble(
            keyIsNumber ? keySecond : 0.0);
        const double duration = qMax(0.1,
            object.value(QStringLiteral("displayDurationSecond")).toDouble(2.0));
        const QString content = object.value(QStringLiteral("content")).toString().trimmed();
        if (content.isEmpty()) continue;
        const qint64 key = qRound64(start * 1000.0);
        const QString fingerprint = QStringLiteral("%1|%2|%3")
            .arg(key).arg(qRound64(duration * 1000.0)).arg(content);
        if (!job.cueFingerprints.contains(fingerprint)) {
            job.cues.insert(key, CaptionCue{start, duration, content});
            job.cueFingerprints.insert(fingerprint);
        }
    }
    return true;
}

void DownloadController::discardCaptionLanguage(const QString &language, const QString &message)
{
    if (!m_captionJobs.contains(language)) return;
    CaptionJob &job = m_captionJobs[language];
    if (job.failed) return;
    job.failed = true;
    job.cues.clear();
    job.cueFingerprints.clear();

    QQueue<CaptionRequest> remaining;
    while (!m_captionQueue.isEmpty()) {
        const CaptionRequest pending = m_captionQueue.dequeue();
        if (pending.language != language) remaining.enqueue(pending);
    }
    m_captionQueue = remaining;
    emit logMessage(message);
}

void DownloadController::finishCaptionDownloadsIfReady()
{
    if (!m_active || m_cancelRequested || m_captionsFinished
        || !m_captionQueue.isEmpty() || !m_captionReplies.isEmpty()) {
        return;
    }

    for (const QString &language : std::as_const(m_captionLanguages)) {
        if (!m_captionJobs.contains(language)) continue;
        const CaptionJob &job = m_captionJobs.value(language);
        if (job.failed) continue;
        if (job.cues.isEmpty()) {
            emit logMessage(MNET_TEXT("%1 未获取到可用字幕").arg(language));
        } else if (writeSrt(job)) {
            m_captionTracks.append(CaptionTrack{language, job.path});
            emit logMessage(MNET_TEXT("%1 字幕转换完成，共 %2 条")
                                .arg(language).arg(job.cues.size()));
        } else {
            emit logMessage(MNET_TEXT("%1 字幕文件写入失败").arg(language));
        }
    }
    m_captionsFinished = true;
    if (!m_mediaFinished) {
        emit stageChanged(MNET_TEXT("字幕完成，正在下载媒体"));
        emit progressTextChanged(MNET_TEXT("已准备 %1 条字幕轨")
                                     .arg(m_captionTracks.size()));
    }
    maybeFinalize();
}

bool DownloadController::writeSrt(const CaptionJob &job)
{
    QSaveFile file(job.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    int index = 1;
    for (const CaptionCue &cue : job.cues) {
        stream << index++ << '\n'
               << srtTimestamp(cue.startSecond) << " --> "
               << srtTimestamp(cue.startSecond + cue.durationSecond) << '\n'
               << normalizeCaption(cue.content) << "\n\n";
    }
    return file.commit();
}

bool DownloadController::exportSrtFiles()
{
    const QString baseName = QFileInfo(m_outputPath).completeBaseName();
    m_exportedSrtPaths.clear();
    for (const CaptionTrack &track : std::as_const(m_captionTracks)) {
        if (!QFileInfo::exists(track.path)) {
            for (const QString &path : std::as_const(m_exportedSrtPaths)) QFile::remove(path);
            m_exportedSrtPaths.clear();
            return false;
        }
        const QString destinationPath = QDir(m_outputDirectory).filePath(
            QStringLiteral("%1.%2.srt").arg(baseName, track.language));
        if (QFileInfo::exists(destinationPath)) {
            for (const QString &path : std::as_const(m_exportedSrtPaths)) QFile::remove(path);
            m_exportedSrtPaths.clear();
            return false;
        }
        if (!QFile::rename(track.path, destinationPath)) {
            for (const QString &path : std::as_const(m_exportedSrtPaths)) QFile::remove(path);
            m_exportedSrtPaths.clear();
            return false;
        }
        m_exportedSrtPaths.append(destinationPath);
        emit logMessage(MNET_TEXT("SRT 已导出：%1").arg(destinationPath));
    }
    return true;
}

void DownloadController::maybeFinalize()
{
    if (!m_active || m_cancelRequested || m_finalizing
        || !m_mediaFinished || !m_captionsFinished) {
        return;
    }
    m_finalizing = true;

    if (m_includeCaptions && !m_captionTracks.isEmpty()) {
        startFinalMux();
        return;
    }
    commitMediaFile(m_mediaPath);
}

void DownloadController::startFinalMux()
{
    const QString ffmpeg = findMediaTool(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        finishWithError(MNET_TEXT("未找到 ffmpeg，无法内封字幕"));
        return;
    }

    emit stageChanged(MNET_TEXT("正在快速内封字幕"));
    emit progressTextChanged(MNET_TEXT("正在进行本地流复制"));
    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"),
        QStringLiteral("-i"), m_mediaPath,
    };
    QList<int> captionInputs;
    int nextInput = 1;
    for (const CaptionTrack &track : std::as_const(m_captionTracks)) {
        if (!QFileInfo::exists(track.path)) continue;
        arguments << QStringLiteral("-i") << track.path;
        captionInputs.append(nextInput++);
    }
    arguments << QStringLiteral("-map") << QStringLiteral("0:v:0")
              << QStringLiteral("-map") << QStringLiteral("0:a:0");
    for (int input : std::as_const(captionInputs)) {
        arguments << QStringLiteral("-map") << QStringLiteral("%1:0").arg(input);
    }
    arguments << QStringLiteral("-c:v") << QStringLiteral("copy")
              << QStringLiteral("-c:a") << QStringLiteral("copy")
              << QStringLiteral("-c:s") << QStringLiteral("srt");
    for (int index = 0; index < captionInputs.size(); ++index) {
        const CaptionTrack &track = m_captionTracks.at(index);
        arguments << QStringLiteral("-metadata:s:s:%1").arg(index)
                  << QStringLiteral("language=%1").arg(matroskaLanguage(track.language))
                  << QStringLiteral("-metadata:s:s:%1").arg(index)
                  << QStringLiteral("title=%1").arg(captionTrackTitle(track.language));
    }
    arguments << QStringLiteral("-metadata") << QStringLiteral("title=%1").arg(m_media.title)
              << m_muxPath;

    m_ffmpegPhase = FfmpegPhase::FinalMux;
    m_ffmpegTail.clear();
    m_ffmpeg.start(ffmpeg, arguments);
    if (!m_ffmpeg.waitForStarted(5000)) {
        finishWithError(MNET_TEXT("ffmpeg 启动失败：%1").arg(m_ffmpeg.errorString()));
    } else {
        m_ffmpegWatchdog.start();
    }
}

bool DownloadController::moveFileToOutput(const QString &sourcePath)
{
    if (QFileInfo::exists(m_outputPath)) return false;
    if (!QFile::rename(sourcePath, m_outputPath)) return false;
    m_outputOwned = true;
    return true;
}

void DownloadController::handleFfmpegOutput()
{
    const QString chunk = QString::fromUtf8(m_ffmpeg.readAllStandardError());
    m_ffmpegWatchdog.start();
    m_ffmpegTail += chunk;
    if (m_ffmpegTail.size() > 16000) m_ffmpegTail = m_ffmpegTail.right(16000);

    static const QRegularExpression timeExpression(
        QStringLiteral(R"(time=(\d{2}:\d{2}:\d{2}(?:\.\d+)?))"));
    auto iterator = timeExpression.globalMatch(chunk);
    QString latest;
    while (iterator.hasNext()) latest = iterator.next().captured(1);
    if (!latest.isEmpty()) {
        emit progressTextChanged(m_ffmpegPhase == FfmpegPhase::MediaDownload
            ? MNET_TEXT("媒体已下载 %1").arg(latest)
            : MNET_TEXT("字幕已内封 %1").arg(latest));
    }
}

void DownloadController::handleFfmpegFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_ffmpegWatchdog.stop();
    const FfmpegPhase phase = std::exchange(m_ffmpegPhase, FfmpegPhase::Idle);
    if (!m_active) return;
    if (m_cancelRequested) {
        if (m_outputOwned) QFile::remove(m_outputPath);
        cleanup();
        m_active = false;
        emit canceled();
        return;
    }

    const QString expectedPath = phase == FfmpegPhase::MediaDownload
        ? m_mediaPath : m_muxPath;
    if (exitStatus != QProcess::NormalExit || exitCode != 0
        || !QFileInfo::exists(expectedPath)) {
        QString detail = m_ffmpegTail.trimmed().section(QLatin1Char('\n'), -1).trimmed();
        if (detail.isEmpty()) detail = MNET_TEXT("ffmpeg 返回代码 %1").arg(exitCode);
        finishWithError(phase == FfmpegPhase::MediaDownload
            ? MNET_TEXT("媒体下载失败：%1").arg(detail)
            : MNET_TEXT("字幕内封失败：%1").arg(detail));
        return;
    }

    if (phase == FfmpegPhase::MediaDownload) {
        m_mediaFinished = true;
        emit logMessage(MNET_TEXT("视频与音频下载完成"));
        if (!m_captionsFinished) {
            emit stageChanged(MNET_TEXT("媒体完成，正在获取字幕"));
        }
        maybeFinalize();
        return;
    }
    commitMediaFile(m_muxPath);
}

QString DownloadController::validateOutput(const QString &path)
{
    const QString ffprobe = findMediaTool(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) return MNET_TEXT("未找到 ffprobe，无法验证 MKV 轨道完整性");

    QProcess process;
    process.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=codec_type,codec_name,duration,width,height:stream_tags=DURATION"),
        QStringLiteral("-of"), QStringLiteral("json"),
        path,
    });
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000)) {
        process.kill();
        return MNET_TEXT("MKV 轨道验证超时");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return MNET_TEXT("ffprobe 验证失败：%1").arg(
            detail.isEmpty() ? MNET_TEXT("返回代码 %1").arg(process.exitCode()) : detail);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return MNET_TEXT("ffprobe 返回了无效的轨道信息");
    }

    double videoDuration = 0.0;
    double audioDuration = 0.0;
    int subtitleCount = 0;
    m_actualVideoWidth = 0;
    m_actualVideoHeight = 0;
    m_actualVideoCodec.clear();
    const QJsonArray streams = document.object().value(QStringLiteral("streams")).toArray();
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        double duration = stream.value(QStringLiteral("duration")).toString().toDouble();
        if (duration <= 0.0) {
            duration = clockDuration(stream.value(QStringLiteral("tags")).toObject()
                                         .value(QStringLiteral("DURATION")).toString());
        }
        const QString type = stream.value(QStringLiteral("codec_type")).toString();
        if (type == QStringLiteral("video")) {
            videoDuration = qMax(videoDuration, duration);
            m_actualVideoWidth = qMax(m_actualVideoWidth,
                                      stream.value(QStringLiteral("width")).toInt());
            m_actualVideoHeight = qMax(m_actualVideoHeight,
                                       stream.value(QStringLiteral("height")).toInt());
            if (m_actualVideoCodec.isEmpty()) {
                m_actualVideoCodec = codecLabel(stream.value(QStringLiteral("codec_name")).toString());
            }
        }
        if (type == QStringLiteral("audio")) audioDuration = qMax(audioDuration, duration);
        if (type == QStringLiteral("subtitle")) ++subtitleCount;
    }
    if (videoDuration <= 0.0 || audioDuration <= 0.0) {
        return MNET_TEXT("合并结果缺少视频或音频轨道，已删除不完整文件");
    }
    const double allowedDifference = qMax(5.0, videoDuration * 0.005);
    if (qAbs(videoDuration - audioDuration) > allowedDifference) {
        return MNET_TEXT("音视频时长不一致（视频 %1 秒，音频 %2 秒），已删除不完整文件")
            .arg(videoDuration, 0, 'f', 1).arg(audioDuration, 0, 'f', 1);
    }
    const int expectedSubtitles = m_includeCaptions ? m_captionTracks.size() : 0;
    if (subtitleCount < expectedSubtitles) {
        return MNET_TEXT("合并结果缺少字幕轨道，已删除不完整文件");
    }
    return {};
}

void DownloadController::commitMediaFile(const QString &sourcePath)
{
    if (!m_active || m_cancelRequested) return;
    const QString validationError = validateOutput(sourcePath);
    if (!validationError.isEmpty()) {
        finishWithError(validationError);
        return;
    }
    if (!resolveFinalOutputPath()) {
        finishWithError(MNET_TEXT("无法确定输出文件名"));
        return;
    }
    if (!moveFileToOutput(sourcePath)) {
        finishWithError(MNET_TEXT("无法提交已完成的 MKV 文件"));
        return;
    }
    finalizeCompleted();
}

QString DownloadController::buildOutputFileName() const
{
    return expandFilenameTemplate(m_media.filenameTemplate, m_media.title, m_media.publishDate,
                                  m_actualVideoHeight, m_actualVideoCodec, QString{});
}

bool DownloadController::resolveFinalOutputPath()
{
    const QString baseName = buildOutputFileName().isEmpty()
        ? safeFileName(m_media.title) : buildOutputFileName();
    m_outputPath = QDir(m_outputDirectory).filePath(baseName + QStringLiteral(".mkv"));

    const auto outputExists = [this](const QString &mkvPath) {
        if (QFileInfo::exists(mkvPath)) return true;
        if (!m_exportCaptions) return false;
        const QString candidateBase = QFileInfo(mkvPath).completeBaseName();
        for (const QString &language : std::as_const(m_captionLanguages)) {
            if (QFileInfo::exists(QDir(m_outputDirectory).filePath(
                    QStringLiteral("%1.%2.srt").arg(candidateBase, language)))) {
                return true;
            }
        }
        return false;
    };
    int suffix = 2;
    while (outputExists(m_outputPath)) {
        m_outputPath = QDir(m_outputDirectory).filePath(
            QStringLiteral("%1 (%2).mkv").arg(baseName).arg(suffix++));
    }
    return true;
}

void DownloadController::finalizeCompleted()
{
    if (m_exportCaptions && !m_captionTracks.isEmpty() && !exportSrtFiles()) {
        finishWithError(MNET_TEXT("独立 SRT 文件导出失败"));
        return;
    }
    if (m_exportCaptions && m_captionTracks.isEmpty()) {
        emit logMessage(MNET_TEXT("没有获取到完整字幕，因此未导出 SRT"));
    }
    if (m_actualVideoWidth > 0 && m_actualVideoHeight > 0) {
        emit logMessage(MNET_TEXT("实际视频：%1x%2 · %3")
                            .arg(m_actualVideoWidth).arg(m_actualVideoHeight)
                            .arg(m_actualVideoCodec));
    }
    emit logMessage(MNET_TEXT("输出文件：%1").arg(m_outputPath));

    cleanup();
    m_active = false;
    m_finalizing = false;
    emit stageChanged(MNET_TEXT("已完成"));
    emit progressTextChanged(m_exportCaptions && !m_captionTracks.isEmpty()
        ? MNET_TEXT("MKV 与 SRT 已保存") : MNET_TEXT("MKV 已保存"));
    emit completed(m_outputPath);
}

void DownloadController::cancel()
{
    if (!m_active) return;
    m_cancelRequested = true;
    m_hlsDownloader.cancel();
    if (m_audioProcess.state() != QProcess::NotRunning) {
        m_audioWatchdog.stop();
        m_audioProcess.blockSignals(true);
        m_audioProcess.kill();
        m_audioProcess.waitForFinished(3000);
        m_audioProcess.blockSignals(false);
    }
    if (m_mediaCookieReply) {
        m_mediaCookieReply->abort();
        m_mediaCookieReply->deleteLater();
        m_mediaCookieReply = nullptr;
    }
    if (m_preflightReply) {
        m_preflightReply->abort();
        m_preflightReply->deleteLater();
        m_preflightReply = nullptr;
    }
    if (m_captionListReply) {
        m_captionListReply->abort();
        m_captionListReply->deleteLater();
        m_captionListReply = nullptr;
    }
    const QList<QNetworkReply *> replies = m_captionReplies.keys();
    m_captionReplies.clear();
    for (QNetworkReply *reply : replies) {
        reply->abort();
        reply->deleteLater();
    }
    m_captionQueue.clear();
    if (m_ffmpeg.state() != QProcess::NotRunning) {
        m_ffmpegWatchdog.stop();
        m_ffmpeg.blockSignals(true);
        m_ffmpeg.kill();
        m_ffmpeg.waitForFinished(3000);
        m_ffmpeg.blockSignals(false);
    }
    m_ffmpegPhase = FfmpegPhase::Idle;
    if (m_outputOwned) QFile::remove(m_outputPath);
    for (const QString &path : std::as_const(m_exportedSrtPaths)) QFile::remove(path);
    m_exportedSrtPaths.clear();
    cleanup();
    m_active = false;
    emit canceled();
}

bool DownloadController::isActive() const
{
    return m_active;
}

void DownloadController::finishWithError(const QString &message)
{
    m_hlsDownloader.cancel();
    if (m_audioProcess.state() != QProcess::NotRunning) {
        m_audioWatchdog.stop();
        m_audioProcess.blockSignals(true);
        m_audioProcess.kill();
        m_audioProcess.waitForFinished(3000);
        m_audioProcess.blockSignals(false);
    }
    if (m_ffmpeg.state() != QProcess::NotRunning) {
        m_ffmpegWatchdog.stop();
        m_ffmpeg.blockSignals(true);
        m_ffmpeg.kill();
        m_ffmpeg.waitForFinished(3000);
        m_ffmpeg.blockSignals(false);
    }
    if (m_mediaCookieReply) {
        m_mediaCookieReply->abort();
        m_mediaCookieReply->deleteLater();
        m_mediaCookieReply = nullptr;
    }
    if (m_preflightReply) {
        m_preflightReply->abort();
        m_preflightReply->deleteLater();
        m_preflightReply = nullptr;
    }
    if (m_captionListReply) {
        m_captionListReply->abort();
        m_captionListReply->deleteLater();
        m_captionListReply = nullptr;
    }
    const QList<QNetworkReply *> replies = m_captionReplies.keys();
    m_captionReplies.clear();
    for (QNetworkReply *reply : replies) {
        reply->abort();
        reply->deleteLater();
    }
    m_captionQueue.clear();
    m_ffmpegPhase = FfmpegPhase::Idle;
    if (m_outputOwned) QFile::remove(m_outputPath);
    for (const QString &path : std::as_const(m_exportedSrtPaths)) QFile::remove(path);
    m_exportedSrtPaths.clear();
    cleanup();
    m_active = false;
    m_finalizing = false;
    emit failed(message);
}

void DownloadController::cleanup()
{
    if (!m_workDirectory.isEmpty()) {
        QDir(m_workDirectory).removeRecursively();
    }
}

QString DownloadController::safeFileName(QString title)
{
    title.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")),
                  QStringLiteral(" "));
    title.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    title = title.trimmed();
    while (title.endsWith(QLatin1Char('.'))) title.chop(1);
    if (title.isEmpty()) title = QStringLiteral("Mnet Plus Video");
#if defined(Q_OS_WIN)
    static const QRegularExpression reservedName(
        QStringLiteral(R"(^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reservedName.match(title).hasMatch()) title.prepend(QLatin1Char('_'));
#endif
    if (title.size() > 180) title = title.left(180).trimmed();
    return title;
}

QString DownloadController::srtTimestamp(double second)
{
    const qint64 totalMilliseconds = qMax<qint64>(0, qRound64(second * 1000.0));
    const qint64 hours = totalMilliseconds / 3600000;
    const qint64 minutes = (totalMilliseconds / 60000) % 60;
    const qint64 seconds = (totalMilliseconds / 1000) % 60;
    const qint64 milliseconds = totalMilliseconds % 1000;
    return QStringLiteral("%1:%2:%3,%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

QString DownloadController::normalizeCaption(const QString &content)
{
    QString html = content;
    html.replace(QRegularExpression(QStringLiteral(R"(<br\s*/?>)"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral("\n"));
    html.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    QTextDocument document;
    document.setHtml(QStringLiteral("<span>%1</span>").arg(html));
    return document.toPlainText().trimmed();
}

QByteArray DownloadController::accessToken() const
{
    const QList<QNetworkCookie> cookies = m_network.cookieJar()->cookiesForUrl(
        QUrl(QStringLiteral("https://api.mnetplus.world/")));
    for (const QNetworkCookie &cookie : cookies) {
        if (cookie.name() == QByteArray("_mnet_atk")) return cookie.value();
    }
    return {};
}
