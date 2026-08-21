#include "mainwindow.h"
#include "localization.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMap>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace {
QLabel *sectionLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionLabel"));
    return label;
}

QFrame *divider()
{
    auto *line = new QFrame;
    line->setObjectName(QStringLiteral("divider"));
    line->setFrameShape(QFrame::HLine);
    return line;
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    const QSettings settings;
    const QString languageOverride = qEnvironmentVariable("MNETPLUS_LANGUAGE");
    const QString savedLanguage = languageOverride.isEmpty()
        ? settings.value(QStringLiteral("language"), AppLocale::defaultLanguageCode()).toString()
        : languageOverride;
    AppLocale::setLanguage(savedLanguage);
    buildUi();
    applyTheme();
    connectSignals();
    resetMediaDisplay();

    QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (defaultPath.isEmpty()) defaultPath = QDir::homePath();
    m_outputEdit->setText(settings.value(QStringLiteral("outputDirectory"), defaultPath).toString());
    m_filenameTemplateEdit->setText(settings.value(
        QStringLiteral("filenameTemplate"), DownloadController::defaultFilenameTemplate()).toString());
    appendLog(MNET_TEXT("就绪"));
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Mnet Plus Downloader"));
    resize(1160, 960);
    setMinimumSize(920, 800);

    auto *central = new QWidget;
    central->setObjectName(QStringLiteral("central"));
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(40, 30, 40, 32);
    root->setSpacing(20);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);
    auto *brandColumn = new QVBoxLayout;
    brandColumn->setSpacing(0);
    auto *eyebrow = new QLabel(QStringLiteral("MNET PLUS"));
    eyebrow->setObjectName(QStringLiteral("eyebrow"));
    m_heading = new QLabel;
    m_heading->setObjectName(QStringLiteral("heading"));
    brandColumn->addWidget(eyebrow);
    brandColumn->addWidget(m_heading);
    header->addLayout(brandColumn);
    header->addStretch();

    auto *languageRow = new QHBoxLayout;
    languageRow->setSpacing(8);
    m_languageLabel = new QLabel;
    m_languageLabel->setObjectName(QStringLiteral("languageLabel"));
    languageRow->addWidget(m_languageLabel);
    m_languageCombo = new QComboBox;
    m_languageCombo->setMinimumHeight(38);
    m_languageCombo->setAccessibleName(QStringLiteral("Language"));
    m_languageCombo->addItem(QStringLiteral("简体中文"), QStringLiteral("zh_CN"));
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_languageCombo->addItem(QStringLiteral("日本語"), QStringLiteral("ja"));
    m_languageCombo->addItem(QStringLiteral("한국어"), QStringLiteral("ko"));
    languageRow->addWidget(m_languageCombo);
    header->addLayout(languageRow);

    m_cookieStatus = new QLabel;
    m_cookieStatus->setObjectName(QStringLiteral("sessionStatus"));
    m_cookieStatus->setAlignment(Qt::AlignCenter);
    header->addWidget(m_cookieStatus, 0, Qt::AlignTop);
    root->addLayout(header);

    m_videoPageSection = sectionLabel(QString());
    root->addWidget(m_videoPageSection);
    auto *urlRow = new QHBoxLayout;
    urlRow->setSpacing(10);
    m_urlEdit = new QLineEdit;
    m_urlEdit->setPlaceholderText(QStringLiteral("https://www.mnetplus.world/media/en/videos/..."));
    m_urlEdit->setClearButtonEnabled(true);
    m_urlEdit->setMinimumHeight(46);
    m_urlEdit->setAccessibleName(MNET_TEXT("Mnet Plus 视频页面地址"));
    urlRow->addWidget(m_urlEdit, 1);

    m_browserCombo = new QComboBox;
    m_browserCombo->setMinimumHeight(46);
    m_browserCombo->setAccessibleName(MNET_TEXT("浏览器会话来源"));
    m_browserCombo->addItem(MNET_TEXT("自动读取会话"), QStringLiteral("auto"));
    m_browserCombo->addItem(QStringLiteral("Chrome"), QStringLiteral("chrome"));
    m_browserCombo->addItem(QStringLiteral("Edge"), QStringLiteral("edge"));
    urlRow->addWidget(m_browserCombo);

    m_resolveButton = new QPushButton;
    m_resolveButton->setObjectName(QStringLiteral("secondaryButton"));
    m_resolveButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_resolveButton->setMinimumSize(104, 46);
    urlRow->addWidget(m_resolveButton);
    root->addLayout(urlRow);

    root->addWidget(divider());
    m_mediaInfoSection = sectionLabel(QString());
    root->addWidget(m_mediaInfoSection);
    m_titleLabel = new QLabel;
    m_titleLabel->setObjectName(QStringLiteral("mediaTitle"));
    m_titleLabel->setWordWrap(true);
    m_titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_titleLabel);

    auto *streamGrid = new QGridLayout;
    streamGrid->setHorizontalSpacing(12);
    streamGrid->setVerticalSpacing(10);
    streamGrid->setColumnStretch(2, 1);
    const QStringList names = {QString(), QString(), QString()};
    QList<QLabel **> states = {&m_videoState, &m_audioState, &m_captionState};
    QList<QLabel **> values = {&m_videoValue, &m_audioValue, &m_captionValue};
    for (int row = 0; row < names.size(); ++row) {
        auto *name = new QLabel;
        name->setObjectName(QStringLiteral("streamName"));
        if (row == 0) m_videoName = name;
        if (row == 1) m_audioName = name;
        if (row == 2) m_captionName = name;
        *states.at(row) = new QLabel;
        (*states.at(row))->setAlignment(Qt::AlignCenter);
        (*states.at(row))->setMinimumSize(104, 24);
        *values.at(row) = new QLabel;
        (*values.at(row))->setObjectName(QStringLiteral("streamValue"));
        (*values.at(row))->setTextInteractionFlags(Qt::TextSelectableByMouse);
        (*values.at(row))->setWordWrap(true);
        streamGrid->addWidget(name, row, 0, Qt::AlignTop);
        streamGrid->addWidget(*states.at(row), row, 1, Qt::AlignTop);
        streamGrid->addWidget(*values.at(row), row, 2);
    }
    root->addLayout(streamGrid);

    root->addWidget(divider());
    auto *batchHeader = new QHBoxLayout;
    m_batchSection = sectionLabel(QString());
    batchHeader->addWidget(m_batchSection);
    batchHeader->addStretch();
    m_batchButton = new QPushButton;
    m_batchButton->setObjectName(QStringLiteral("secondaryButton"));
    m_batchButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_batchButton->setMinimumSize(120, 38);
    batchHeader->addWidget(m_batchButton);
    root->addLayout(batchHeader);
    m_batchEdit = new QPlainTextEdit;
    m_batchEdit->setObjectName(QStringLiteral("batchEdit"));
    m_batchEdit->setPlaceholderText(QString());
    m_batchEdit->setMaximumHeight(84);
    m_batchEdit->setAccessibleName(MNET_TEXT("批量视频地址"));
    root->addWidget(m_batchEdit);

    root->addWidget(divider());
    m_outputSection = sectionLabel(QString());
    root->addWidget(m_outputSection);

    auto *captionRow = new QHBoxLayout;
    captionRow->setSpacing(10);
    m_captionCheck = new QCheckBox;
    m_captionCheck->setChecked(true);
    m_captionCheck->setMinimumHeight(36);
    captionRow->addWidget(m_captionCheck);
    m_srtCheck = new QCheckBox;
    m_srtCheck->setChecked(false);
    m_srtCheck->setMinimumHeight(36);
    captionRow->addWidget(m_srtCheck);
    captionRow->addStretch();
    m_captionHint = new QLabel;
    m_captionHint->setObjectName(QStringLiteral("progressText"));
    captionRow->addWidget(m_captionHint);
    root->addLayout(captionRow);

    auto *outputRow = new QHBoxLayout;
    outputRow->setSpacing(10);
    m_outputEdit = new QLineEdit;
    m_outputEdit->setMinimumHeight(44);
    m_outputEdit->setAccessibleName(MNET_TEXT("输出目录"));
    outputRow->addWidget(m_outputEdit, 1);
    m_outputButton = new QToolButton;
    m_outputButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_outputButton->setToolTip(MNET_TEXT("选择输出目录"));
    m_outputButton->setAccessibleName(MNET_TEXT("选择输出目录"));
    m_outputButton->setMinimumSize(44, 44);
    outputRow->addWidget(m_outputButton);
    root->addLayout(outputRow);

    auto *templateRow = new QHBoxLayout;
    templateRow->setSpacing(10);
    m_filenameTemplateLabel = new QLabel;
    m_filenameTemplateLabel->setObjectName(QStringLiteral("streamName"));
    templateRow->addWidget(m_filenameTemplateLabel);
    m_filenameTemplateEdit = new QLineEdit;
    m_filenameTemplateEdit->setMinimumHeight(42);
    m_filenameTemplateEdit->setToolTip(
        MNET_TEXT("可用占位符：{date} {title} {res} {resolution} {codec} {tag} {ext}"));
    m_filenameTemplateEdit->setAccessibleName(MNET_TEXT("文件名模板"));
    templateRow->addWidget(m_filenameTemplateEdit, 1);
    m_templateResetButton = new QToolButton;
    m_templateResetButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_templateResetButton->setToolTip(MNET_TEXT("恢复默认模板"));
    m_templateResetButton->setAccessibleName(MNET_TEXT("恢复默认模板"));
    m_templateResetButton->setMinimumSize(42, 42);
    templateRow->addWidget(m_templateResetButton);
    root->addLayout(templateRow);

    auto *actionRow = new QHBoxLayout;
    actionRow->setSpacing(10);
    m_downloadButton = new QPushButton;
    m_downloadButton->setObjectName(QStringLiteral("primaryButton"));
    m_downloadButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_downloadButton->setMinimumHeight(50);
    actionRow->addWidget(m_downloadButton, 1);
    m_cancelButton = new QToolButton;
    m_cancelButton->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    m_cancelButton->setToolTip(MNET_TEXT("取消当前任务"));
    m_cancelButton->setAccessibleName(MNET_TEXT("取消当前任务"));
    m_cancelButton->setMinimumSize(50, 50);
    m_cancelButton->setEnabled(false);
    actionRow->addWidget(m_cancelButton);
    root->addLayout(actionRow);

    auto *progressHeader = new QHBoxLayout;
    m_stageLabel = new QLabel;
    m_stageLabel->setObjectName(QStringLiteral("stageLabel"));
    m_progressLabel = new QLabel;
    m_progressLabel->setObjectName(QStringLiteral("progressText"));
    m_progressLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progressHeader->addWidget(m_stageLabel);
    progressHeader->addStretch();
    progressHeader->addWidget(m_progressLabel);
    root->addLayout(progressHeader);

    m_progressBar = new QProgressBar;
    m_progressBar->setTextVisible(false);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(5);
    root->addWidget(m_progressBar);

    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("logView"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(500);
    m_log->setMinimumHeight(96);
    m_log->setMaximumHeight(132);
    m_log->setAccessibleName(MNET_TEXT("任务日志"));
    root->addWidget(m_log);

    auto *focusUrl = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(focusUrl, &QShortcut::activated, m_urlEdit, qOverload<>(&QWidget::setFocus));
    connect(m_languageCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::changeLanguage);
    retranslateUi();
}

void MainWindow::retranslateUi()
{
    setWindowTitle(QStringLiteral("Mnet Plus Downloader"));
    if (!m_heading) return;
    m_heading->setText(MNET_TEXT("视频下载器"));
    m_languageLabel->setText(MNET_TEXT("语言"));
    m_cookieStatus->setText(m_cookieStatusSource.isEmpty()
        ? MNET_TEXT("浏览器会话：等待连接")
        : MNET_TEXT(m_cookieStatusSource.toUtf8().constData()));
    if (!m_cookieStatusArguments.isEmpty()) {
        QString translated = MNET_TEXT(m_cookieStatusSource.toUtf8().constData());
        for (const QString &argument : m_cookieStatusArguments) translated = translated.arg(argument);
        m_cookieStatus->setText(translated);
    }
    m_videoPageSection->setText(MNET_TEXT("视频页面"));
    m_urlEdit->setPlaceholderText(QStringLiteral("https://www.mnetplus.world/media/en/videos/..."));
    m_browserCombo->setItemText(0, MNET_TEXT("自动读取会话"));
    m_resolveButton->setText(MNET_TEXT("解析"));
    m_mediaInfoSection->setText(MNET_TEXT("媒体信息"));
    m_videoName->setText(MNET_TEXT("视频"));
    m_audioName->setText(MNET_TEXT("音频"));
    m_captionName->setText(MNET_TEXT("字幕"));
    m_batchSection->setText(MNET_TEXT("批量下载"));
    m_batchButton->setText(MNET_TEXT("批量下载"));
    m_batchEdit->setPlaceholderText(MNET_TEXT("每行一个视频页面地址，可一次粘贴多个"));
    m_outputSection->setText(MNET_TEXT("输出设置"));
    m_captionCheck->setText(MNET_TEXT("内封字幕"));
    m_srtCheck->setText(MNET_TEXT("单独导出 SRT"));
    m_captionHint->setText(MNET_TEXT("将下载全部可用字幕"));
    m_outputButton->setToolTip(MNET_TEXT("选择输出目录"));
    m_outputButton->setAccessibleName(MNET_TEXT("选择输出目录"));
    m_filenameTemplateLabel->setText(MNET_TEXT("文件名模板"));
    m_filenameTemplateEdit->setToolTip(MNET_TEXT("可用占位符：{date} {title} {res} {resolution} {codec} {tag} {ext}"));
    m_templateResetButton->setToolTip(MNET_TEXT("恢复默认模板"));
    m_templateResetButton->setAccessibleName(MNET_TEXT("恢复默认模板"));
    m_downloadButton->setText(MNET_TEXT("下载并合并 MKV"));
    m_cancelButton->setToolTip(MNET_TEXT("取消当前任务"));
    m_cancelButton->setAccessibleName(MNET_TEXT("取消当前任务"));
    if (!m_batchActive && !m_downloader.isActive() && !m_resolving) {
        if (m_hasMedia) {
            populateMediaDetails(m_media);
            m_stageLabel->setText(MNET_TEXT("解析完成"));
            m_progressLabel->setText(m_media.previewOnly
                ? MNET_TEXT("当前会话只返回预览流") : MNET_TEXT("可以开始下载"));
        } else {
            resetMediaDisplay();
            m_stageLabel->setText(MNET_TEXT("等待解析"));
        }
    }
    m_log->setAccessibleName(MNET_TEXT("任务日志"));
    const QString code = AppLocale::languageCode();
    const QSignalBlocker blocker(m_languageCombo);
    const int index = m_languageCombo->findData(code);
    if (index >= 0) m_languageCombo->setCurrentIndex(index);
}

void MainWindow::changeLanguage(int index)
{
    if (index < 0 || !m_languageCombo) return;
    const QString code = m_languageCombo->itemData(index).toString();
    if (!AppLocale::setLanguage(code)) return;
    QSettings().setValue(QStringLiteral("language"), code);
    retranslateUi();
    appendLog(MNET_TEXT("界面语言已切换为 %1").arg(m_languageCombo->currentText()));
}

void MainWindow::setCookieStatus(const QString &source, const QStringList &arguments)
{
    m_cookieStatusSource = source;
    m_cookieStatusArguments = arguments;
    QString translated = MNET_TEXT(source.toUtf8().constData());
    for (const QString &argument : arguments) translated = translated.arg(argument);
    m_cookieStatus->setText(translated);
}

void MainWindow::connectSignals()
{
    connect(m_resolveButton, &QPushButton::clicked, this, &MainWindow::beginResolve);
    connect(m_urlEdit, &QLineEdit::returnPressed, this, &MainWindow::beginResolve);
    connect(m_outputButton, &QToolButton::clicked, this, &MainWindow::chooseOutputDirectory);
    connect(m_downloadButton, &QPushButton::clicked, this, &MainWindow::startDownload);
    connect(m_batchButton, &QPushButton::clicked, this, &MainWindow::startBatchDownload);
    connect(m_outputEdit, &QLineEdit::editingFinished, this, [this] {
        const QString directory = m_outputEdit->text().trimmed();
        if (!directory.isEmpty()) QSettings().setValue(QStringLiteral("outputDirectory"), directory);
    });
    connect(m_filenameTemplateEdit, &QLineEdit::editingFinished, this, [this] {
        const QString templateString = m_filenameTemplateEdit->text().trimmed();
        if (!templateString.isEmpty()) {
            QSettings().setValue(QStringLiteral("filenameTemplate"), templateString);
        }
    });
    connect(m_templateResetButton, &QToolButton::clicked, this, [this] {
        m_filenameTemplateEdit->setText(DownloadController::defaultFilenameTemplate());
        QSettings().setValue(QStringLiteral("filenameTemplate"),
                             m_filenameTemplateEdit->text());
        appendLog(MNET_TEXT("文件名模板已恢复默认"));
    });
    connect(m_cancelButton, &QToolButton::clicked, this, [this] {
        if (m_downloader.isActive()) {
            m_downloader.cancel();
            return;
        }
        if (m_batchActive || m_batchDownloading) {
            m_batchActive = false;
            m_batchDownloading = false;
            m_batchMedia.clear();
            m_cookieLoader.cancel();
            m_resolver.cancel();
            setBusy(false);
            m_stageLabel->setText(MNET_TEXT("已取消"));
            m_progressLabel->clear();
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            appendLog(MNET_TEXT("批量任务已取消"));
            return;
        }
        if (m_resolving) {
            m_resolving = false;
            m_cookieLoader.cancel();
            m_resolver.cancel();
            setBusy(false);
            m_stageLabel->setText(MNET_TEXT("已取消"));
            m_progressLabel->clear();
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            appendLog(MNET_TEXT("解析已取消"));
        }
    });

    connect(&m_cookieLoader, &BrowserCookieLoader::loadingBrowser, this,
            [this](const QString &browser) {
        setCookieStatus(QStringLiteral("正在连接 %1"), {browserDisplayName(browser)});
        appendLog(MNET_TEXT("正在读取 %1 的 Mnet Plus 会话").arg(browserDisplayName(browser)));
    });
    connect(&m_cookieLoader, &BrowserCookieLoader::loaded, this,
            [this](const QList<QNetworkCookie> &cookies, const QString &browser) {
        setCookieStatus(QStringLiteral("已连接 %1"), {browserDisplayName(browser)});
        m_cookieStatus->setProperty("connected", true);
        m_cookieStatus->style()->unpolish(m_cookieStatus);
        m_cookieStatus->style()->polish(m_cookieStatus);
        appendLog(MNET_TEXT("%1 会话已连接，仅使用 Mnet Plus 域 Cookie")
                      .arg(browserDisplayName(browser)));
        m_resolver.setCookies(cookies);
        m_downloader.setCookies(cookies);
        if (m_batchActive) {
            beginBatchResolve();
        } else if (m_resolving) {
            m_resolver.resolve(m_pendingUrl);
        }
    });
    connect(&m_cookieLoader, &BrowserCookieLoader::unavailable, this,
            [this](const QString &reason) {
        if (m_batchActive) {
            appendLog(reason);
            m_resolver.setCookies({});
            m_downloader.setCookies({});
            beginBatchResolve();
            return;
        }
        resolveAsGuest(reason);
    });

    connect(&m_resolver, &MnetResolver::resolved, this, [this](const MediaInfo &media) {
        if (m_batchActive) {
            m_batchMedia.append(media);
            appendLog(MNET_TEXT("解析完成（%1/%2）：%3")
                          .arg(m_batchResolveIndex + 1).arg(m_batchUrls.size()).arg(media.title));
            ++m_batchResolveIndex;
            resolveNextBatchUrl();
            return;
        }
        showMedia(media);
    });
    connect(&m_resolver, &MnetResolver::failed, this, [this](const QString &message) {
        if (m_batchActive) {
            appendLog(MNET_TEXT("解析失败（%1/%2）：%3")
                          .arg(m_batchResolveIndex + 1).arg(m_batchUrls.size()).arg(message));
            ++m_batchResolveIndex;
            resolveNextBatchUrl();
            return;
        }
        if (!m_resolving) return;
        m_resolving = false;
        setBusy(false);
        m_stageLabel->setText(MNET_TEXT("解析失败"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        appendLog(message);
    });

    connect(&m_downloader, &DownloadController::stageChanged, this,
            [this](const QString &stage) { m_stageLabel->setText(AppLocale::text(stage)); });
    connect(&m_downloader, &DownloadController::progressTextChanged, this,
            [this](const QString &text) { m_progressLabel->setText(AppLocale::text(text)); });
    connect(&m_downloader, &DownloadController::logMessage,
            this, &MainWindow::appendLog);
    connect(&m_downloader, &DownloadController::completed, this,
            [this](const QString &path) {
        appendLog(MNET_TEXT("完成：%1").arg(path));
        if (m_batchDownloading) {
            m_batchMedia.removeFirst();
            startNextBatchItem();
            return;
        }
        setBusy(false);
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(100);
    });
    connect(&m_downloader, &DownloadController::failed, this,
            [this](const QString &message) {
        appendLog(message);
        if (m_batchDownloading) {
            m_batchMedia.removeFirst();
            startNextBatchItem();
            return;
        }
        setBusy(false);
        m_stageLabel->setText(MNET_TEXT("任务失败"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
    });
    connect(&m_downloader, &DownloadController::canceled, this, [this] {
        if (m_batchActive || m_batchDownloading) {
            m_batchActive = false;
            m_batchDownloading = false;
            m_batchMedia.clear();
            setBusy(false);
            m_stageLabel->setText(MNET_TEXT("已取消"));
            m_progressLabel->clear();
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            appendLog(MNET_TEXT("任务已取消"));
            return;
        }
        setBusy(false);
        m_stageLabel->setText(MNET_TEXT("已取消"));
        m_progressLabel->clear();
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        appendLog(MNET_TEXT("任务已取消"));
    });
}

void MainWindow::applyTheme()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#central {
            background: #111315;
            color: #F4F6F8;
        }
        QLabel#eyebrow {
            color: #FF5364;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel#heading {
            color: #FFFFFF;
            font-size: 28px;
            font-weight: 700;
        }
        QLabel#sessionStatus {
            background: #202428;
            border: 1px solid #343A3F;
            border-radius: 8px;
            color: #AEB6BC;
            padding: 8px 12px;
            font-size: 12px;
            font-weight: 600;
        }
        QLabel#sessionStatus[connected="true"] {
            background: #12362D;
            border-color: #225D4D;
            color: #7EE2BE;
        }
        QLabel#sectionLabel {
            color: #9BA4AA;
            font-size: 12px;
            font-weight: 700;
        }
        QLabel#languageLabel {
            color: #9BA4AA;
            font-size: 12px;
            font-weight: 650;
        }
        QFrame#divider {
            color: #2A2F33;
            background: #2A2F33;
            border: 0;
            max-height: 1px;
        }
        QLineEdit, QComboBox {
            background: #1B1F22;
            border: 1px solid #343A3F;
            border-radius: 8px;
            color: #F4F6F8;
            padding: 0 13px;
            selection-background-color: #E43D50;
        }
        QPlainTextEdit#batchEdit {
            background: #1B1F22;
            border: 1px solid #343A3F;
            border-radius: 8px;
            color: #F4F6F8;
            padding: 9px 11px;
            selection-background-color: #E43D50;
        }
        QPlainTextEdit#batchEdit:hover {
            border-color: #596168;
        }
        QPlainTextEdit#batchEdit:focus {
            border: 2px solid #FF5364;
            padding: 8px 10px;
        }
        QLineEdit:hover, QComboBox:hover {
            border-color: #596168;
        }
        QLineEdit:focus, QComboBox:focus {
            border: 2px solid #FF5364;
            padding: 0 12px;
        }
        QComboBox::drop-down {
            border: 0;
            width: 24px;
        }
        QComboBox QAbstractItemView {
            background: #202428;
            color: #F4F6F8;
            border: 1px solid #3A4147;
            selection-background-color: #C93547;
            padding: 5px;
        }
        QMenu {
            background: #202428;
            color: #F4F6F8;
            border: 1px solid #3A4147;
            padding: 6px;
        }
        QMenu::item {
            min-width: 150px;
            padding: 8px 26px 8px 10px;
            border-radius: 5px;
        }
        QMenu::item:selected {
            background: #343A3F;
        }
        QMenu::item:checked {
            color: #FF8995;
            font-weight: 650;
        }
        QPushButton, QToolButton {
            border: 1px solid #3A4147;
            border-radius: 8px;
            background: #24292D;
            color: #F4F6F8;
            font-weight: 650;
            padding: 0 14px;
        }
        QPushButton:hover, QToolButton:hover {
            background: #2D3338;
            border-color: #69737A;
        }
        QPushButton:pressed, QToolButton:pressed {
            background: #181B1E;
        }
        QPushButton:focus, QToolButton:focus {
            border: 2px solid #F77C89;
        }
        QPushButton:disabled, QToolButton:disabled {
            color: #687077;
            background: #1A1D20;
            border-color: #292E32;
        }
        QPushButton#primaryButton {
            background: #E43D50;
            border-color: #F05B6C;
            color: #FFFFFF;
            font-size: 14px;
        }
        QPushButton#primaryButton:hover {
            background: #F04A5E;
        }
        QPushButton#primaryButton:pressed {
            background: #C83244;
        }
        QPushButton#primaryButton:disabled {
            background: #532A31;
            border-color: #633139;
            color: #9E7278;
        }
        QLabel#mediaTitle {
            color: #FFFFFF;
            font-size: 18px;
            font-weight: 650;
        }
        QLabel#streamName {
            color: #8D979E;
            font-size: 12px;
            min-width: 36px;
        }
        QLabel#streamValue {
            color: #C7CDD1;
            font-size: 12px;
        }
        QLabel[state="good"] {
            background: #12362D;
            border-radius: 5px;
            color: #7EE2BE;
            font-size: 11px;
            font-weight: 700;
            padding: 3px 7px;
        }
        QLabel[state="warning"] {
            background: #44341B;
            border-radius: 5px;
            color: #FFD07A;
            font-size: 11px;
            font-weight: 700;
            padding: 3px 7px;
        }
        QLabel[state="muted"] {
            background: #282D31;
            border-radius: 5px;
            color: #9BA4AA;
            font-size: 11px;
            font-weight: 700;
            padding: 3px 7px;
        }
        QCheckBox {
            color: #D6DBDE;
            spacing: 8px;
        }
        QLabel#stageLabel {
            color: #F4F6F8;
            font-size: 12px;
            font-weight: 650;
        }
        QLabel#progressText {
            color: #8F999F;
            font-size: 12px;
        }
        QProgressBar {
            border: 0;
            border-radius: 2px;
            background: #292E32;
        }
        QProgressBar::chunk {
            border-radius: 2px;
            background: #FF5364;
        }
        QPlainTextEdit#logView {
            background: #0C0E0F;
            border: 1px solid #292E32;
            border-radius: 8px;
            color: #97A2A9;
            font-family: "SFMono-Regular", "Cascadia Mono", monospace;
            font-size: 11px;
            padding: 9px;
        }
        QToolTip {
            background: #2B3034;
            color: #F4F6F8;
            border: 1px solid #545C62;
            padding: 6px;
        }
    )"));
}

void MainWindow::beginResolve()
{
    if (m_downloader.isActive()) return;
    const QUrl url = QUrl::fromUserInput(m_urlEdit->text().trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        appendLog(MNET_TEXT("请输入有效的视频页面地址"));
        m_urlEdit->setFocus();
        return;
    }

    m_pendingUrl = url;
    m_resolving = true;
    m_hasMedia = false;
    resetMediaDisplay();
    setBusy(true);
    m_stageLabel->setText(MNET_TEXT("正在连接浏览器会话"));
    m_progressLabel->setText(MNET_TEXT("Cookie 只用于 Mnet Plus 域"));
    m_progressBar->setRange(0, 0);
    appendLog(MNET_TEXT("开始解析：%1").arg(url.toString()));
    m_cookieLoader.load(url, m_browserCombo->currentData().toString());
}

void MainWindow::resolveAsGuest(const QString &reason)
{
    if (!m_resolving) return;
    appendLog(reason);
    setCookieStatus(QStringLiteral("游客模式"));
    m_cookieStatus->setProperty("connected", false);
    m_cookieStatus->style()->unpolish(m_cookieStatus);
    m_cookieStatus->style()->polish(m_cookieStatus);
    m_resolver.setCookies({});
    m_downloader.setCookies({});
    m_stageLabel->setText(MNET_TEXT("正在解析页面"));
    m_resolver.resolve(m_pendingUrl);
}

void MainWindow::showMedia(const MediaInfo &media)
{
    if (!m_resolving) return;
    m_resolving = false;
    m_media = media;
    m_hasMedia = true;
    setBusy(false);
    populateMediaDetails(media);
    m_downloadButton->setEnabled(true);
    m_stageLabel->setText(MNET_TEXT("解析完成"));
    m_progressLabel->setText(media.previewOnly
        ? MNET_TEXT("当前会话只返回预览流")
        : MNET_TEXT("可以开始下载"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    appendLog(MNET_TEXT("媒体解析完成：%1").arg(media.title));
}

void MainWindow::populateMediaDetails(const MediaInfo &media)
{
    m_titleLabel->setText(media.title);
    m_titleLabel->setToolTip(media.title);
    setStreamRow(m_videoState, m_videoValue,
                 media.previewOnly ? MNET_TEXT("预览流") : MNET_TEXT("已找到"),
                 media.videoUrl,
                 media.previewOnly ? QStringLiteral("warning") : QStringLiteral("good"));
    if (media.audioUrl.isEmpty()) {
        setStreamRow(m_audioState, m_audioValue, MNET_TEXT("随 HLS"),
                     MNET_TEXT("将使用 HLS 中的音轨"), QStringLiteral("muted"));
    } else {
        setStreamRow(m_audioState, m_audioValue, MNET_TEXT("已找到"),
                     media.audioUrl, QStringLiteral("good"));
    }
    if (media.captionId.isEmpty()) {
        setStreamRow(m_captionState, m_captionValue, MNET_TEXT("未公开"),
                     MNET_TEXT("页面未提供字幕配置"), QStringLiteral("muted"));
    } else {
        const QString languages = media.captionLanguages.isEmpty()
            ? MNET_TEXT("字幕配置 %1").arg(media.captionId)
            : MNET_TEXT("全部下载 · %1").arg(media.captionLanguages.join(QStringLiteral(" / ")));
        setStreamRow(m_captionState, m_captionValue, MNET_TEXT("已找到"),
                     languages, QStringLiteral("good"));
    }
}

void MainWindow::setBusy(bool busy)
{
    m_resolveButton->setEnabled(!busy);
    m_urlEdit->setEnabled(!busy);
    m_browserCombo->setEnabled(!busy);
    m_languageCombo->setEnabled(!busy);
    m_outputEdit->setEnabled(!busy);
    m_outputButton->setEnabled(!busy);
    m_captionCheck->setEnabled(!busy);
    m_srtCheck->setEnabled(!busy);
    m_batchEdit->setEnabled(!busy);
    m_batchButton->setEnabled(!busy);
    m_filenameTemplateEdit->setEnabled(!busy);
    m_templateResetButton->setEnabled(!busy);
    m_downloadButton->setEnabled(!busy && m_hasMedia);
    m_cancelButton->setEnabled(busy);
}

void MainWindow::appendLog(const QString &message)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
             AppLocale::text(message)));
}

void MainWindow::chooseOutputDirectory()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, MNET_TEXT("选择输出目录"), m_outputEdit->text());
    if (!directory.isEmpty()) {
        m_outputEdit->setText(directory);
        QSettings().setValue(QStringLiteral("outputDirectory"), directory);
    }
}

void MainWindow::startDownload()
{
    if (!m_hasMedia || m_downloader.isActive()) return;
    const QString directory = m_outputEdit->text().trimmed();
    if (directory.isEmpty()) {
        appendLog(MNET_TEXT("请选择输出目录"));
        return;
    }
    QSettings().setValue(QStringLiteral("outputDirectory"), directory);
    m_media.filenameTemplate = currentFilenameTemplate();
    setBusy(true);
    m_cancelButton->setEnabled(true);
    m_progressBar->setRange(0, 0);
    m_progressLabel->clear();
    m_downloader.start(m_media, directory, m_captionCheck->isChecked(), m_srtCheck->isChecked());
}

QString MainWindow::currentFilenameTemplate() const
{
    const QString templateString = m_filenameTemplateEdit->text().trimmed();
    return templateString.isEmpty() ? DownloadController::defaultFilenameTemplate() : templateString;
}

void MainWindow::startBatchDownload()
{
    if (m_downloader.isActive() || m_batchActive) return;
    QStringList urls;
    const QStringList lines = m_batchEdit->toPlainText().split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.isEmpty()) continue;
        const QUrl url = QUrl::fromUserInput(line);
        if (url.isValid() && !url.host().isEmpty()) urls.append(url.toString(QUrl::FullyEncoded));
    }
    urls.removeDuplicates();
    if (urls.isEmpty()) {
        appendLog(MNET_TEXT("请至少输入一个视频页面地址"));
        m_batchEdit->setFocus();
        return;
    }

    m_batchUrls = urls;
    m_batchMedia.clear();
    m_batchResolveIndex = 0;
    m_batchActive = true;
    m_batchDownloading = false;
    appendLog(MNET_TEXT("批量下载：共 %1 个地址，开始逐个解析").arg(m_batchUrls.size()));
    setBusy(true);
    m_cancelButton->setEnabled(true);
    m_stageLabel->setText(MNET_TEXT("批量解析"));
    m_progressBar->setRange(0, 0);
    m_progressLabel->setText(MNET_TEXT("正在连接浏览器会话"));
    m_cookieLoader.load(QUrl::fromUserInput(m_batchUrls.constFirst()),
                        m_browserCombo->currentData().toString());
}

void MainWindow::beginBatchResolve()
{
    if (!m_batchActive) return;
    m_batchResolveIndex = 0;
    resolveNextBatchUrl();
}

void MainWindow::resolveNextBatchUrl()
{
    if (!m_batchActive) return;
    if (m_batchResolveIndex >= m_batchUrls.size()) {
        beginBatchDownloadPhase();
        return;
    }
    const QUrl url = QUrl::fromUserInput(m_batchUrls.at(m_batchResolveIndex));
    m_pendingUrl = url;
    m_stageLabel->setText(MNET_TEXT("批量解析 %1/%2")
                              .arg(m_batchResolveIndex + 1).arg(m_batchUrls.size()));
    m_progressLabel->setText(url.toString());
    m_resolver.resolve(url);
}

void MainWindow::beginBatchDownloadPhase()
{
    if (!m_batchActive) return;
    if (m_batchMedia.isEmpty()) {
        appendLog(MNET_TEXT("没有可下载的媒体，批量任务结束"));
        m_batchActive = false;
        setBusy(false);
        m_stageLabel->setText(MNET_TEXT("批量结束"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        return;
    }
    m_batchDownloading = true;
    appendLog(MNET_TEXT("解析完成，开始下载 %1 个视频").arg(m_batchMedia.size()));
    startNextBatchItem();
}

void MainWindow::startNextBatchItem()
{
    if (!m_batchActive || !m_batchDownloading) return;
    if (m_batchMedia.isEmpty()) {
        m_batchActive = false;
        m_batchDownloading = false;
        setBusy(false);
        m_stageLabel->setText(MNET_TEXT("批量完成"));
        m_progressLabel->setText(MNET_TEXT("全部下载完成"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(100);
        appendLog(MNET_TEXT("批量下载全部完成"));
        return;
    }

    MediaInfo media = m_batchMedia.constFirst();
    media.filenameTemplate = currentFilenameTemplate();
    const QString directory = m_outputEdit->text().trimmed();
    if (directory.isEmpty()) {
        appendLog(MNET_TEXT("请选择输出目录"));
        m_batchActive = false;
        m_batchDownloading = false;
        setBusy(false);
        return;
    }
    QSettings().setValue(QStringLiteral("outputDirectory"), directory);
    appendLog(MNET_TEXT("开始下载（剩余 %1）：%2").arg(m_batchMedia.size()).arg(media.title));
    m_stageLabel->setText(MNET_TEXT("批量下载中"));
    m_progressBar->setRange(0, 0);
    m_progressLabel->clear();
    m_downloader.start(media, directory, m_captionCheck->isChecked(), m_srtCheck->isChecked());
}

void MainWindow::resetMediaDisplay()
{
    m_titleLabel->setText(MNET_TEXT("等待解析视频页面"));
    setStreamRow(m_videoState, m_videoValue, MNET_TEXT("等待"),
                 QStringLiteral("-"), QStringLiteral("muted"));
    setStreamRow(m_audioState, m_audioValue, MNET_TEXT("等待"),
                 QStringLiteral("-"), QStringLiteral("muted"));
    setStreamRow(m_captionState, m_captionValue, MNET_TEXT("等待"),
                 QStringLiteral("-"), QStringLiteral("muted"));
    m_downloadButton->setEnabled(false);
}

QString MainWindow::browserDisplayName(const QString &browser) const
{
    const QMap<QString, QString> names = {
        {QStringLiteral("auto"), MNET_TEXT("浏览器")},
        {QStringLiteral("chrome"), QStringLiteral("Chrome")},
        {QStringLiteral("edge"), QStringLiteral("Edge")},
    };
    return names.value(browser, browser);
}

void MainWindow::setStreamRow(QLabel *stateLabel, QLabel *valueLabel,
                              const QString &state, const QString &value,
                              const QString &stateStyle)
{
    stateLabel->setText(state);
    stateLabel->setProperty("state", stateStyle);
    stateLabel->style()->unpolish(stateLabel);
    stateLabel->style()->polish(stateLabel);
    valueLabel->setText(value);
    valueLabel->setToolTip(value);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_downloader.isActive() && !m_resolving) {
        event->accept();
        return;
    }
    const auto result = QMessageBox::question(
        this, MNET_TEXT("退出下载器"),
        MNET_TEXT("当前任务仍在运行。确定要取消并退出吗？"),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (result == QMessageBox::Yes) {
        if (m_downloader.isActive()) m_downloader.cancel();
        if (m_resolving) {
            m_resolving = false;
            m_cookieLoader.cancel();
            m_resolver.cancel();
        }
        event->accept();
    } else {
        event->ignore();
    }
}
