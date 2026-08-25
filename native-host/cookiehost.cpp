// bgi-dl Cookie Helper — native messaging host.
//
// Protocol: Chrome/Firefox native messaging over stdin/stdout.
//   - stdin: 4-byte little-endian length prefix + UTF-8 JSON payload
//   - stdout: same framing for replies
//
// The host writes received cookies to a well-known cache file that the
// Mnet Plus Downloader reads. One message per browser invocation; the host
// exits after replying.
//
// Implementation note: pure C stdio only — no <iostream>/<fstream>/<string>.
// The MinGW C++ runtime's iostream static initialization is fragile under
// some environments; plain C keeps the binary dependency-free (no
// libstdc++-6.dll / libgcc DLLs) and rock solid.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <shlobj.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t kMaxMessageSize = 1024 * 1024; // Chrome's native messaging limit

char *readAllInput(uint32_t *lengthOut)
{
    uint32_t length = 0;
    if (fread(&length, sizeof(length), 1, stdin) != 1) return nullptr;
    if (length == 0 || length > kMaxMessageSize) return nullptr;

    char *payload = static_cast<char *>(malloc(length + 1));
    if (!payload) return nullptr;
    uint32_t remaining = length;
    char *cursor = payload;
    while (remaining > 0) {
        const uint32_t want = remaining < 4096 ? remaining : 4096;
        const size_t got = fread(cursor, 1, want, stdin);
        if (got == 0) {
            free(payload);
            return nullptr;
        }
        cursor += got;
        remaining -= static_cast<uint32_t>(got);
    }
    payload[length] = '\0';
    *lengthOut = length;
    return payload;
}

void writeMessage(const char *body)
{
    const uint32_t length = static_cast<uint32_t>(strlen(body));
    fwrite(&length, sizeof(length), 1, stdout);
    fwrite(body, 1, length, stdout);
    fflush(stdout);
}

#if defined(_WIN32)
std::FILE *openCacheFile(const char **pathOut)
{
    char base[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) {
        return nullptr;
    }
    static char fullPath[MAX_PATH * 2];
    snprintf(fullPath, sizeof(fullPath), "%s\\bgi-dl", base);
    CreateDirectoryA(fullPath, nullptr);
    static char filePath[MAX_PATH * 2];
    snprintf(filePath, sizeof(filePath), "%s\\cookies.json", fullPath);
    *pathOut = filePath;
    return fopen(filePath, "wb");
}
#else
std::FILE *openCacheFile(const char **pathOut)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CACHE_HOME");
    static char filePath[4096];
    if (xdg && *xdg) {
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s/bgi-dl", xdg);
        mkdir(dir, 0755);
        snprintf(filePath, sizeof(filePath), "%s/cookies.json", dir);
    } else if (home && *home) {
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s/.cache/bgi-dl", home);
        mkdir(dir, 0755);
        snprintf(filePath, sizeof(filePath), "%s/cookies.json", dir);
    } else {
        return nullptr;
    }
    *pathOut = filePath;
    return fopen(filePath, "wb");
}
#endif

// Escapes a string for embedding in the JSON cache file.
void appendEscaped(char **buffer, size_t *size, size_t *used, const char *text, size_t textLength)
{
    for (size_t index = 0; index < textLength; ++index) {
        const char ch = text[index];
        char piece[8];
        const char *append = piece;
        size_t pieceLength = 1;
        switch (ch) {
        case '"': append = "\\\""; pieceLength = 2; break;
        case '\\': append = "\\\\"; pieceLength = 2; break;
        case '\n': append = "\\n"; pieceLength = 2; break;
        case '\r': append = "\\r"; pieceLength = 2; break;
        case '\t': append = "\\t"; pieceLength = 2; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                snprintf(piece, sizeof(piece), "\\u%04x", ch);
                pieceLength = 6;
            } else {
                piece[0] = ch;
                pieceLength = 1;
            }
        }
        if (*used + pieceLength + 1 > *size) {
            *size = (*size + pieceLength) * 2;
            *buffer = static_cast<char *>(realloc(*buffer, *size));
        }
        memcpy(*buffer + *used, append, pieceLength);
        *used += pieceLength;
    }
}

void appendRaw(char **buffer, size_t *size, size_t *used, const char *text)
{
    const size_t textLength = strlen(text);
    if (*used + textLength + 1 > *size) {
        *size = (*size + textLength) * 2;
        *buffer = static_cast<char *>(realloc(*buffer, *size));
    }
    memcpy(*buffer + *used, text, textLength);
    *used += textLength;
}

// Minimal JSON field extraction from the extension's message. The message
// shape is fixed (we control the extension), so a hand parser is fine.
// Returns the value length and sets *valueStart / 0 when not found.
size_t findStringField(const char *json, const char *key, const char **valueStart)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *at = strstr(json, needle);
    if (!at) return 0;
    const char *value = at + strlen(needle);
    *valueStart = value;
    size_t length = 0;
    while (value[length] && value[length] != '"') {
        if (value[length] == '\\' && value[length + 1]) ++length; // skip escaped char
        ++length;
    }
    return length;
}

// Extracts a number field; returns its parsed double value.
double findNumberField(const char *json, const char *key, bool *found)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(json, needle);
    if (!at) {
        if (found) *found = false;
        return 0.0;
    }
    if (found) *found = true;
    return strtod(at + strlen(needle), nullptr);
}

// Copies a raw field value into a NUL-terminated buffer with escapes resolved.
void decodeFieldValue(const char *value, size_t length, char *out, size_t outSize)
{
    size_t outIndex = 0;
    for (size_t index = 0; index < length && outIndex + 1 < outSize; ++index) {
        char ch = value[index];
        if (ch == '\\' && index + 1 < length) {
            ++index;
            switch (value[index]) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'u': {
                // \uXXXX: decode BMP codepoint to UTF-8
                if (index + 4 < length) {
                    char hex[5] = {value[index + 1], value[index + 2], value[index + 3], value[index + 4], 0};
                    const unsigned long codepoint = strtoul(hex, nullptr, 16);
                    index += 4;
                    if (codepoint < 0x80) {
                        ch = static_cast<char>(codepoint);
                    } else {
                        if (codepoint < 0x800) {
                            out[outIndex++] = static_cast<char>(0xC0 | (codepoint >> 6));
                            ch = static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else {
                            out[outIndex++] = static_cast<char>(0xE0 | (codepoint >> 12));
                            out[outIndex++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            ch = static_cast<char>(0x80 | (codepoint & 0x3F));
                        }
                        if (outIndex + 1 >= outSize) break;
                    }
                    out[outIndex++] = ch;
                    continue;
                }
                ch = '?';
                break;
            }
            default: ch = value[index]; break;
            }
        }
        out[outIndex++] = ch;
    }
    out[outIndex] = '\0';
}

} // namespace

int main()
{
    uint32_t payloadLength = 0;
    char *payload = readAllInput(&payloadLength);
    if (!payload) {
        writeMessage("{\"status\":\"error\",\"message\":\"bad request\"}");
        return 1;
    }

    const char *typeValue = nullptr;
    const size_t typeLength = findStringField(payload, "type", &typeValue);
    if (typeLength != 7 || strncmp(typeValue, "cookies", 7) != 0) {
        free(payload);
        writeMessage("{\"status\":\"error\",\"message\":\"unsupported message\"}");
        return 0;
    }

    const char *browserValue = nullptr;
    const size_t browserLength = findStringField(payload, "browser", &browserValue);
    char browser[64] = {0};
    if (browserLength > 0) {
        decodeFieldValue(browserValue, browserLength < 63 ? browserLength : 63, browser, sizeof(browser));
    }

    bool timestampFound = false;
    const double timestamp = findNumberField(payload, "timestamp", &timestampFound);

    // Build the cache JSON by scanning successive cookie objects.
    size_t cacheSize = 65536;
    size_t cacheUsed = 0;
    char *cache = static_cast<char *>(malloc(cacheSize));

    appendRaw(&cache, &cacheSize, &cacheUsed, "{\"browser\":\"");
    appendEscaped(&cache, &cacheSize, &cacheUsed, browser, strlen(browser));
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "\",\"timestamp\":%.0f,\"cookies\":[", timestampFound ? timestamp : 0.0);
    appendRaw(&cache, &cacheSize, &cacheUsed, stamp);

    unsigned long written = 0;
    const char *scan = payload;
    while (true) {
        const char *domainAt = strstr(scan, "\"domain\":\"");
        if (!domainAt) break;
        // Bound all field lookups to this cookie object: from domainAt up to
        // the next cookie's "domain" key (or end of payload).
        const char *nextDomain = strstr(domainAt + 10, "\"domain\":\"");
        const size_t objectLength = nextDomain
            ? static_cast<size_t>(nextDomain - domainAt)
            : strlen(domainAt);
        char *object = static_cast<char *>(malloc(objectLength + 1));
        memcpy(object, domainAt, objectLength);
        object[objectLength] = '\0';

        const char *domainValue = nullptr;
        const size_t domainLength = findStringField(object, "domain", &domainValue);
        const char *nameValue = nullptr;
        const size_t nameLength = findStringField(object, "name", &nameValue);
        const char *valueValue = nullptr;
        const size_t valueLength = findStringField(object, "value", &valueValue);
        const char *pathValue = nullptr;
        const size_t pathLength = findStringField(object, "path", &pathValue);
        bool secureFound = false;
        bool secure = findNumberField(object, "secure", &secureFound) != 0.0;
        if (!secure) secure = strstr(object, "\"secure\":true") != nullptr;
        const bool httpOnly = strstr(object, "\"httpOnly\":true") != nullptr;
        bool expiryFound = false;
        const double expiry = findNumberField(object, "expirationDate", &expiryFound);

        if (domainLength > 0 && nameLength > 0) {
            // Decode escape sequences first (the message payload is JSON), then
            // re-escape when writing — appending raw field text would double the
            // backslashes for values containing quotes or tabs.
            char domain[512]; char name[512]; char value[65536]; char path[1024];
            decodeFieldValue(domainValue, domainLength, domain, sizeof(domain));
            decodeFieldValue(nameValue, nameLength, name, sizeof(name));
            decodeFieldValue(valueValue, valueLength, value, sizeof(value));
            if (pathLength > 0) {
                decodeFieldValue(pathValue, pathLength, path, sizeof(path));
            } else {
                path[0] = '/'; path[1] = '\0';
            }
            if (written > 0) appendRaw(&cache, &cacheSize, &cacheUsed, ",");
            appendRaw(&cache, &cacheSize, &cacheUsed, "{\"domain\":\"");
            appendEscaped(&cache, &cacheSize, &cacheUsed, domain, strlen(domain));
            appendRaw(&cache, &cacheSize, &cacheUsed, "\",\"name\":\"");
            appendEscaped(&cache, &cacheSize, &cacheUsed, name, strlen(name));
            appendRaw(&cache, &cacheSize, &cacheUsed, "\",\"value\":\"");
            appendEscaped(&cache, &cacheSize, &cacheUsed, value, strlen(value));
            appendRaw(&cache, &cacheSize, &cacheUsed, "\",\"path\":\"");
            appendEscaped(&cache, &cacheSize, &cacheUsed, path, strlen(path));
            appendRaw(&cache, &cacheSize, &cacheUsed,
                      secure ? "\",\"secure\":true" : "\",\"secure\":false");
            appendRaw(&cache, &cacheSize, &cacheUsed,
                      httpOnly ? ",\"httpOnly\":true" : ",\"httpOnly\":false");
            snprintf(stamp, sizeof(stamp), ",\"expirationDate\":%.0f}", expiryFound ? expiry : 0.0);
            appendRaw(&cache, &cacheSize, &cacheUsed, stamp);
            ++written;
        }
        free(object);
        scan = domainAt + 10;
    }
    appendRaw(&cache, &cacheSize, &cacheUsed, "]}");
    cache[cacheUsed] = '\0';

    const char *cachePath = nullptr;
    std::FILE *cacheFile = openCacheFile(&cachePath);
    if (!cacheFile) {
        free(payload);
        free(cache);
        writeMessage("{\"status\":\"error\",\"message\":\"cannot write cache\"}");
        return 1;
    }
    fwrite(cache, 1, cacheUsed, cacheFile);
    fclose(cacheFile);
    free(payload);
    free(cache);

    char reply[64];
    snprintf(reply, sizeof(reply), "{\"status\":\"ok\",\"cookies\":%lu}", written);
    writeMessage(reply);
    return 0;
}
