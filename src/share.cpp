// Result SHARE upload implementation. See share.h for the contract and the privacy rule.
//
// Ported from analysis\share-protocol.md (recovered from the original's managed assembly)
// and validated live by the Lead. No Ookla binary is shipped or called; the salt that the
// original took from the native suite is ours to choose because the seed travels in
// X-Cache-Id and the server decodes with what it receives.
#include "share.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "http_client.h"
#include "json.h"

namespace st {
namespace {

// --- hashing / encoding primitives ----------------------------------------------------

bool Sha256(const unsigned char* data, std::size_t size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    DWORD objectLength = 0;
    DWORD written = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                      sizeof(objectLength), &written, 0);
    std::vector<unsigned char> object(objectLength ? objectLength : 512);
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr,
                               0, 0) == 0;
    if (ok && size > 0) {
        ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0;
    }
    if (ok) ok = BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool RandomBytes(unsigned char* out, std::size_t size) {
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string HexUpper(const unsigned char* data, std::size_t size) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

std::string HexLower(const unsigned char* data, std::size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

std::uint32_t Crc32(const std::vector<unsigned char>& data) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::string Base64(const std::vector<unsigned char>& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
        i += 3;
    }
    const std::size_t left = data.size() - i;
    if (left == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out += "==";
    } else if (left == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// --- gzip with DEFLATE stored blocks (Windows ships no gzip API) -----------------------

std::vector<unsigned char> GzipStored(const std::vector<unsigned char>& data) {
    std::vector<unsigned char> out;
    out.reserve(data.size() + 64);
    const unsigned char header[10] = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    out.insert(out.end(), header, header + 10);

    std::size_t offset = 0;
    if (data.empty()) {
        const unsigned char empty[5] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
        out.insert(out.end(), empty, empty + 5);
    }
    while (offset < data.size()) {
        const std::size_t chunk = std::min<std::size_t>(65535, data.size() - offset);
        const bool last = (offset + chunk) >= data.size();
        out.push_back(last ? 0x01 : 0x00);  // BFINAL | BTYPE=00 (stored)
        out.push_back(static_cast<unsigned char>(chunk & 0xFF));
        out.push_back(static_cast<unsigned char>((chunk >> 8) & 0xFF));
        const unsigned short nlen = static_cast<unsigned short>(~chunk);
        out.push_back(static_cast<unsigned char>(nlen & 0xFF));
        out.push_back(static_cast<unsigned char>((nlen >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                   data.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    }

    const std::uint32_t crc = Crc32(data);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((crc >> (8 * i)) & 0xFF));
    const std::uint32_t isize = static_cast<std::uint32_t>(data.size() & 0xFFFFFFFFu);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((isize >> (8 * i)) & 0xFF));

    out[8] = 0;  // XFL, forced like the original
    out[9] = 3;  // OS, forced like the original
    return out;
}

// Rolling XOR exactly as recovered: x is never reset and the seed only advances 32 times.
std::vector<unsigned char> RollingXor(std::vector<unsigned char> gz, const unsigned char seed[32]) {
    unsigned int j = 0;
    unsigned char x = 0;
    for (std::size_t i = 0; i < gz.size(); ++i) {
        if (j < 32) x ^= seed[j++];
        x ^= gz[i];
        gz[i] = x;
    }
    return gz;
}

// --- local identity / time ------------------------------------------------------------

std::wstring RegString(const wchar_t* subkey, const wchar_t* value) {
    wchar_t buffer[512] = {0};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     &type, buffer, &size) != ERROR_SUCCESS) {
        return std::wstring();
    }
    return std::wstring(buffer);
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::string ComputerName() {
    wchar_t buffer[256] = {0};
    DWORD size = 256;
    if (!GetComputerNameW(buffer, &size)) return "windows-pc";
    return Narrow(buffer);
}

std::string OsVersionString() {
    const std::wstring product =
        RegString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    const std::wstring build =
        RegString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
    std::string os = Narrow(product);
    if (os.empty()) os = "Windows";
    if (!build.empty()) os += " (" + Narrow(build) + ")";
    return os;
}

// Stable per-machine id, shaped like Unity's deviceUniqueIdentifier (40 hex chars).
std::string DeviceId() {
    const std::string material = "speedtest-reimpl|" + ComputerName() + "|" + OsVersionString();
    unsigned char digest[32] = {0};
    if (!Sha256(reinterpret_cast<const unsigned char*>(material.data()), material.size(), digest)) {
        return std::string(40, '0');
    }
    return HexLower(digest, 32).substr(0, 40);
}

std::string Iso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// start.timestamp / end.timestamp are DECIMAL STRINGS. Measured against the live backend:
// it renders from_unixtime(<value>/1000), and our integer Unix SECONDS produced
// from_unixtime(1791924.266) = 1970-01-21, so the wire value is Unix MILLISECONDS.
std::string UnixMillis(std::chrono::system_clock::time_point tp) {
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    return std::to_string(ms);
}

struct TimezoneInfo {
    std::string name;
    std::string offset;  // +HH:MM
};

TimezoneInfo LocalTimezone() {
    TIME_ZONE_INFORMATION tz{};
    const DWORD state = GetTimeZoneInformation(&tz);
    long bias = tz.Bias;
    if (state == TIME_ZONE_ID_DAYLIGHT) {
        bias += tz.DaylightBias;
    } else if (state == TIME_ZONE_ID_STANDARD) {
        bias += tz.StandardBias;
    }
    const long minutes = -bias;
    const long absMinutes = minutes < 0 ? -minutes : minutes;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%02ld:%02ld", minutes < 0 ? '-' : '+', absMinutes / 60,
                  absMinutes % 60);
    TimezoneInfo info;
    info.offset = buf;
    const wchar_t* source = (state == TIME_ZONE_ID_DAYLIGHT && tz.DaylightName[0] != L'\0')
                                ? tz.DaylightName
                                : tz.StandardName;
    info.name = Narrow(source);
    if (info.name.empty()) info.name = "UTC";
    return info;
}

std::string Num(double v, int decimals = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

// --- payload ---------------------------------------------------------------------------

int ServerIdOf(const Result& result, const ShareContext& ctx) {
    if (ctx.serverId > 0) return ctx.serverId;
    if (!result.server.id.empty()) {
        const int v = std::atoi(result.server.id.c_str());
        if (v > 0) return v;
    }
    return 0;
}

double MinimumPing(const Result& result) {
    if (!result.pings.empty()) return *std::min_element(result.pings.begin(), result.pings.end());
    return result.pingMs > 0 ? result.pingMs : 0.0;
}

// The reports backend refuses a null ip_address, so the payload needs the client IP that
// Ookla already publishes at its config endpoint (<client ip="...">). This is the same host
// the engine uses for discovery; the share path only reaches it when the user shares.
std::string DiscoverExternalIp(const std::string& configUrl) {
    HttpRequest req;
    req.url = configUrl;
    req.timeoutMs = 15000;
    req.maxBytes = 512 * 1024;
    const HttpResult resp = HttpPerform(req);
    if (!resp.ok) return std::string();
    const std::size_t at = resp.body.find("<client");
    if (at == std::string::npos) return std::string();
    const std::size_t end = resp.body.find('>', at);
    if (end == std::string::npos) return std::string();
    const std::string tag = resp.body.substr(at, end - at);
    const std::size_t ip = tag.find("ip=\"");
    if (ip == std::string::npos) return std::string();
    const std::size_t start = ip + 4;
    const std::size_t close = tag.find('"', start);
    if (close == std::string::npos) return std::string();
    return tag.substr(start, close - start);
}

// Real throughput shape (ResultDataCollectorHelper::CreateResultThroughputJSON):
//   {"bytes": int64 BYTES, "elapsed": int64 MILLISECONDS, "speed": int64 BYTES PER SECOND}
// "bandwidth", "testpoints" and "latency" do not exist in that object; the backend divides
// speed by 125 for kbps. "scaling" is optional and only emitted when we know finalConns.
std::string ThroughputJson(std::uint64_t bytes, double seconds, int finalConns) {
    const double secs = seconds > 0.0 ? seconds : 0.0;
    const long long elapsedMs = static_cast<long long>(secs * 1000.0 + 0.5);
    const long long speed =
        secs > 0.0 ? static_cast<long long>(static_cast<double>(bytes) / secs + 0.5) : 0;
    std::string j = "{\"bytes\":" + std::to_string(bytes);
    j += ",\"elapsed\":" + std::to_string(elapsedMs);
    j += ",\"speed\":" + std::to_string(speed);
    if (finalConns > 0) j += ",\"scaling\":{\"finalConns\":" + std::to_string(finalConns) + "}";
    j += "}";
    return j;
}

// One Ookla::PingDetails entry for serverSelection.closestPingDetails. Keys are the eight
// fields the original exposes: latency, jitter, protocol, remoteIp, server, pings, success,
// tcpError.
std::string PingDetailsJson(const Result& result) {
    std::string j = "{\"latency\":" + Num(result.pingMs > 0 ? result.pingMs : 0.0);
    j += ",\"jitter\":" + Num(result.jitterMs > 0 ? result.jitterMs : 0.0);
    j += ",\"protocol\":\"" +
         (result.protocol.empty() ? std::string("HTTP") : result.protocol) + "\"";
    j += ",\"remoteIp\":\"" + result.remoteIp + "\"";
    j += ",\"server\":\"" + result.server.host + "\"";
    j += ",\"pings\":" + std::to_string(static_cast<int>(result.pings.size()));
    j += ",\"success\":" + std::to_string(result.pingsSuccess > 0 ? result.pingsSuccess : 0);
    j += ",\"tcpError\":" + std::string(result.tcpError ? "true" : "false") + "}";
    return j;
}

std::string BuildBody(const Result& result, const ShareContext& ctx, const std::string& guidHex,
                      const std::string& deviceId, const std::string& osVersion,
                      const std::string& retrievedAtIso, const std::string& startStamp,
                      const std::string& endStamp, const std::string& externalIp) {
    const int serverId = ServerIdOf(result, ctx);

    std::string app = "{\"version\":\"" + ctx.appVersion + "\",\"storeVersion\":\"" +
                      ctx.appVersion + "\",\"name\":\"Speedtest\",\"unityInfo\":{\"systemInfo\":{},";
    app += "\"applicationInfo\":{\"identifier\":\"com.ookla.speedtest\",\"platform\":"
           "\"WindowsPlayer\"}}}";

    std::string config = "{\"retrievedAt\":\"" + retrievedAtIso + "\",\"testMethod\":\"tcp\",\"server\":{";
    config += "\"id\":" + std::to_string(serverId) + ",\"selectionMethod\":\"automatic\"}";
    config += ",\"externalIp\":\"" + externalIp + "\",\"configTag\":\"\"}";

    const std::string computer = ComputerName();
    std::string device = "{\"platform\":\"windows\",\"id\":\"" + deviceId + "\",";
    device += "\"systemFamily\":\"Windows\",\"version\":\"" + osVersion + "\",";
    device += "\"architecture\":\"x64\",\"manufacturer\":\"" + computer + "\",";
    device += "\"model\":\"" + computer + "\"}";

    const std::string user = "{\"userId\":\"" + deviceId + "\"}";

    std::string latency = "{\"minimum\":" + Num(MinimumPing(result));
    latency += ",\"jitter\":" + Num(result.jitterMs > 0 ? result.jitterMs : 0.0);
    latency += ",\"pings\":[";
    for (std::size_t i = 0; i < result.pings.size(); ++i) {
        if (i) latency += ",";
        latency += Num(result.pings[i]);
    }
    latency += "]}";

    const std::string packetLoss = "{\"sent\":0,\"received\":0}";

    // Ookla::ServerSelectionResult::toJsonTree (VA 0x1800f2540): closestServerId,
    // closestPingDetails, elapsed, timestamp. Our old serverId/selectionMethod keys did not
    // exist in the real payload.
    double pingSum = 0.0;
    for (const double v : result.pings) pingSum += v;
    const long long selectionElapsedMs = static_cast<long long>(pingSum + 0.5);
    std::string serverSelection = "{\"closestServerId\":" + std::to_string(serverId);
    serverSelection += ",\"closestPingDetails\":[" + PingDetailsJson(result) + "]";
    serverSelection += ",\"elapsed\":" + std::to_string(selectionElapsedMs);
    serverSelection += ",\"timestamp\":\"" + startStamp + "\"}";

    // speed is BYTES per second; the backend does the kbps conversion.
    const std::string download =
        ThroughputJson(result.downloadBytes, result.downloadSeconds, result.downloadConnectionsPeak);
    // Upload is NESTED by CreateUploadSection. We measure the client's send, so "client" is
    // the primary; suiteStats is null, so there is no secondary reading.
    const std::string upload = "{\"client\":" +
                               ThroughputJson(result.uploadBytes, result.uploadSeconds, 0) +
                               ",\"primary\":\"client\"}";

    // Exact field order from the IL; every value is raw pre-serialized JSON, not a string.
    std::string body = "[{\"guid\":\"" + guidHex + "\"";
    body += ",\"app\":" + app;
    body += ",\"config\":" + config;
    body += ",\"device\":" + device;
    body += ",\"user\":" + user;
    // start/end carry a decimal Unix-seconds timestamp plus timezone name and offset.
    const TimezoneInfo tz = LocalTimezone();
    body += ",\"start\":{\"timestamp\":\"" + startStamp + "\",\"timezone\":{\"name\":\"" +
            tz.name + "\",\"offset\":\"" + tz.offset + "\"}}";
    body += ",\"end\":{\"timestamp\":\"" + endStamp + "\",\"timezone\":{\"name\":\"" + tz.name +
            "\",\"offset\":\"" + tz.offset + "\"}}";
    body += ",\"packetLoss\":" + packetLoss;
    body += ",\"serverSelection\":" + serverSelection;
    body += ",\"latency\":" + latency;
    body += ",\"download\":" + download;
    body += ",\"upload\":" + upload;
    body += ",\"error\":null}]";
    return body;
}

std::string DashedGuid(const std::string& hex) {
    if (hex.size() != 32) return hex;
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

}  // namespace

ShareOutcome ShareUpload(const Result& result, const ShareContext& ctx) {
    ShareOutcome outcome;

    // 1) Mint the guid. We send the 32-hex form (no dashes); the share URL uses the dashed
    //    form, preferring the guid the server echoes back.
    unsigned char guidBytes[16] = {0};
    if (!RandomBytes(guidBytes, sizeof(guidBytes))) {
        outcome.detail = "BCryptGenRandom failed for the guid";
        return outcome;
    }
    guidBytes[6] = static_cast<unsigned char>((guidBytes[6] & 0x0F) | 0x40);  // version 4
    guidBytes[8] = static_cast<unsigned char>((guidBytes[8] & 0x3F) | 0x80);  // variant
    // Faithful form: TestRunner::Init uses Guid.NewGuid().ToString(), whose "D" format is
    // dashed 8-4-4-4-12. The same dashed string goes in the body and in the my-result link.
    const std::string guidHex = HexLower(guidBytes, sizeof(guidBytes));
    const std::string guidDashed = DashedGuid(guidHex);

    const std::string deviceId = ctx.deviceId.empty() ? DeviceId() : ctx.deviceId;
    const std::string osVersion = ctx.osVersion.empty() ? OsVersionString() : ctx.osVersion;

    const auto now = std::chrono::system_clock::now();
    const double measuredSeconds = (result.downloadSeconds > 0 ? result.downloadSeconds : 0.0) +
                                   (result.uploadSeconds > 0 ? result.uploadSeconds : 0.0);
    const std::string retrievedAtIso = Iso8601(now);
    const std::string endStamp = ctx.endIso.empty() ? UnixMillis(now) : ctx.endIso;
    const std::string startStamp =
        ctx.startIso.empty()
            ? UnixMillis(now -
                         std::chrono::milliseconds(static_cast<long long>(measuredSeconds * 1000)))
            : ctx.startIso;

    // 2) Body, gzip (stored blocks), rolling XOR, Base64.
    const std::string externalIp =
        ctx.externalIp.empty() ? DiscoverExternalIp(ctx.configUrl) : ctx.externalIp;
    const std::string body = BuildBody(result, ctx, guidDashed, deviceId, osVersion,
                                       retrievedAtIso, startStamp, endStamp, externalIp);
    const std::vector<unsigned char> plain(body.begin(), body.end());
    const std::vector<unsigned char> gz = GzipStored(plain);

    // 3) seed = UPPER(SHA256(body + salt)); the salt is ours (the server decodes with what
    //    it receives, so the original's native RandomString is not needed).
    unsigned char saltRandom[16] = {0};
    RandomBytes(saltRandom, sizeof(saltRandom));
    const std::string salt =
        ctx.appVersion + ":" + deviceId + ":" + HexLower(saltRandom, sizeof(saltRandom));
    const std::string seedInput = body + salt;
    unsigned char digest[32] = {0};
    if (!Sha256(reinterpret_cast<const unsigned char*>(seedInput.data()), seedInput.size(), digest)) {
        outcome.detail = "BCrypt SHA-256 failed";
        return outcome;
    }
    const std::string seedHex = HexUpper(digest, sizeof(digest));

    const std::vector<unsigned char> obfuscated = RollingXor(gz, digest);
    const std::string payload = Base64(obfuscated);

    // 4) POST.
    HttpRequest req;
    req.url = ctx.endpoint;
    req.method = "POST";
    req.postBody = payload;
    req.contentType = "application/octet-stream; charset=utf-8";
    req.timeoutMs = 25000;
    req.maxBytes = 64 * 1024;
    req.userAgent = "SpeedTest/" + ctx.appVersion + "/" + osVersion;
    req.headers.push_back({"X-Cache-Id", seedHex});
    req.headers.push_back({"X-Transfer-Encoding", "base64"});

    const HttpResult resp = HttpPerform(req);
    outcome.detail = resp.body.empty() ? (resp.error.empty() ? "no response body" : resp.error)
                                       : resp.body;

    if (!resp.ok) {
        if (resp.error.empty()) {
            outcome.detail = "http status " + std::to_string(resp.status) + ": " + outcome.detail;
        }
        return outcome;
    }

    // 5) Success = the element reports status "ok". The endpoint answers with an ARRAY of
    //    one element, so accept either shape. Surface the raw text either way.
    JsonValue root;
    std::string parseError;
    if (!JsonParse(resp.body, root, &parseError)) {
        outcome.detail = "response was not JSON (" + parseError + "): " + resp.body;
        return outcome;
    }
    const JsonValue* element = &root;
    if (root.isArray()) {
        if (root.items.empty()) {
            outcome.detail = "response array was empty: " + resp.body;
            return outcome;
        }
        element = &root.items.front();
    }
    if (!element->isObject()) {
        outcome.detail = "response element was not an object: " + resp.body;
        return outcome;
    }
    const JsonValue* status = element->find("status");
    if (!status || status->strOr() != "ok") {
        return outcome;  // detail already carries the server's own error text
    }
    outcome.ok = true;

    std::string echoedGuid;
    if (const JsonValue* g = element->find("guid")) echoedGuid = g->strOr();
    // The server echoes the guid dashed; normalise whichever form came back so the share
    // URL always uses the dashed form the website expects.
    std::string urlGuid = echoedGuid.empty() ? guidDashed : echoedGuid;
    if (urlGuid.find('-') == std::string::npos) urlGuid = DashedGuid(urlGuid);
    outcome.url = "https://www.speedtest.net/my-result/d/" + urlGuid;
    return outcome;
}

}  // namespace st
