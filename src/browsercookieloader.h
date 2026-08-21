#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QObject>
#include <QFutureWatcher>
#include <QUrl>

struct CookieLoadResult
{
    QList<QNetworkCookie> cookies;
    QString browser;
    QString error;
};

class BrowserCookieLoader final : public QObject
{
    Q_OBJECT

public:
    explicit BrowserCookieLoader(QObject *parent = nullptr);
    ~BrowserCookieLoader() override;

    void load(const QUrl &pageUrl, const QString &browser);
    void cancel();

signals:
    void loadingBrowser(const QString &browser);
    void loaded(const QList<QNetworkCookie> &cookies, const QString &browser);
    void unavailable(const QString &reason);

private:
    static CookieLoadResult loadSync(const QString &requestedBrowser);
    static CookieLoadResult loadChromium(const QString &browser);
    static QStringList automaticCandidates();

    QFutureWatcher<CookieLoadResult> m_watcher;
    bool m_ignoreResult = false;
};
