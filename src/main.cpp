#include "mainwindow.h"
#include "localization.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Mnet Plus Downloader"));
    QApplication::setApplicationDisplayName(QStringLiteral("Mnet Plus Downloader"));
    QApplication::setOrganizationName(QStringLiteral("Local Tools"));
    QApplication::setApplicationVersion(QStringLiteral("1.1.0"));

    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSize(13);
    app.setFont(font);

    MainWindow window;
    window.show();
    const QStringList arguments = app.arguments();
    const int screenshotIndex = arguments.indexOf(QStringLiteral("--screenshot"));
    if (screenshotIndex >= 0 && screenshotIndex + 1 < arguments.size()) {
        const QString screenshotPath = arguments.at(screenshotIndex + 1);
        QTimer::singleShot(500, &app, [&window, screenshotPath, &app] {
            window.grab().save(screenshotPath);
            app.quit();
        });
    }
    return app.exec();
}
