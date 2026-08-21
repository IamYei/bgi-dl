#include "hlsdownloader.h"
#include "localization.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <utility>

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
constexpr int kMaxConcurrency = 6;
constexpr int kMaxManifestDepth = 3;

bool isAllowedMediaUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))
        && (host == QStringLiteral("mnetplus.world")
            || host.endsWith(QStringLiteral(".mnetplus.world"))
            || host == QStringLiteral("127.0.0.1")
            || host == QStringLiteral("localhost"));
}

QString attributeValue(const QString &line, const QString &name)
{
    const QRegularExpression expression(
        QStringLiteral(R"REGEX((?:^|,)%1=(?:"([^"]*)"|([^,]*)))REGEX").arg(
            QRegularExpression::escape(name)),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = expression.match(line.section(QLatin1Char(':'), 1));
    return match.hasMatch() ? (match.captured(1).isNull()
        ? match.captured(2).trimmed() : match.captured(1)) : QString{};
}

int resolutionHeight(const QString &line)
{
    static const QRegularExpression expression(
        QStringLiteral(R"((?:^|,)RESOLUTION=\d+x(\d+)(?:,|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(line.section(QLatin1Char(':'), 1)).captured(1).toInt();
}

int heightFromUrl(const QUrl &url)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(_(\d{3,4})pw)"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(url.path()).captured(1).toInt();
}

QUrl derivedResolutionUrl(const QUrl &url, int targetHeight)
{
    QUrl upgraded(url);
    QString path = upgraded.path();
    static const QRegularExpression expression(
        QStringLiteral(R"(_\d{3,4}pw)"),
        QRegularExpression::CaseInsensitiveOption);
    if (!path.contains(expression)) return {};
    path.replace(expression, QStringLiteral("_%1pw").arg(targetHeight));
    upgraded.setPath(path);
    return upgraded;
}

bool parseByteRange(const QString &value, qint64 *length, qint64 *offset)
{
    static const QRegularExpression expression(QStringLiteral(R"(^(\d+)(?:@(\d+))?$)"));
    const auto match = expression.match(value.trimmed());
    if (!match.hasMatch()) return false;
    *length = match.captured(1).toLongLong();
    *offset = match.captured(2).isEmpty() ? -1 : match.captured(2).toLongLong();
    return *length > 0;
}
}

HlsDownloader::HlsDownloader(QObject *parent)
    : QObject(parent)
{
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));
}

void HlsDownloader::setCookies(const QList<QNetworkCookie> &cookies)
{
    auto *jar = new QNetworkCookieJar(&m_network);
    m_network.setCookieJar(jar);
    for (const QNetworkCookie &cookie : cookies) {
        QString host = cookie.domain();
        if (host.startsWith(QLatin1Char('.'))) host.remove(0, 1);
        jar->setCookiesFromUrl({cookie}, QUrl(QStringLiteral("https://%1/").arg(host)));
    }
}

void HlsDownloader::setMaxHeight(int height)
{
    m_maxHeight = qMax(240, qMin(2160, height));
}

void HlsDownloader::start(const QUrl &manifestUrl, const QString &outputDirectory,
                          const QString &referer)
{
    if (m_active) return;
    if (!manifestUrl.isValid() || !isAllowedMediaUrl(manifestUrl)) {
        emit failed(MNET_TEXT("HLS 清单地址无效或不受支持"));
        return;
    }
    if (!QDir().mkpath(outputDirectory)) {
        emit failed(MNET_TEXT("无法创建视频分片目录"));
        return;
    }

    m_manifestUrl = manifestUrl;
    m_outputDirectory = outputDirectory;
    m_playlistPath = QDir(outputDirectory).filePath(QStringLiteral("local.m3u8"));
    m_referer = referer;
    m_manifestDepth = 0;
    m_expectedHeight = 0;
    m_totalResources = 0;
    m_completedResources = 0;
    m_playlistLines.clear();
    m_resourceQueue.clear();
    m_resourceReplies.clear();
    m_resourcePaths.clear();
    m_manifestFallbacks.clear();
    m_cancelRequested = false;
    m_active = true;
    fetchManifest(manifestUrl, 0, 0);
}

void HlsDownloader::fetchManifest(const QUrl &url, int depth, int hintedHeight)
{
    if (!m_active || m_cancelRequested) return;
    if (depth > kMaxManifestDepth || !isAllowedMediaUrl(url)) {
        fail(MNET_TEXT("HLS 清单层级或地址无效"));
        return;
    }
    m_manifestUrl = url;
    m_manifestDepth = depth;
    if (hintedHeight > 0) m_expectedHeight = hintedHeight;
    m_manifestReply = m_network.get(requestFor(url));
    connect(m_manifestReply, &QNetworkReply::finished,
            this, &HlsDownloader::handleManifestReply);
}

void HlsDownloader::handleManifestReply()
{
    if (!m_manifestReply) return;
    QNetworkReply *reply = m_manifestReply;
    m_manifestReply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    const QByteArray payload = reply->readAll();
    const QUrl responseUrl = reply->url();
    reply->deleteLater();

    if (!m_active || m_cancelRequested) return;
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 400) {
        if (!m_manifestFallbacks.isEmpty()) {
            const ManifestCandidate next = m_manifestFallbacks.takeFirst();
            emit logMessage(MNET_TEXT("清单 %1 不可用（HTTP %2），尝试下一档 %3p")
                                .arg(responseUrl.toString(QUrl::FullyEncoded))
                                .arg(status).arg(next.height));
            fetchManifest(next.url, next.depth, next.height);
        } else {
            fail(MNET_TEXT("HLS 清单读取失败（HTTP %1）：%2").arg(status).arg(networkMessage));
        }
        return;
    }
    if (!payload.startsWith("#EXTM3U")) {
        fail(MNET_TEXT("服务器返回的不是有效 HLS 清单"));
        return;
    }

    const QStringList lines = QString::fromUtf8(payload).split(
        QRegularExpression(QStringLiteral("\\r?\\n")), Qt::KeepEmptyParts);
    struct Variant { QUrl url; int height = 0; qint64 bandwidth = 0; };
    QList<Variant> variants;
    QUrl audioStreamUrl;
    for (int index = 0; index < lines.size(); ++index) {
        const QString line = lines.at(index).trimmed();
        if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:"), Qt::CaseInsensitive)) {
            int uriIndex = index + 1;
            while (uriIndex < lines.size() && lines.at(uriIndex).trimmed().isEmpty()) ++uriIndex;
            if (uriIndex >= lines.size() || lines.at(uriIndex).trimmed().startsWith(QLatin1Char('#'))) continue;
            const QUrl variantUrl = responseUrl.resolved(QUrl(lines.at(uriIndex).trimmed()));
            if (!isAllowedMediaUrl(variantUrl)) continue;
            int height = resolutionHeight(line);
            if (height <= 0) height = heightFromUrl(variantUrl);
            variants.append(Variant{variantUrl, height,
                                    attributeValue(line, QStringLiteral("BANDWIDTH")).toLongLong()});
        } else if (line.startsWith(QStringLiteral("#EXT-X-MEDIA:"), Qt::CaseInsensitive)) {
            const QString type = attributeValue(line, QStringLiteral("TYPE"));
            const QString uri = attributeValue(line, QStringLiteral("URI"));
            if (type.compare(QStringLiteral("AUDIO"), Qt::CaseInsensitive) == 0 && !uri.isEmpty()) {
                const QUrl resolved = responseUrl.resolved(QUrl(uri));
                if (isAllowedMediaUrl(resolved)) audioStreamUrl = resolved;
            }
        }
    }

    if (audioStreamUrl.isValid()) emit audioStreamDiscovered(audioStreamUrl);

    if (!variants.isEmpty()) {
        std::sort(variants.begin(), variants.end(), [](const Variant &left, const Variant &right) {
            if (left.height != right.height) return left.height > right.height;
            return left.bandwidth > right.bandwidth;
        });

        QList<ManifestCandidate> candidates;
        const int maxListedHeight = variants.constFirst().height;
        const int preferredHeights[] = {2160, 1440, 1080};
        for (const int target : preferredHeights) {
            if (target <= maxListedHeight || target > m_maxHeight) continue;
            const QUrl derived = derivedResolutionUrl(variants.constFirst().url, target);
            if (!derived.isEmpty() && isAllowedMediaUrl(derived)) {
                candidates.append(ManifestCandidate{derived, target, m_manifestDepth + 1});
                emit logMessage(MNET_TEXT("尝试更高分辨率 %1p：%2")
                                    .arg(target).arg(derived.toString(QUrl::FullyEncoded)));
            }
        }
        for (const Variant &variant : std::as_const(variants)) {
            candidates.append(ManifestCandidate{variant.url, variant.height, m_manifestDepth + 1});
        }

        QSet<QString> seenUrls;
        QList<ManifestCandidate> deduped;
        for (const ManifestCandidate &candidate : std::as_const(candidates)) {
            const QString key = candidate.url.toString(QUrl::FullyEncoded);
            if (seenUrls.contains(key)) continue;
            seenUrls.insert(key);
            deduped.append(candidate);
        }
        if (deduped.isEmpty()) {
            fail(MNET_TEXT("HLS master 没有可用的视频流"));
            return;
        }
        const ManifestCandidate first = deduped.takeFirst();
        m_manifestFallbacks = deduped;
        emit logMessage(MNET_TEXT("已选择 %1p 视频流候选").arg(first.height));
        fetchManifest(first.url, first.depth, first.height);
        return;
    }

    if (m_expectedHeight <= 0) m_expectedHeight = heightFromUrl(responseUrl);

    // When the page exposes a direct media playlist (not a master), still probe higher
    // resolutions by rewriting the `_<height>pw` marker in the URL (e.g. `_720pw_h264.m3u8`
    // -> `_2160pw_h264.m3u8`). This only runs once at the top level to avoid loops.
    if (m_manifestDepth == 0 && m_manifestFallbacks.isEmpty() && m_expectedHeight > 0) {
        const int preferredHeights[] = {2160, 1440, 1080};
        QList<ManifestCandidate> candidates;
        for (const int target : preferredHeights) {
            if (target <= m_expectedHeight || target > m_maxHeight) continue;
            const QUrl derived = derivedResolutionUrl(responseUrl, target);
            if (!derived.isEmpty() && isAllowedMediaUrl(derived)) {
                candidates.append(ManifestCandidate{derived, target, m_manifestDepth + 1});
                emit logMessage(MNET_TEXT("尝试更高分辨率 %1p：%2")
                                    .arg(target).arg(derived.toString(QUrl::FullyEncoded)));
            }
        }
        if (!candidates.isEmpty()) {
            candidates.append(ManifestCandidate{responseUrl, m_expectedHeight, m_manifestDepth + 1});
            const ManifestCandidate first = candidates.takeFirst();
            m_manifestFallbacks = candidates;
            fetchManifest(first.url, first.depth, first.height);
            return;
        }
    }

    QString error;
    if (!parseMediaPlaylist(payload, responseUrl, &error)) {
        fail(error);
        return;
    }
    emit mediaPlaylistSelected(responseUrl, m_expectedHeight);
    emit logMessage(MNET_TEXT("视频清单确认：%1 个资源，%2 路并发下载")
                        .arg(m_totalResources).arg(kMaxConcurrency));
    pumpDownloads();
}

bool HlsDownloader::parseMediaPlaylist(const QByteArray &payload, const QUrl &baseUrl,
                                       QString *error)
{
    const QStringList lines = QString::fromUtf8(payload).split(
        QRegularExpression(QStringLiteral("\\r?\\n")), Qt::KeepEmptyParts);
    if (!lines.contains(QStringLiteral("#EXT-X-ENDLIST"))) {
        *error = MNET_TEXT("暂不支持仍在更新的直播 HLS 清单");
        return false;
    }

    qint64 pendingRangeLength = -1;
    qint64 pendingRangeOffset = -1;
    qint64 previousRangeEnd = 0;
    QUrl previousRangeUrl;
    bool hasMediaSegment = false;
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("#EXT-X-BYTERANGE:"), Qt::CaseInsensitive)) {
            if (!parseByteRange(line.section(QLatin1Char(':'), 1),
                                &pendingRangeLength, &pendingRangeOffset)) {
                *error = MNET_TEXT("HLS BYTERANGE 格式无效");
                return false;
            }
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-MAP:"), Qt::CaseInsensitive)
            || line.startsWith(QStringLiteral("#EXT-X-KEY:"), Qt::CaseInsensitive)) {
            const QString uri = attributeValue(line, QStringLiteral("URI"));
            if (uri.isEmpty()) {
                m_playlistLines.append(rawLine);
                continue;
            }
            const QUrl resourceUrl = baseUrl.resolved(QUrl(uri));
            if (!isAllowedMediaUrl(resourceUrl)) {
                *error = MNET_TEXT("HLS 包含不受支持的资源地址");
                return false;
            }
            qint64 rangeLength = -1;
            qint64 rangeOffset = -1;
            const QString mapRange = attributeValue(line, QStringLiteral("BYTERANGE"));
            if (!mapRange.isEmpty() && !parseByteRange(mapRange, &rangeLength, &rangeOffset)) {
                *error = MNET_TEXT("HLS MAP BYTERANGE 格式无效");
                return false;
            }
            if (rangeLength > 0 && rangeOffset < 0) {
                *error = MNET_TEXT("HLS MAP BYTERANGE 缺少偏移量");
                return false;
            }
            const QString localName = addResource(resourceUrl, rangeOffset, rangeLength);
            QString rewritten = rawLine;
            rewritten.replace(QStringLiteral("URI=\"%1\"").arg(uri),
                              QStringLiteral("URI=\"%1\"").arg(localName));
            rewritten.remove(QRegularExpression(
                QStringLiteral(R"(,?BYTERANGE="[^"]+")"),
                QRegularExpression::CaseInsensitiveOption));
            m_playlistLines.append(rewritten);
            continue;
        }
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            const QUrl resourceUrl = baseUrl.resolved(QUrl(line));
            if (!isAllowedMediaUrl(resourceUrl)) {
                *error = MNET_TEXT("HLS 包含不受支持的视频分片地址");
                return false;
            }
            qint64 rangeOffset = pendingRangeOffset;
            if (pendingRangeLength > 0 && rangeOffset < 0) {
                if (resourceUrl != previousRangeUrl) {
                    *error = MNET_TEXT("HLS BYTERANGE 缺少有效偏移量");
                    return false;
                }
                rangeOffset = previousRangeEnd;
            }
            const QString localName = addResource(resourceUrl, rangeOffset, pendingRangeLength);
            m_playlistLines.append(localName);
            if (pendingRangeLength > 0) {
                previousRangeUrl = resourceUrl;
                previousRangeEnd = rangeOffset + pendingRangeLength;
            }
            pendingRangeLength = -1;
            pendingRangeOffset = -1;
            hasMediaSegment = true;
            continue;
        }
        m_playlistLines.append(rawLine);
    }
    if (!hasMediaSegment || m_totalResources == 0) {
        *error = MNET_TEXT("HLS 清单中没有可下载的视频分片");
        return false;
    }
    return true;
}

QString HlsDownloader::addResource(const QUrl &url, qint64 rangeStart, qint64 rangeLength)
{
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(url.toString(QUrl::FullyEncoded))
                            .arg(rangeStart).arg(rangeLength);
    if (m_resourcePaths.contains(key)) return m_resourcePaths.value(key);

    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > 8) suffix = QStringLiteral("bin");
    const QString localName = QStringLiteral("part_%1.%2")
                                  .arg(m_totalResources, 6, 10, QLatin1Char('0')).arg(suffix);
    const QString path = QDir(m_outputDirectory).filePath(localName);
    m_resourcePaths.insert(key, localName);
    m_resourceQueue.enqueue(Resource{url, path, rangeStart, rangeLength, 0});
    ++m_totalResources;
    return localName;
}

void HlsDownloader::pumpDownloads()
{
    if (!m_active || m_cancelRequested) return;
    while (m_resourceReplies.size() < kMaxConcurrency && !m_resourceQueue.isEmpty()) {
        const Resource resource = m_resourceQueue.dequeue();
        QNetworkRequest request = requestFor(resource.url);
        if (resource.rangeLength > 0) {
            request.setRawHeader("Range", QStringLiteral("bytes=%1-%2")
                .arg(resource.rangeStart)
                .arg(resource.rangeStart + resource.rangeLength - 1).toUtf8());
        }
        QNetworkReply *reply = m_network.get(request);
        m_resourceReplies.insert(reply, resource);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply] { handleResourceReply(reply); });
    }
    finishIfReady();
}

void HlsDownloader::handleResourceReply(QNetworkReply *reply)
{
    if (!m_resourceReplies.contains(reply)) {
        reply->deleteLater();
        return;
    }
    Resource resource = m_resourceReplies.take(reply);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error();
    const QString networkMessage = reply->errorString();
    QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (!m_active || m_cancelRequested) return;
    if (networkError != QNetworkReply::NoError || status < 200 || status >= 400) {
        if (resource.retryCount < 2) {
            ++resource.retryCount;
            m_resourceQueue.prepend(resource);
            pumpDownloads();
            return;
        }
        fail(MNET_TEXT("视频分片下载失败（HTTP %1）：%2")
                 .arg(status).arg(networkMessage));
        return;
    }
    if (resource.rangeLength > 0) {
        if (status == 200 && payload.size() >= resource.rangeStart + resource.rangeLength) {
            payload = payload.mid(resource.rangeStart, resource.rangeLength);
        }
        if (payload.size() != resource.rangeLength) {
            fail(MNET_TEXT("视频分片字节范围长度不匹配"));
            return;
        }
    }
    if (payload.isEmpty()) {
        fail(MNET_TEXT("视频分片内容为空"));
        return;
    }

    QSaveFile file(resource.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()
        || !file.commit()) {
        fail(MNET_TEXT("视频分片写入失败：%1").arg(resource.path));
        return;
    }
    ++m_completedResources;
    emit progressChanged(m_completedResources, m_totalResources);
    pumpDownloads();
}

void HlsDownloader::finishIfReady()
{
    if (!m_active || m_cancelRequested || !m_resourceQueue.isEmpty()
        || !m_resourceReplies.isEmpty()) {
        return;
    }
    QSaveFile playlist(m_playlistPath);
    if (!playlist.open(QIODevice::WriteOnly | QIODevice::Text)) {
        fail(MNET_TEXT("无法写入本地 HLS 清单"));
        return;
    }
    const QByteArray content = m_playlistLines.join(QLatin1Char('\n')).toUtf8();
    if (playlist.write(content) != content.size() || !playlist.commit()) {
        fail(MNET_TEXT("本地 HLS 清单写入失败"));
        return;
    }
    m_active = false;
    emit completed(m_playlistPath, m_expectedHeight);
}

void HlsDownloader::cancel()
{
    if (!m_active) return;
    m_cancelRequested = true;
    if (m_manifestReply) {
        m_manifestReply->abort();
        m_manifestReply->deleteLater();
        m_manifestReply = nullptr;
    }
    const QList<QNetworkReply *> replies = m_resourceReplies.keys();
    m_resourceReplies.clear();
    for (QNetworkReply *reply : replies) {
        reply->abort();
        reply->deleteLater();
    }
    m_resourceQueue.clear();
    m_active = false;
}

bool HlsDownloader::isActive() const
{
    return m_active;
}

void HlsDownloader::fail(const QString &message)
{
    if (!m_active) return;
    cancel();
    emit failed(message);
}

QNetworkRequest HlsDownloader::requestFor(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "*/*");
    request.setRawHeader("Referer", m_referer.toUtf8());
    request.setTransferTimeout(30000);
    return request;
}
