#include "mnetresolver.h"
#include "localization.h"

#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>

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

QString decodeHtml(const QString &source)
{
    QTextDocument document;
    document.setHtml(QStringLiteral("<span>%1</span>").arg(source));
    return document.toPlainText().trimmed();
}

QString metaContent(const QString &html, const QString &property)
{
    static const QRegularExpression tagExpression(
        QStringLiteral(R"(<meta\b[^>]*>)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression contentExpression(
        QStringLiteral(R"(\bcontent\s*=\s*["']([^"']*)["'])"),
        QRegularExpression::CaseInsensitiveOption);

    auto matchIterator = tagExpression.globalMatch(html);
    while (matchIterator.hasNext()) {
        const QString tag = matchIterator.next().captured(0);
        const QRegularExpression propertyExpression(
            QStringLiteral(R"(\b(?:property|name)\s*=\s*["']%1["'])")
                .arg(QRegularExpression::escape(property)),
            QRegularExpression::CaseInsensitiveOption);
        if (!propertyExpression.match(tag).hasMatch()) {
            continue;
        }
        const auto contentMatch = contentExpression.match(tag);
        if (contentMatch.hasMatch()) {
            return decodeHtml(contentMatch.captured(1));
        }
    }
    return {};
}

QString normalizedPage(QString html)
{
    html.replace(QStringLiteral("\\u0026"), QStringLiteral("&"), Qt::CaseInsensitive);
    html.replace(QStringLiteral("\\u003d"), QStringLiteral("="), Qt::CaseInsensitive);
    html.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    html.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    return html;
}

QStringList findUrls(const QString &source, const QRegularExpression &expression)
{
    QStringList urls;
    QSet<QString> seen;
    auto iterator = expression.globalMatch(source);
    while (iterator.hasNext()) {
        QString url = iterator.next().captured(0);
        url.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        if (QUrl(url).isValid() && !seen.contains(url)) {
            seen.insert(url);
            urls.append(url);
        }
    }
    return urls;
}

int hlsScore(const QString &url)
{
    int score = 0;
    if (url.contains(QStringLiteral("/converted/"), Qt::CaseInsensitive)) score += 600;
    if (url.contains(QStringLiteral("1080"), Qt::CaseInsensitive)) score += 180;
    if (url.contains(QStringLiteral("2160"), Qt::CaseInsensitive)) score += 220;
    if (url.contains(QStringLiteral("720"), Qt::CaseInsensitive)) score += 100;
    if (url.contains(QStringLiteral("master.m3u8"), Qt::CaseInsensitive)) score += 80;
    if (url.contains(QStringLiteral("/preview/"), Qt::CaseInsensitive)) score -= 500;
    return score;
}

QString bestHls(const QStringList &urls)
{
    QString best;
    int bestScore = std::numeric_limits<int>::min();
    for (const QString &url : urls) {
        const int score = hlsScore(url);
        if (score > bestScore) {
            best = url;
            bestScore = score;
        }
    }
    return best;
}

bool looksLikeAudioHls(const QString &url)
{
    return url.contains(QRegularExpression(
        QStringLiteral(R"((?:^|[/_.-])audio(?:[/_.-]|$))"),
        QRegularExpression::CaseInsensitiveOption));
}

QString bestAudio(const QStringList &urls)
{
    QString best;
    int bestScore = std::numeric_limits<int>::min();
    for (const QString &url : urls) {
        int score = 0;
        if (url.contains(QStringLiteral("/converted/"), Qt::CaseInsensitive)) score += 150;
        if (looksLikeAudioHls(url)) score += 500;
        if (url.contains(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) score += 300;
        if (url.contains(QRegularExpression(
                QStringLiteral(R"((?:^|[/_.-])(?:segment|chunk|init)(?:[/_.-]|$))"),
                QRegularExpression::CaseInsensitiveOption))) {
            score -= 1000;
        }
        if (score > bestScore) {
            best = url;
            bestScore = score;
        }
    }
    return bestScore < 0 ? QString{} : best;
}

QString derivedAudioUrl(const QString &videoUrl)
{
    QUrl audio(videoUrl);
    QString path = audio.path();
    static const QRegularExpression videoSuffixExpression(
        QStringLiteral(R"(_\d{3,4}pw.*$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (!path.contains(videoSuffixExpression)) return {};
    path.replace(videoSuffixExpression, QStringLiteral("_audio.cmfa"));
    audio.setPath(path);
    return audio.toString(QUrl::FullyEncoded);
}

QString extractPublishDate(const QString &normalized)
{
    static const QRegularExpression dateValueExpression(
        QStringLiteral(R"REGEX("(?:uploadDate|datePublished|publishedAt|releaseDate|createdAt|openDate)"\s*:\s*"(\d{4}-\d{2}-\d{2}))REGEX"),
        QRegularExpression::CaseInsensitiveOption);
    return dateValueExpression.match(normalized).captured(1);
}

QByteArray preferredLanguages()
{
    const QString language = AppLocale::languageCode();
    if (language == QStringLiteral("en")) return "en-US,en;q=0.9";
    if (language == QStringLiteral("ja")) return "ja-JP,ja;q=0.9,en;q=0.8";
    if (language == QStringLiteral("ko")) return "ko-KR,ko;q=0.9,en;q=0.8";
    return "zh-CN,zh;q=0.9,en;q=0.8,ja;q=0.7";
}
}

MnetResolver::MnetResolver(QObject *parent)
    : QObject(parent)
{
    m_network.setCookieJar(new QNetworkCookieJar(&m_network));
}

void MnetResolver::setCookies(const QList<QNetworkCookie> &cookies)
{
    auto *jar = new QNetworkCookieJar(&m_network);
    m_network.setCookieJar(jar);
    for (const QNetworkCookie &cookie : cookies) {
        QString host = cookie.domain();
        if (host.startsWith(QLatin1Char('.'))) host.remove(0, 1);
        jar->setCookiesFromUrl({cookie}, QUrl(QStringLiteral("https://%1/").arg(host)));
    }
}

void MnetResolver::resolve(const QUrl &pageUrl)
{
    const QString host = pageUrl.host().toLower();
    if (!pageUrl.isValid()
        || pageUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || !(host == QStringLiteral("mnetplus.world")
             || host.endsWith(QStringLiteral(".mnetplus.world")))) {
        emit failed(MNET_TEXT("请输入有效的 mnetplus.world 视频页面地址"));
        return;
    }

    static const QRegularExpression videoIdExpression(
        QStringLiteral(R"(/videos/([0-9a-fA-F]{24})(?:/|$))"));
    if (!videoIdExpression.match(pageUrl.path()).hasMatch()) {
        emit failed(MNET_TEXT("地址中没有找到 Mnet Plus 视频 ID"));
        return;
    }

    QNetworkRequest request(pageUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Language", preferredLanguages());
    request.setTransferTimeout(30000);

    cancel();
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, pageUrl] {
        if (reply != m_reply) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        const auto networkError = reply->error();
        const QString networkMessage = reply->errorString();
        reply->deleteLater();

        if (networkError != QNetworkReply::NoError || status >= 400) {
            if (status == 401 || status == 403) {
                emit failed(MNET_TEXT("站点拒绝访问（HTTP %1），请确认浏览器已登录 Mnet Plus").arg(status));
            } else {
                emit failed(MNET_TEXT("页面请求失败：%1").arg(networkMessage));
            }
            return;
        }

        QString error;
        const MediaInfo media = parsePage(QString::fromUtf8(payload), pageUrl, &error);
        if (!error.isEmpty()) {
            emit failed(error);
            return;
        }
        if (media.captionId.isEmpty() || media.captionLanguages.isEmpty()) {
            fetchCaptionList(media);
        } else {
            emit resolved(media);
        }
    });
}

void MnetResolver::cancel()
{
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
    if (m_captionListReply) {
        m_captionListReply->abort();
        m_captionListReply->deleteLater();
        m_captionListReply = nullptr;
    }
}

QByteArray MnetResolver::accessToken() const
{
    const QList<QNetworkCookie> cookies = m_network.cookieJar()->cookiesForUrl(
        QUrl(QStringLiteral("https://api.mnetplus.world/")));
    for (const QNetworkCookie &cookie : cookies) {
        if (cookie.name() == QByteArray("_mnet_atk")) return cookie.value();
    }
    return {};
}

void MnetResolver::fetchCaptionList(const MediaInfo &media)
{
    m_pendingMedia = media;
    QUrl url(QStringLiteral("https://api.mnetplus.world/media/v1/public/videos/%1/captions")
                 .arg(media.videoId));
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Origin", "https://www.mnetplus.world");
    request.setRawHeader("Referer", media.pageUrl.toUtf8());
    const QByteArray token = accessToken();
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    request.setTransferTimeout(30000);
    m_captionListReply = m_network.get(request);
    connect(m_captionListReply, &QNetworkReply::finished, this, [this] {
        if (!m_captionListReply) return;
        QNetworkReply *reply = m_captionListReply;
        m_captionListReply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        reply->deleteLater();

        if (status >= 200 && status < 300) {
            QString discoveredCaptionId;
            QSet<QString> languages;
            const std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &value) {
                if (value.isObject()) {
                    const QJsonObject object = value.toObject();
                    const QString captionId = object.value(QStringLiteral("captionId")).toString();
                    if (!captionId.isEmpty()) discoveredCaptionId = captionId;
                    const QString language = object.value(QStringLiteral("language")).toString();
                    if (!language.isEmpty()) languages.insert(language);
                    for (auto it = object.constBegin(); it != object.constEnd(); ++it) walk(it.value());
                } else if (value.isArray()) {
                    for (const QJsonValue &item : value.toArray()) walk(item);
                }
            };
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error == QJsonParseError::NoError) walk(document.object());

            if (!discoveredCaptionId.isEmpty()) m_pendingMedia.captionId = discoveredCaptionId;
            if (!languages.isEmpty()) {
                QStringList ordered = languages.values();
                ordered.sort(Qt::CaseInsensitive);
                m_pendingMedia.captionLanguages = ordered;
            }
        }
        emit resolved(m_pendingMedia);
    });
}

MediaInfo MnetResolver::parsePage(const QString &html, const QUrl &pageUrl, QString *error)
{
    MediaInfo media;
    media.pageUrl = pageUrl.toString(QUrl::FullyEncoded);
    static const QRegularExpression videoIdExpression(
        QStringLiteral(R"(/videos/([0-9a-fA-F]{24})(?:/|$))"));
    media.videoId = videoIdExpression.match(pageUrl.path()).captured(1);

    media.title = metaContent(html, QStringLiteral("og:title"));
    if (media.title.isEmpty()) {
        const QRegularExpression titleExpression(
            QStringLiteral(R"(<title[^>]*>(.*?)</title>)"),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        media.title = decodeHtml(titleExpression.match(html).captured(1));
    }
    if (media.title.isEmpty()) media.title = media.videoId;

    const QString normalized = normalizedPage(html);
    const QRegularExpression isoDurationExpression(
        QStringLiteral(R"REGEX("duration"\s*:\s*"PT(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?")REGEX"),
        QRegularExpression::CaseInsensitiveOption);
    const auto durationMatch = isoDurationExpression.match(normalized);
    if (durationMatch.hasMatch()) {
        media.durationSecond = durationMatch.captured(1).toDouble() * 3600.0
            + durationMatch.captured(2).toDouble() * 60.0
            + durationMatch.captured(3).toDouble();
    }
    if (media.durationSecond <= 0.0) {
        const QRegularExpression numericDurationExpression(
            QStringLiteral(R"REGEX("(?:durationSecond|videoDuration)"\s*:\s*(\d+(?:\.\d+)?))REGEX"),
            QRegularExpression::CaseInsensitiveOption);
        const auto numericMatch = numericDurationExpression.match(normalized);
        if (numericMatch.hasMatch()) media.durationSecond = numericMatch.captured(1).toDouble();
    }
    const QRegularExpression hlsExpression(
        QStringLiteral(R"(https://video\.cdn\.mnetplus\.world/[^\s"'<>\\]+?\.m3u8(?:\?[^\s"'<>\\]+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression audioExpression(
        QStringLiteral(R"(https://video\.cdn\.mnetplus\.world/[^\s"'<>\\]+?\.(?:cmfa|m4a|aac)(?:\?[^\s"'<>\\]+)?)"),
        QRegularExpression::CaseInsensitiveOption);

    const QStringList allHlsUrls = findUrls(normalized, hlsExpression);
    QStringList videoHlsUrls;
    QStringList audioUrls = findUrls(normalized, audioExpression);
    for (const QString &url : allHlsUrls) {
        if (looksLikeAudioHls(url)) {
            audioUrls.append(url);
        } else {
            videoHlsUrls.append(url);
        }
    }
    media.videoUrl = bestHls(videoHlsUrls);
    media.audioUrl = bestAudio(audioUrls);
    if (media.audioUrl.isEmpty()) {
        media.audioUrl = derivedAudioUrl(media.videoUrl);
        media.audioDerived = !media.audioUrl.isEmpty();
    }
    media.previewOnly = media.videoUrl.contains(QStringLiteral("/preview/"), Qt::CaseInsensitive);
    media.publishDate = extractPublishDate(normalized);

    QRegularExpression captionUrlExpression(
        QStringLiteral(R"(/captions/([A-Za-z0-9_-]{12,})/cues)"),
        QRegularExpression::CaseInsensitiveOption);
    auto captionMatch = captionUrlExpression.match(normalized);
    if (captionMatch.hasMatch()) {
        media.captionId = captionMatch.captured(1);
    } else {
        QRegularExpression captionIdExpression(
            QStringLiteral(R"REGEX("(?:videoCaptionId|captionId)"\s*:\s*"([A-Za-z0-9_-]{12,})")REGEX"),
            QRegularExpression::CaseInsensitiveOption);
        captionMatch = captionIdExpression.match(normalized);
        if (captionMatch.hasMatch()) media.captionId = captionMatch.captured(1);
    }

    QString languageArea = normalized;
    const int captionIndex = normalized.indexOf(QStringLiteral("videoCaptionId"));
    if (captionIndex >= 0) languageArea = normalized.mid(captionIndex, 12000);
    const QRegularExpression languageExpression(
        QStringLiteral(R"REGEX("language"\s*:\s*"([a-z]{2}(?:[-_][A-Za-z]{2})?)")REGEX"));
    QSet<QString> languages;
    auto languageIterator = languageExpression.globalMatch(languageArea);
    while (languageIterator.hasNext()) {
        languages.insert(languageIterator.next().captured(1));
    }
    media.captionLanguages = languages.values();
    media.captionLanguages.sort(Qt::CaseInsensitive);

    if (media.videoUrl.isEmpty()) {
        *error = MNET_TEXT("页面中没有找到可公开访问的 HLS 视频流；内容可能需要登录、购买或受 DRM 保护");
    }
    return media;
}
