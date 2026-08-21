#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QObject>
#include <QStringList>
#include <QUrl>

struct MediaInfo
{
    QString pageUrl;
    QString videoId;
    QString title;
    QString publishDate;
    QString videoUrl;
    QString audioUrl;
    QString captionId;
    QStringList captionLanguages;
    QString filenameTemplate;
    double durationSecond = 0.0;
    bool previewOnly = false;
    bool audioDerived = false;
};

Q_DECLARE_METATYPE(MediaInfo)

class MnetResolver final : public QObject
{
    Q_OBJECT

public:
    explicit MnetResolver(QObject *parent = nullptr);

    void setCookies(const QList<QNetworkCookie> &cookies);
    void resolve(const QUrl &pageUrl);
    void cancel();
    static MediaInfo parsePage(const QString &html, const QUrl &pageUrl, QString *error);

signals:
    void resolved(const MediaInfo &media);
    void failed(const QString &message);

private:
    void fetchCaptionList(const MediaInfo &media);
    QByteArray accessToken() const;

    QNetworkAccessManager m_network;
    QNetworkReply *m_reply = nullptr;
    QNetworkReply *m_captionListReply = nullptr;
    MediaInfo m_pendingMedia;
};
