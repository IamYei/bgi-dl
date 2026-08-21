#include "../src/localization.h"

#include <QtTest>

class LocalizationTest final : public QObject
{
    Q_OBJECT

private slots:
    void translatesPrimaryUiInEachSupportedLanguage();
    void normalizesSupportedLanguageCodes();
    void preservesUnknownSourceText();
};

void LocalizationTest::translatesPrimaryUiInEachSupportedLanguage()
{
    QVERIFY(AppLocale::setLanguage(QStringLiteral("zh_CN")));
    QCOMPARE(MNET_TEXT("视频下载器"), QStringLiteral("视频下载器"));

    QVERIFY(AppLocale::setLanguage(QStringLiteral("en")));
    QCOMPARE(MNET_TEXT("视频下载器"), QStringLiteral("Video Downloader"));
    QCOMPARE(MNET_TEXT("下载并合并 MKV"), QStringLiteral("Download and merge MKV"));

    QVERIFY(AppLocale::setLanguage(QStringLiteral("ja")));
    QCOMPARE(MNET_TEXT("视频下载器"), QStringLiteral("ビデオダウンローダー"));
    QCOMPARE(MNET_TEXT("解析"), QStringLiteral("解析"));

    QVERIFY(AppLocale::setLanguage(QStringLiteral("ko")));
    QCOMPARE(MNET_TEXT("视频下载器"), QStringLiteral("비디오 다운로더"));
    QCOMPARE(MNET_TEXT("语言"), QStringLiteral("언어"));
}

void LocalizationTest::normalizesSupportedLanguageCodes()
{
    QVERIFY(AppLocale::setLanguage(QStringLiteral("en-US")));
    QCOMPARE(AppLocale::languageCode(), QStringLiteral("en"));
    QVERIFY(AppLocale::setLanguage(QStringLiteral("ja_JP")));
    QCOMPARE(AppLocale::languageCode(), QStringLiteral("ja"));
    QVERIFY(AppLocale::setLanguage(QStringLiteral("ko-KR")));
    QCOMPARE(AppLocale::languageCode(), QStringLiteral("ko"));
    QVERIFY(!AppLocale::setLanguage(QStringLiteral("fr_FR")));
}

void LocalizationTest::preservesUnknownSourceText()
{
    QVERIFY(AppLocale::setLanguage(QStringLiteral("en")));
    QCOMPARE(MNET_TEXT("未登记的文案"), QStringLiteral("未登记的文案"));
}

QTEST_APPLESS_MAIN(LocalizationTest)
#include "localization_test.moc"
