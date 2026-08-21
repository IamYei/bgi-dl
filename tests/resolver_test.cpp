#include "../src/mnetresolver.h"

#include <QtTest>

class ResolverTest final : public QObject
{
    Q_OBJECT

private slots:
    void prefersFullConvertedStream();
    void fallsBackToPreviewStream();
    void separatesAudioPlaylist();
    void keeps720AndDerivesAudio();
    void derivesAudioWithCodecSuffix();
    void extractsPublishDate();
    void reportsMissingStream();
};

void ResolverTest::prefersFullConvertedStream()
{
    const QString html = QStringLiteral(R"HTML(
        <html><head>
        <meta property="og:title" content="Show &amp; Stage">
        <meta property="og:video" content="https://video.cdn.mnetplus.world/path/preview/samplemaster.m3u8">
        <script type="application/ld+json">
        {"duration":"PT1H02M03.5S"}
        </script>
        </head><body><script>
        {\"videoSrc\":\"https:\/\/video.cdn.mnetplus.world\/path\/converted\/sample_1080pw.m3u8\",
         \"audioUrl\":\"https:\/\/video.cdn.mnetplus.world\/path\/converted\/sample_audio.cmfa\",
         \"videoCaptionId\":\"69fdf4951bbf77224c50aa50\",
         \"languageConfigs\":[{\"language\":\"ja\"},{\"language\":\"en\"},{\"language\":\"zh_CN\"}]}
        </script></body></html>
    )HTML");
    const QUrl pageUrl(QStringLiteral(
        "https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303"));
    QString error;
    const MediaInfo media = MnetResolver::parsePage(html, pageUrl, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(media.videoId, QStringLiteral("69fc55cf40f458661f11a303"));
    QCOMPARE(media.title, QStringLiteral("Show & Stage"));
    QCOMPARE(media.videoUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/path/converted/sample_1080pw.m3u8"));
    QCOMPARE(media.audioUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/path/converted/sample_audio.cmfa"));
    QCOMPARE(media.captionId, QStringLiteral("69fdf4951bbf77224c50aa50"));
    QVERIFY(media.captionLanguages.contains(QStringLiteral("ja")));
    QVERIFY(media.captionLanguages.contains(QStringLiteral("en")));
    QVERIFY(media.captionLanguages.contains(QStringLiteral("zh_CN")));
    QCOMPARE(media.durationSecond, 3723.5);
    QVERIFY(!media.previewOnly);
}

void ResolverTest::fallsBackToPreviewStream()
{
    const QString html = QStringLiteral(R"HTML(
        <html><head><title>Preview Video</title>
        <meta property="og:video" content="https://video.cdn.mnetplus.world/path/preview/master.m3u8">
        </head></html>
    )HTML");
    QString error;
    const MediaInfo media = MnetResolver::parsePage(
        html,
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(media.title, QStringLiteral("Preview Video"));
    QVERIFY(media.previewOnly);
}

void ResolverTest::separatesAudioPlaylist()
{
    const QString html = QStringLiteral(R"HTML(
        <meta property="og:title" content="Separate audio">
        <script>{
          "video":"https://video.cdn.mnetplus.world/v/converted/program_1080pw.m3u8",
          "audio":"https://video.cdn.mnetplus.world/v/converted/program_audio.m3u8",
          "segment":"https://video.cdn.mnetplus.world/v/converted/program_audio_0001.cmfa"
        }</script>
    )HTML");
    QString error;
    const MediaInfo media = MnetResolver::parsePage(
        html,
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(media.videoUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_1080pw.m3u8"));
    QCOMPARE(media.audioUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_audio.m3u8"));
}

void ResolverTest::keeps720AndDerivesAudio()
{
    const QString html = QStringLiteral(R"HTML(
        <meta property="og:title" content="HD upgrade">
        <script>{
          "video":"https://video.cdn.mnetplus.world/v/converted/program_720pw.m3u8"
        }</script>
    )HTML");
    QString error;
    const MediaInfo media = MnetResolver::parsePage(
        html,
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);

    QVERIFY(error.isEmpty());
    QCOMPARE(media.videoUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_720pw.m3u8"));
    QCOMPARE(media.audioUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_audio.cmfa"));
    QVERIFY(media.audioDerived);
}

void ResolverTest::derivesAudioWithCodecSuffix()
{
    const QString html = QStringLiteral(R"HTML(
        <meta property="og:title" content="Codec suffixed">
        <script>{
          "video":"https://video.cdn.mnetplus.world/v/converted/program_720pw_h264.m3u8"
        }</script>
    )HTML");
    QString error;
    const MediaInfo media = MnetResolver::parsePage(
        html,
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(media.videoUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_720pw_h264.m3u8"));
    QCOMPARE(media.audioUrl, QStringLiteral(
        "https://video.cdn.mnetplus.world/v/converted/program_audio.cmfa"));
    QVERIFY(media.audioDerived);
}

void ResolverTest::extractsPublishDate()
{
    const QString html = QStringLiteral(R"HTML(
        <meta property="og:title" content="Dated video">
        <script>{
          "video":"https://video.cdn.mnetplus.world/v/converted/program_1080pw.m3u8",
          "uploadDate":"2026-05-01T09:00:00.000Z"
        }</script>
    )HTML");
    QString error;
    const MediaInfo media = MnetResolver::parsePage(
        html,
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(media.publishDate, QStringLiteral("2026-05-01"));
}

void ResolverTest::reportsMissingStream()
{
    QString error;
    MnetResolver::parsePage(
        QStringLiteral("<html><title>No Media</title></html>"),
        QUrl(QStringLiteral("https://www.mnetplus.world/media/en/videos/69fc55cf40f458661f11a303")),
        &error);
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(ResolverTest)
#include "resolver_test.moc"
