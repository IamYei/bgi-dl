#pragma once

#include "browsercookieloader.h"
#include "downloadcontroller.h"
#include "mnetresolver.h"

#include <QMainWindow>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QToolButton;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void connectSignals();
    void applyTheme();
    void beginResolve();
    void resolveAsGuest(const QString &reason);
    void showMedia(const MediaInfo &media);
    void populateMediaDetails(const MediaInfo &media);
    void setBusy(bool busy);
    void appendLog(const QString &message);
    void chooseOutputDirectory();
    void startDownload();
    void startBatchDownload();
    void beginBatchResolve();
    void resolveNextBatchUrl();
    void beginBatchDownloadPhase();
    void startNextBatchItem();
    void resetMediaDisplay();
    void changeLanguage(int index);
    void retranslateUi();
    void setCookieStatus(const QString &source, const QStringList &arguments = {});
    QString browserDisplayName(const QString &browser) const;
    QString currentFilenameTemplate() const;
    void setStreamRow(QLabel *stateLabel, QLabel *valueLabel,
                      const QString &state, const QString &value,
                      const QString &stateStyle);

    BrowserCookieLoader m_cookieLoader;
    MnetResolver m_resolver;
    DownloadController m_downloader;
    MediaInfo m_media;
    QUrl m_pendingUrl;
    bool m_hasMedia = false;
    bool m_resolving = false;
    QString m_cookieStatusSource;
    QStringList m_cookieStatusArguments;

    QLineEdit *m_urlEdit = nullptr;
    QPushButton *m_resolveButton = nullptr;
    QComboBox *m_browserCombo = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QLabel *m_cookieStatus = nullptr;
    QLabel *m_languageLabel = nullptr;
    QLabel *m_heading = nullptr;
    QLabel *m_videoPageSection = nullptr;
    QLabel *m_mediaInfoSection = nullptr;
    QLabel *m_batchSection = nullptr;
    QLabel *m_outputSection = nullptr;
    QLabel *m_videoName = nullptr;
    QLabel *m_audioName = nullptr;
    QLabel *m_captionName = nullptr;
    QLabel *m_captionHint = nullptr;
    QLabel *m_filenameTemplateLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_videoState = nullptr;
    QLabel *m_videoValue = nullptr;
    QLabel *m_audioState = nullptr;
    QLabel *m_audioValue = nullptr;
    QLabel *m_captionState = nullptr;
    QLabel *m_captionValue = nullptr;
    QLineEdit *m_outputEdit = nullptr;
    QToolButton *m_outputButton = nullptr;
    QCheckBox *m_captionCheck = nullptr;
    QCheckBox *m_srtCheck = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QToolButton *m_cancelButton = nullptr;
    QLineEdit *m_filenameTemplateEdit = nullptr;
    QToolButton *m_templateResetButton = nullptr;
    QPlainTextEdit *m_batchEdit = nullptr;
    QPushButton *m_batchButton = nullptr;
    QLabel *m_stageLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QStringList m_batchUrls;
    QList<MediaInfo> m_batchMedia;
    int m_batchResolveIndex = 0;
    bool m_batchActive = false;
    bool m_batchDownloading = false;
};
