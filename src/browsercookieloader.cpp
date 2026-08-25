#include "browsercookieloader.h"
#include "localization.h"

#include <QtConcurrent>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <limits>

#if defined(Q_OS_MACOS)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#endif

namespace {
struct ChromiumSpec
{
    QString id;
    QString root;
    QString keyringName;
};

bool isMnetDomain(QString domain)
{
    domain = domain.trimmed().toLower();
    if (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
    return domain == QStringLiteral("mnetplus.world")
        || domain.endsWith(QStringLiteral(".mnetplus.world"));
}

// A cookie jar may store the host with or without a leading dot; normalise both.
bool cookieHostMatches(QString host)
{
    host = host.trimmed().toLower();
    if (host.startsWith(QLatin1Char('.'))) host.remove(0, 1);
    return host == QStringLiteral("mnetplus.world")
        || host.endsWith(QStringLiteral(".mnetplus.world"));
}

// Chromium encrypts with AES-GCM (prefix "v10"/"v11") or, on Windows, legacy DPAPI blobs.
bool isChromiumEncrypted(const QByteArray &value)
{
    return value.size() >= 3
        && (value.at(0) == 'v')
        && (value.at(1) == '1')
        && (value.at(2) == '0' || value.at(2) == '1');
}

// Fire-and-forget registry capture: remember the last DPAPI/app-bound key that
// successfully decrypted a cookie so the UI can explain what happened.

QList<ChromiumSpec> chromiumSpecs()
{
    const QString home = QDir::homePath();
#if defined(Q_OS_MACOS)
    return {
        {QStringLiteral("chrome"), home + QStringLiteral("/Library/Application Support/Google/Chrome"),
         QStringLiteral("Chrome")},
        {QStringLiteral("edge"), home + QStringLiteral("/Library/Application Support/Microsoft Edge"),
         QStringLiteral("Microsoft Edge")},
    };
#elif defined(Q_OS_WIN)
    // Browsers can be installed per-user or machine-wide; collect every known root
    // and keep the ones that exist. LOCALAPPDATA may be unset in some shells.
    QStringList roots;
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) {
        roots << local + QStringLiteral("/Google/Chrome/User Data");
        roots << local + QStringLiteral("/Microsoft/Edge/User Data");
    }
    roots << QDir::home().filePath(QStringLiteral("AppData/Local/Google/Chrome/User Data"));
    roots << QDir::home().filePath(QStringLiteral("AppData/Local/Microsoft/Edge/User Data"));
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty()) {
        roots << programFiles + QStringLiteral("/Google/Chrome/User Data");
        roots << programFiles + QStringLiteral("/Microsoft/Edge/User Data");
    }
    QList<ChromiumSpec> specs;
    const QStringList ids = {QStringLiteral("chrome"), QStringLiteral("edge")};
    for (int index = 0; index < ids.size(); ++index) {
        QString chosen;
        for (const QString &root : roots) {
            if (QFileInfo::exists(root)) { chosen = root; break; }
        }
        // Per-user install wins; otherwise keep the first candidate for diagnostics.
        if (chosen.isEmpty()) chosen = roots.value(index * 3);
        specs.append({ids.at(index), chosen, {}});
    }
    return specs;
#else
    return {
        {QStringLiteral("chrome"), home + QStringLiteral("/.config/google-chrome"), {}},
        {QStringLiteral("edge"), home + QStringLiteral("/.config/microsoft-edge"), {}},
    };
#endif
}

ChromiumSpec chromiumSpec(const QString &browser)
{
    for (const ChromiumSpec &spec : chromiumSpecs()) {
        if (spec.id == browser) return spec;
    }
    return {};
}

QStringList chromiumProfiles(const QString &root)
{
    QList<QPair<QDateTime, QString>> datedProfiles;
    QDir directory(root);
    if (!directory.exists()) return {};
    const QStringList names = directory.entryList(
        {QStringLiteral("Default"), QStringLiteral("Profile *")},
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QString profile = directory.filePath(name);
        QString database = QDir(profile).filePath(QStringLiteral("Network/Cookies"));
        if (!QFileInfo::exists(database)) database = QDir(profile).filePath(QStringLiteral("Cookies"));
        datedProfiles.append({QFileInfo(database).lastModified(), profile});
    }
    std::sort(datedProfiles.begin(), datedProfiles.end(), [](const auto &left, const auto &right) {
        return left.first > right.first;
    });
    QStringList profiles;
    for (const auto &profile : std::as_const(datedProfiles)) profiles.append(profile.second);
    return profiles;
}

bool copySqliteBundle(const QString &source, const QString &destination)
{
    if (!QFileInfo::exists(source)) return false;
    // Cookies databases are opened with WAL by running browsers; copying the main
    // file alone can yield a stale snapshot. Copying the whole bundle while the
    // browser holds it can also fail on Windows, so retry once after a beat.
    for (int attempt = 0; attempt < 2; ++attempt) {
        QFile::remove(destination);
        QFile::remove(destination + QStringLiteral("-wal"));
        QFile::remove(destination + QStringLiteral("-shm"));
        if (!QFile::copy(source, destination)) {
            QThread::msleep(150);
            continue;
        }
        if (QFileInfo::exists(source + QStringLiteral("-wal"))) {
            QFile::copy(source + QStringLiteral("-wal"), destination + QStringLiteral("-wal"));
        }
        if (QFileInfo::exists(source + QStringLiteral("-shm"))) {
            QFile::copy(source + QStringLiteral("-shm"), destination + QStringLiteral("-shm"));
        }
        return true;
    }
    return false;
}

#if defined(Q_OS_MACOS)
QByteArray keychainPassword(const QString &keyringName)
{
    const QString service = keyringName + QStringLiteral(" Safe Storage");
    const QByteArray serviceUtf8 = service.toUtf8();
    const QByteArray accountUtf8 = keyringName.toUtf8();
    CFStringRef serviceString = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(serviceUtf8.constData()),
        serviceUtf8.size(), kCFStringEncodingUTF8, false);
    if (!serviceString) return {};
    CFStringRef accountString = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(accountUtf8.constData()),
        accountUtf8.size(), kCFStringEncodingUTF8, false);
    if (!accountString) {
        CFRelease(serviceString);
        return {};
    }

    const void *keys[] = {kSecClass, kSecAttrAccount, kSecAttrService,
                          kSecReturnData, kSecMatchLimit};
    const void *values[] = {kSecClassGenericPassword, accountString, serviceString,
                            kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(accountString);
    CFRelease(serviceString);
    if (status != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) {
        if (result) CFRelease(result);
        return {};
    }

    const auto data = static_cast<CFDataRef>(result);
    QByteArray password(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                        CFDataGetLength(data));
    CFRelease(result);
    return password;
}

QByteArray chromiumKey(const ChromiumSpec &spec)
{
    const QByteArray password = keychainPassword(spec.keyringName);
    if (password.isEmpty()) return {};
    QByteArray key(16, Qt::Uninitialized);
    const QByteArray salt("saltysalt");
    const int status = CCKeyDerivationPBKDF(
        kCCPBKDF2, password.constData(), password.size(),
        reinterpret_cast<const uint8_t *>(salt.constData()), salt.size(),
        kCCPRFHmacAlgSHA1, 1003,
        reinterpret_cast<uint8_t *>(key.data()), key.size());
    return status == kCCSuccess ? key : QByteArray{};
}

QByteArray decryptChromiumValue(const QByteArray &encrypted,
                                const QByteArray &key,
                                bool hasHashPrefix,
                                const QString &host)
{
    if (!isChromiumEncrypted(encrypted)) return encrypted;
    if (key.size() != 16) return {};
    const QByteArray ciphertext = encrypted.mid(3);
    const QByteArray iv(16, ' ');
    QByteArray plaintext(ciphertext.size() + kCCBlockSizeAES128, Qt::Uninitialized);
    size_t outputLength = 0;
    const CCCryptorStatus status = CCCrypt(
        kCCDecrypt, kCCAlgorithmAES128, kCCOptionPKCS7Padding,
        key.constData(), key.size(), iv.constData(),
        ciphertext.constData(), ciphertext.size(),
        plaintext.data(), plaintext.size(), &outputLength);
    if (status != kCCSuccess) return {};
    plaintext.resize(static_cast<qsizetype>(outputLength));

    if (hasHashPrefix) {
        if (plaintext.size() < 32) return {};
        const QByteArray expected = QCryptographicHash::hash(
            host.toUtf8(), QCryptographicHash::Sha256);
        // Newer databases put a 32-byte SHA256(host) prefix before the value.
        if (plaintext.left(32) != expected) return {};
        plaintext.remove(0, 32);
    }
    return plaintext;
}
#elif defined(Q_OS_WIN)
QByteArray decryptDpapi(const QByteArray &encrypted)
{
    if (encrypted.isEmpty() || encrypted.size() > std::numeric_limits<DWORD>::max()) return {};
    DATA_BLOB input {};
    input.cbData = static_cast<DWORD>(encrypted.size());
    input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()));
    DATA_BLOB output {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) return {};
    QByteArray plaintext(reinterpret_cast<const char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

QByteArray decryptAesGcm(const QByteArray &encrypted, const QByteArray &key)
{
    constexpr int kPrefixSize = 3;
    constexpr int kNonceSize = 12;
    constexpr int kTagSize = 16;
    if (key.size() != 32 || encrypted.size() <= kPrefixSize + kNonceSize + kTagSize) return {};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS openStatus = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM,
                                                             nullptr, 0);
    if (openStatus != 0) return {};
    const wchar_t chainingMode[] = BCRYPT_CHAIN_MODE_GCM;
    const NTSTATUS chainingStatus = BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(chainingMode)), sizeof(chainingMode), 0);
    if (chainingStatus != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    DWORD keyObjectLength = 0;
    ULONG propertyLength = 0;
    const NTSTATUS propertyStatus = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&keyObjectLength),
        sizeof(keyObjectLength), &propertyLength, 0);
    if (propertyStatus != 0 || keyObjectLength == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    QByteArray keyObject(keyObjectLength, Qt::Uninitialized);
    BCRYPT_KEY_HANDLE cryptoKey = nullptr;
    const NTSTATUS keyStatus = BCryptGenerateSymmetricKey(
        algorithm, &cryptoKey, reinterpret_cast<PUCHAR>(keyObject.data()), keyObject.size(),
        reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())), key.size(), 0);
    if (keyStatus != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    const QByteArray nonce = encrypted.mid(kPrefixSize, kNonceSize);
    const QByteArray ciphertext = encrypted.mid(kPrefixSize + kNonceSize,
                                                 encrypted.size() - kPrefixSize - kNonceSize - kTagSize);
    const QByteArray tag = encrypted.right(kTagSize);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication {};
    BCRYPT_INIT_AUTH_MODE_INFO(authentication);
    authentication.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(nonce.constData()));
    authentication.cbNonce = nonce.size();
    authentication.pbTag = reinterpret_cast<PUCHAR>(const_cast<char *>(tag.constData()));
    authentication.cbTag = tag.size();

    QByteArray plaintext(ciphertext.size(), Qt::Uninitialized);
    ULONG plaintextSize = 0;
    const NTSTATUS decryptStatus = BCryptDecrypt(
        cryptoKey, reinterpret_cast<PUCHAR>(const_cast<char *>(ciphertext.constData())),
        ciphertext.size(), &authentication, nullptr, 0,
        reinterpret_cast<PUCHAR>(plaintext.data()), plaintext.size(), &plaintextSize, 0);
    BCryptDestroyKey(cryptoKey);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (decryptStatus != 0) return {};
    plaintext.resize(static_cast<qsizetype>(plaintextSize));
    return plaintext;
}

QByteArray chromiumKey(const ChromiumSpec &spec)
{
    QFile localState(QDir(spec.root).filePath(QStringLiteral("Local State")));
    if (!localState.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(localState.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const QString encoded = document.object().value(QStringLiteral("os_crypt")).toObject()
                                .value(QStringLiteral("encrypted_key")).toString();
    QByteArray encrypted = QByteArray::fromBase64(encoded.toLatin1());
    if (!encrypted.startsWith("DPAPI")) return {};
    encrypted.remove(0, 5);
    return decryptDpapi(encrypted);
}

QByteArray decryptChromiumValue(const QByteArray &encrypted,
                                const QByteArray &key,
                                bool hasHashPrefix,
                                const QString &host)
{
    QByteArray plaintext;
    if (isChromiumEncrypted(encrypted)) {
        if (key.size() != 32) return {};
        plaintext = decryptAesGcm(encrypted, key);
    } else {
        plaintext = decryptDpapi(encrypted);
    }
    if (plaintext.isEmpty()) return {};
    if (hasHashPrefix) {
        if (plaintext.size() < 32) return {};
        const QByteArray expected = QCryptographicHash::hash(
            host.toUtf8(), QCryptographicHash::Sha256);
        if (plaintext.left(32) != expected) return {};
        plaintext.remove(0, 32);
    }
    return plaintext;
}
#else
QByteArray chromiumKey(const ChromiumSpec &)
{
    return {};
}

QByteArray decryptChromiumValue(const QByteArray &encrypted, const QByteArray &, bool,
                                const QString &)
{
    return isChromiumEncrypted(encrypted) ? QByteArray{} : encrypted;
}
#endif

QList<QNetworkCookie> queryChromiumCookies(const QString &databasePath,
                                           const QByteArray &key)
{
    QList<QNetworkCookie> cookies;
    const QString connection = QStringLiteral("chromium-cookies-%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            int metaVersion = 0;
            QSqlQuery metaQuery(database);
            if (metaQuery.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'version'"))
                && metaQuery.next()) {
                metaVersion = metaQuery.value(0).toInt();
            }
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "SELECT host_key, name, value, encrypted_value, path, expires_utc, "
                "is_secure, is_httponly FROM cookies "
                "WHERE host_key = ? OR host_key LIKE ?"));
            query.addBindValue(QStringLiteral("%mnetplus.world"));
            query.addBindValue(QStringLiteral("%.mnetplus.world"));
            if (query.exec()) {
                while (query.next()) {
                    const QString host = query.value(0).toString();
                    if (!cookieHostMatches(host)) continue;
                    QByteArray value = query.value(2).toByteArray();
                    if (value.isEmpty()) {
#if defined(Q_OS_MACOS)
                        const bool hasHashPrefix = metaVersion >= 24;
#else
                        const bool hasHashPrefix = false;
#endif
                        value = decryptChromiumValue(query.value(3).toByteArray(), key,
                                                     hasHashPrefix, host);
                    }
                    if (value.isEmpty()) continue;

                    QNetworkCookie cookie(query.value(1).toByteArray(), value);
                    cookie.setDomain(host);
                    cookie.setPath(query.value(4).toString());
                    cookie.setSecure(query.value(6).toBool());
                    cookie.setHttpOnly(query.value(7).toBool());
                    const qint64 chromiumTime = query.value(5).toLongLong();
                    const qint64 unixSeconds = chromiumTime / 1000000LL - 11644473600LL;
                    if (unixSeconds > 0) {
                        const QDateTime expiration = QDateTime::fromSecsSinceEpoch(
                            unixSeconds, QTimeZone::UTC);
                        if (expiration <= QDateTime::currentDateTimeUtc()) continue;
                        cookie.setExpirationDate(expiration);
                    }
                    cookies.append(cookie);
                }
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return cookies;
}

// --- Firefox -----------------------------------------------------------------

struct FirefoxProfile
{
    QString name;
    QString path;
    bool isDefault = false;
};

QString firefoxProfilesIniPath()
{
#if defined(Q_OS_WIN)
    QStringList roots;
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) roots << local + QStringLiteral("/Mozilla/Firefox");
    roots << QDir::home().filePath(QStringLiteral("AppData/Local/Mozilla/Firefox"));
    roots << QDir::home().filePath(QStringLiteral("AppData/Roaming/Mozilla/Firefox"));
    for (const QString &root : roots) {
        const QString ini = QDir(root).filePath(QStringLiteral("profiles.ini"));
        if (QFileInfo::exists(ini)) return ini;
    }
    return QString{};
#else
    return QDir::home().filePath(QStringLiteral(".mozilla/firefox/profiles.ini"));
#endif
}

QList<FirefoxProfile> firefoxProfiles()
{
    QList<FirefoxProfile> profiles;
    const QString iniPath = firefoxProfilesIniPath();
    if (iniPath.isEmpty()) return profiles;

    QSettings ini(iniPath, QSettings::IniFormat);

    const QStringList groups = ini.childGroups();
    QString defaultRelative;
    ini.beginGroup(QStringLiteral("Install0"));
    defaultRelative = ini.value(QStringLiteral("Default")).toString();
    ini.endGroup();
    if (defaultRelative.isEmpty()) {
        ini.beginGroup(QStringLiteral("General"));
        defaultRelative = ini.value(QStringLiteral("Default")).toString();
        ini.endGroup();
    }

    for (const QString &group : groups) {
        if (!group.startsWith(QStringLiteral("Profile"), Qt::CaseInsensitive)) continue;
        ini.beginGroup(group);
        FirefoxProfile profile;
        profile.name = ini.value(QStringLiteral("Name")).toString();
        profile.path = ini.value(QStringLiteral("Path")).toString();
        profile.isDefault = ini.value(QStringLiteral("Default"), 0).toBool();
        const QString isRelative = ini.value(QStringLiteral("IsRelative")).toString();
        ini.endGroup();
        if (profile.path.isEmpty()) continue;
        if (isRelative == QStringLiteral("1") || isRelative.isEmpty()) {
            profile.path = QDir(QFileInfo(iniPath).absolutePath()).filePath(profile.path);
        }
        if (!profile.isDefault
            && !defaultRelative.isEmpty() && defaultRelative == profile.name) {
            profile.isDefault = true;
        }
        if (!QFileInfo::exists(profile.path)) continue;
        profiles.append(profile);
    }

    // Most recently used profile first; the default profile outranks everything.
    std::sort(profiles.begin(), profiles.end(), [](const FirefoxProfile &left,
                                                   const FirefoxProfile &right) {
        const QDateTime leftStamp = QFileInfo(QDir(left.path).filePath(
            QStringLiteral("cookies.sqlite"))).lastModified();
        const QDateTime rightStamp = QFileInfo(QDir(right.path).filePath(
            QStringLiteral("cookies.sqlite"))).lastModified();
        if (left.isDefault != right.isDefault) return left.isDefault;
        return leftStamp > rightStamp;
    });
    return profiles;
}

QList<QNetworkCookie> queryFirefoxCookies(const QString &databasePath)
{
    QList<QNetworkCookie> cookies;
    const QString connection = QStringLiteral("firefox-cookies-%1")
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            // value is stored as BLOB; host may or may not start with a dot.
            query.prepare(QStringLiteral(
                "SELECT host, name, value, path, expiry, isSecure, isHttpOnly FROM moz_cookies "
                "WHERE host LIKE ?"));
            query.addBindValue(QStringLiteral("%mnetplus.world%"));
            if (query.exec()) {
                while (query.next()) {
                    QString host = query.value(0).toString().toLower();
                    if (!cookieHostMatches(host)) continue;
                    const QByteArray value = query.value(2).toByteArray();
                    if (value.isEmpty()) continue;

                    QNetworkCookie cookie(query.value(1).toByteArray(), value);
                    cookie.setDomain(host);
                    cookie.setPath(query.value(3).toString());
                    cookie.setSecure(query.value(5).toBool());
                    cookie.setHttpOnly(query.value(6).toBool());
                    const qint64 expiry = query.value(4).toLongLong();
                    if (expiry > 0) {
                        const QDateTime expiration = QDateTime::fromSecsSinceEpoch(
                            expiry, QTimeZone::UTC);
                        if (expiration <= QDateTime::currentDateTimeUtc()) continue;
                        cookie.setExpirationDate(expiration);
                    }
                    cookies.append(cookie);
                }
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return cookies;
}

// --- Netscape cookies.txt ------------------------------------------------------

// Standard cookies.txt (Netscape) format:
// domain \t includeSubdomains(TRUE/FALSE) \t path \t secure(TRUE/FALSE) \t expiry \t name \t value
// Lines starting with '#' are comments, except the "#HttpOnly_" prefix.
QList<QNetworkCookie> parseCookiesTxtContent(const QByteArray &content)
{
    QList<QNetworkCookie> cookies;
    const QStringList lines = QString::fromUtf8(content).split(
        QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        bool httpOnly = false;
        if (line.startsWith(QStringLiteral("#HttpOnly_"), Qt::CaseInsensitive)) {
            httpOnly = true;
            line.remove(0, 10);
        }
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 7) continue;

        QString domain = parts.at(0).trimmed().toLower();
        if (!cookieHostMatches(domain)) continue;
        const QString path = parts.at(2).trimmed();
        const bool secure = parts.at(3).trimmed().compare(
            QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
        const qint64 expiry = parts.at(4).trimmed().toLongLong();
        const QByteArray name = parts.at(5).trimmed().toUtf8();
        const QByteArray value = parts.at(6).toUtf8();
        if (name.isEmpty() || value.isEmpty()) continue;

        QNetworkCookie cookie(name, value);
        cookie.setDomain(domain);
        cookie.setPath(path.isEmpty() ? QStringLiteral("/") : path);
        cookie.setSecure(secure);
        cookie.setHttpOnly(httpOnly);
        if (expiry > 0) {
            const QDateTime expiration = QDateTime::fromSecsSinceEpoch(expiry, QTimeZone::UTC);
            if (expiration <= QDateTime::currentDateTimeUtc()) continue;
            cookie.setExpirationDate(expiration);
        }
        cookies.append(cookie);
    }
    return cookies;
}
} // namespace

QList<QNetworkCookie> BrowserCookieLoader::parseCookiesTxt(const QByteArray &content)
{
    return parseCookiesTxtContent(content);
}

BrowserCookieLoader::BrowserCookieLoader(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<CookieLoadResult>::finished, this, [this] {
        if (m_ignoreResult) return;
        const CookieLoadResult result = m_watcher.result();
        if (!result.cookies.isEmpty()) {
            emit loaded(result.cookies, result.browser);
        } else {
            emit unavailable(result.error.isEmpty()
                ? MNET_TEXT("未找到 Mnet Plus 浏览器会话，将使用游客模式")
                : AppLocale::text(result.error));
        }
    });
}

BrowserCookieLoader::~BrowserCookieLoader()
{
    m_ignoreResult = true;
}

void BrowserCookieLoader::load(const QUrl &, const QString &browser)
{
    if (m_watcher.isRunning()) {
        emit unavailable(MNET_TEXT("浏览器会话仍在读取，将暂时使用游客模式"));
        return;
    }
    m_ignoreResult = false;
    const QString cookiesTxtPath = m_cookiesTxtPath;
    emit loadingBrowser(browser);
    m_watcher.setFuture(QtConcurrent::run([browser, cookiesTxtPath] {
        return loadSync(browser, cookiesTxtPath);
    }));
}

void BrowserCookieLoader::cancel()
{
    m_ignoreResult = true;
}

void BrowserCookieLoader::setCookiesTxtPath(const QString &path)
{
    m_cookiesTxtPath = path;
}

CookieLoadResult BrowserCookieLoader::loadSync(const QString &requestedBrowser,
                                               const QString &cookiesTxtPath)
{
    // An explicit cookies.txt file always wins when it is selected.
    if (requestedBrowser == QStringLiteral("cookies_txt") && !cookiesTxtPath.isEmpty()) {
        return loadCookiesTxtFile(cookiesTxtPath);
    }
    if (requestedBrowser == QStringLiteral("extension")) {
        return loadExtensionCache();
    }

    const QStringList candidates = requestedBrowser == QStringLiteral("auto")
        ? automaticCandidates() : QStringList{requestedBrowser};
    QStringList errors;
    CookieLoadResult bestResult;
    int bestScore = -1;
    for (const QString &browser : candidates) {
        CookieLoadResult result;
        if (browser == QStringLiteral("firefox")) {
            result = loadFirefox();
        } else if (browser == QStringLiteral("extension")) {
            result = loadExtensionCache();
        } else {
            result = loadChromium(browser);
        }
        if (!result.cookies.isEmpty()) {
            int score = result.cookies.size();
            for (const QNetworkCookie &cookie : result.cookies) {
                const QByteArray name = cookie.name().toLower();
                if (name.contains("auth") || name.contains("token")
                    || name.contains("session") || name.contains("access")
                    || name.contains("refresh")) {
                    score += 100;
                }
            }
            if (score > bestScore) {
                bestScore = score;
                bestResult = result;
            }
            if (score >= 100) return result;
        }
        if (!result.error.isEmpty()) errors.append(result.error);
    }

    if (!bestResult.cookies.isEmpty()) return bestResult;

    // When nothing else worked but a cookies.txt path is configured, use it as fallback.
    if (!cookiesTxtPath.isEmpty()) {
        const CookieLoadResult fallback = loadCookiesTxtFile(cookiesTxtPath);
        if (!fallback.cookies.isEmpty()) return fallback;
    }

    CookieLoadResult result;
    result.error = errors.isEmpty()
        ? MNET_TEXT("没有检测到可读取的浏览器配置，将使用游客模式")
        : MNET_TEXT("未找到 Mnet Plus 登录会话，将使用游客模式（%1）")
                  .arg(AppLocale::text(errors.constFirst()));
    return result;
}

CookieLoadResult BrowserCookieLoader::loadChromium(const QString &browser)
{
    CookieLoadResult result;
    result.browser = browser;
    const ChromiumSpec spec = chromiumSpec(browser);
    if (spec.id.isEmpty() || !QFileInfo::exists(spec.root)) {
        result.error = MNET_TEXT("%1 配置不存在").arg(browser);
        return result;
    }

    const QByteArray key = chromiumKey(spec);
    bool sawDatabase = false;
    for (const QString &profile : chromiumProfiles(spec.root)) {
        QString source = QDir(profile).filePath(QStringLiteral("Network/Cookies"));
        if (!QFileInfo::exists(source)) source = QDir(profile).filePath(QStringLiteral("Cookies"));
        if (!QFileInfo::exists(source)) continue;
        sawDatabase = true;

        QTemporaryDir temporary;
        if (!temporary.isValid()) continue;
        const QString copy = temporary.filePath(QStringLiteral("Cookies"));
        if (!copySqliteBundle(source, copy)) continue;
        result.cookies = queryChromiumCookies(copy, key);
        if (!result.cookies.isEmpty()) return result;
    }
    if (!sawDatabase) {
        result.error = MNET_TEXT("%1 中没有可读取的 Cookie 数据库").arg(browser);
        return result;
    }
    result.error = key.isEmpty()
        ? MNET_TEXT("无法读取 %1 的浏览器解密密钥（新版 Chrome/Edge 可能需要完全关闭浏览器后重试，或改用 cookies.txt）").arg(browser)
        : MNET_TEXT("%1 中没有 Mnet Plus Cookie（请确认已在该浏览器登录 Mnet Plus；新版 Chrome 加密的 Cookie 可能无法读取，可改用 cookies.txt）").arg(browser);
    return result;
}

CookieLoadResult BrowserCookieLoader::loadFirefox()
{
    CookieLoadResult result;
    result.browser = QStringLiteral("firefox");
    const QList<FirefoxProfile> profiles = firefoxProfiles();
    if (profiles.isEmpty()) {
        result.error = MNET_TEXT("Firefox 配置不存在");
        return result;
    }
    for (const FirefoxProfile &profile : profiles) {
        const QString source = QDir(profile.path).filePath(QStringLiteral("cookies.sqlite"));
        if (!QFileInfo::exists(source)) continue;

        QTemporaryDir temporary;
        if (!temporary.isValid()) continue;
        const QString copy = temporary.filePath(QStringLiteral("cookies.sqlite"));
        if (!copySqliteBundle(source, copy)) continue;
        result.cookies = queryFirefoxCookies(copy);
        if (!result.cookies.isEmpty()) return result;
    }
    result.error = MNET_TEXT("Firefox 中没有 Mnet Plus Cookie");
    return result;
}

CookieLoadResult BrowserCookieLoader::loadCookiesTxtFile(const QString &path)
{
    CookieLoadResult result;
    result.browser = QStringLiteral("cookies_txt");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = MNET_TEXT("无法读取 cookies.txt：%1").arg(path);
        return result;
    }
    result.cookies = parseCookiesTxt(file.readAll());
    if (result.cookies.isEmpty()) {
        result.error = MNET_TEXT("cookies.txt 中没有 Mnet Plus Cookie（域名需为 mnetplus.world）");
    }
    return result;
}

// --- bgi-dl browser extension cache -------------------------------------------

QString BrowserCookieLoader::extensionCachePath()
{
#if defined(Q_OS_WIN)
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) return QDir(local).filePath(QStringLiteral("bgi-dl/cookies.json"));
    return QDir::home().filePath(QStringLiteral("AppData/Local/bgi-dl/cookies.json"));
#else
    const QString cache = qEnvironmentVariable("XDG_CACHE_HOME");
    if (!cache.isEmpty()) return QDir(cache).filePath(QStringLiteral("bgi-dl/cookies.json"));
    return QDir::home().filePath(QStringLiteral(".cache/bgi-dl/cookies.json"));
#endif
}

CookieLoadResult BrowserCookieLoader::loadExtensionCache()
{
    CookieLoadResult result;
    result.browser = QStringLiteral("extension");
    const QString path = extensionCachePath();
    if (!QFileInfo::exists(path)) {
        result.error = MNET_TEXT("未检测到浏览器扩展缓存（请安装 bgi-dl 浏览器助手并点击其图标推送 Cookie）");
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = MNET_TEXT("无法读取浏览器扩展缓存：%1").arg(path);
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = MNET_TEXT("浏览器扩展缓存格式无效");
        return result;
    }

    const QJsonObject root = document.object();
    const qint64 timestamp = static_cast<qint64>(root.value(QStringLiteral("timestamp")).toDouble());
    // Cookie pushes older than an hour are stale; require a fresh click.
    if (timestamp > 0
        && QDateTime::currentSecsSinceEpoch() - timestamp > 3600) {
        result.error = MNET_TEXT("浏览器扩展推送的 Cookie 已过期（超过 1 小时），请在浏览器中重新点击 bgi-dl 助手图标");
        return result;
    }

    const QJsonArray cookies = root.value(QStringLiteral("cookies")).toArray();
    for (const QJsonValue &value : cookies) {
        const QJsonObject object = value.toObject();
        const QString domain = object.value(QStringLiteral("domain")).toString().toLower();
        if (!cookieHostMatches(domain)) continue;
        const QByteArray name = object.value(QStringLiteral("name")).toString().toUtf8();
        const QByteArray cookieValue = object.value(QStringLiteral("value")).toString().toUtf8();
        if (name.isEmpty() || cookieValue.isEmpty()) continue;

        QNetworkCookie cookie(name, cookieValue);
        cookie.setDomain(domain);
        const QString cookiePath = object.value(QStringLiteral("path")).toString();
        cookie.setPath(cookiePath.isEmpty() ? QStringLiteral("/") : cookiePath);
        cookie.setSecure(object.value(QStringLiteral("secure")).toBool());
        cookie.setHttpOnly(object.value(QStringLiteral("httpOnly")).toBool());
        const qint64 expiry = static_cast<qint64>(
            object.value(QStringLiteral("expirationDate")).toDouble());
        if (expiry > 0) {
            const QDateTime expiration = QDateTime::fromSecsSinceEpoch(expiry, QTimeZone::UTC);
            if (expiration <= QDateTime::currentDateTimeUtc()) continue;
            cookie.setExpirationDate(expiration);
        }
        result.cookies.append(cookie);
    }
    if (result.cookies.isEmpty()) {
        result.error = MNET_TEXT("浏览器扩展缓存中没有 Mnet Plus Cookie（请在已登录 Mnet Plus 的浏览器中点击 bgi-dl 助手图标）");
    }
    return result;
}

QStringList BrowserCookieLoader::automaticCandidates()
{
    QStringList candidates;
    // The extension cache is preferred: it works on every browser and every
    // encryption regime, including Chrome 127+ app-bound encryption.
    if (QFileInfo::exists(extensionCachePath())) {
        candidates.append(QStringLiteral("extension"));
    }
    for (const ChromiumSpec &spec : chromiumSpecs()) {
        if (QFileInfo::exists(spec.root)) candidates.append(spec.id);
    }
    if (!firefoxProfilesIniPath().isEmpty()) candidates.append(QStringLiteral("firefox"));
#if defined(Q_OS_WIN)
    // Chromium can be installed per-user or machine-wide; include the configured
    // browser roots even when the directory is not present yet so an explicit
    // selection gets a useful diagnostic.
    if (candidates.isEmpty()) {
        for (const ChromiumSpec &spec : chromiumSpecs()) {
            if (!candidates.contains(spec.id)) candidates.append(spec.id);
        }
        candidates.append(QStringLiteral("firefox"));
    }
#endif
    return candidates;
}
