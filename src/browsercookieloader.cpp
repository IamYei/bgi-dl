#include "browsercookieloader.h"
#include "localization.h"

#include <QtConcurrent>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
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
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    return {
        {QStringLiteral("chrome"), local + QStringLiteral("/Google/Chrome/User Data"), {}},
        {QStringLiteral("edge"), local + QStringLiteral("/Microsoft/Edge/User Data"), {}},
    };
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
    if (!QFileInfo::exists(source) || !QFile::copy(source, destination)) return false;
    if (QFileInfo::exists(source + QStringLiteral("-wal"))) {
        QFile::copy(source + QStringLiteral("-wal"), destination + QStringLiteral("-wal"));
    }
    if (QFileInfo::exists(source + QStringLiteral("-shm"))) {
        QFile::copy(source + QStringLiteral("-shm"), destination + QStringLiteral("-shm"));
    }
    return true;
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
    if (!encrypted.startsWith("v10")) return encrypted;
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
    if (encrypted.size() >= 3 && encrypted.at(0) == 'v') {
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
    return encrypted.startsWith("v10") ? QByteArray{} : encrypted;
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
            query.addBindValue(QStringLiteral("mnetplus.world"));
            query.addBindValue(QStringLiteral("%.mnetplus.world"));
            if (query.exec()) {
                while (query.next()) {
                    const QString host = query.value(0).toString();
                    if (!isMnetDomain(host)) continue;
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
    emit loadingBrowser(browser);
    m_watcher.setFuture(QtConcurrent::run([browser] {
        return loadSync(browser);
    }));
}

void BrowserCookieLoader::cancel()
{
    m_ignoreResult = true;
}

CookieLoadResult BrowserCookieLoader::loadSync(const QString &requestedBrowser)
{
    const QStringList candidates = requestedBrowser == QStringLiteral("auto")
        ? automaticCandidates() : QStringList{requestedBrowser};
    QStringList errors;
    CookieLoadResult bestResult;
    int bestScore = -1;
    for (const QString &browser : candidates) {
        const CookieLoadResult result = loadChromium(browser);
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
    for (const QString &profile : chromiumProfiles(spec.root)) {
        QString source = QDir(profile).filePath(QStringLiteral("Network/Cookies"));
        if (!QFileInfo::exists(source)) source = QDir(profile).filePath(QStringLiteral("Cookies"));
        if (!QFileInfo::exists(source)) continue;

        QTemporaryDir temporary;
        if (!temporary.isValid()) continue;
        const QString copy = temporary.filePath(QStringLiteral("Cookies"));
        if (!copySqliteBundle(source, copy)) continue;
        result.cookies = queryChromiumCookies(copy, key);
        if (!result.cookies.isEmpty()) return result;
    }
    result.error = key.isEmpty()
        ? MNET_TEXT("无法读取 %1 的浏览器解密密钥").arg(browser)
        : MNET_TEXT("%1 中没有 Mnet Plus Cookie").arg(browser);
    return result;
}

QStringList BrowserCookieLoader::automaticCandidates()
{
    QStringList candidates;
    for (const ChromiumSpec &spec : chromiumSpecs()) {
        if (QFileInfo::exists(spec.root)) candidates.append(spec.id);
    }
#if defined(Q_OS_WIN)
    // Chromium can be installed per-user or through the portable channel; include
    // the configured browser roots even when the directory is not present yet so
    // an explicit selection gets a useful diagnostic.
    if (candidates.isEmpty()) {
        for (const ChromiumSpec &spec : chromiumSpecs()) {
            if (!candidates.contains(spec.id)) candidates.append(spec.id);
        }
    }
#endif
    return candidates;
}
