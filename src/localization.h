#pragma once

#include <QString>
#include <QStringList>

namespace AppLocale {

enum class Language
{
    English,
    Chinese,
    Japanese,
    Korean,
};

Language language();
QString languageCode();
QString defaultLanguageCode();
bool setLanguage(const QString &code);
QString text(const char *source);
QString text(const QString &source);

} // namespace AppLocale

#define MNET_TEXT(source) AppLocale::text(source)
