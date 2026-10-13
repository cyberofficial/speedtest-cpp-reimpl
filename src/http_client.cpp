#include "http_client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace st {
namespace {

constexpr const wchar_t* kDefaultAgent = L"SpeedtestCpp/1.0 (reimpl)";

// One WinHTTP session per User-Agent string. The config endpoint is sensitive to the
// agent: no agent or a complete realistic agent returns 200, a bare "Mozilla/5.0"
// returns 403. Nothing here ever sends a bare "Mozilla/5.0".
HINTERNET SessionFor(const std::string& agent) {
    static std::mutex mutex;
    static std::map<std::string, HINTERNET> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(agent);
    if (it != cache.end()) return it->second;

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
    const std::wstring wide = ToWide(agent);
    const wchar_t* name = agent == "-" ? L"" : (agent.empty() ? kDefaultAgent : wide.c_str());
    HINTERNET session = WinHttpOpen(name, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(name, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session) {
        // The engine scales up to the server-advertised connection count, so lift the
        // WinHTTP per-server pool cap (otherwise parallel streams share too few sockets).
        DWORD maxConns = 64;
        WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxConns, sizeof(maxConns));
        WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &maxConns,
                         sizeof(maxConns));
    }
    cache[agent] = session;
    return session;
}

struct HandleGuard {
    HINTERNET h = nullptr;
    explicit HandleGuard(HINTERNET handle) : h(handle) {}
    ~HandleGuard() {
        if (h) WinHttpCloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

double NowSeconds() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

std::string LastErrorText(const char* what) {
    return std::string(what) + " failed (win32 error " + std::to_string(GetLastError()) + ")";
}

}  // namespace

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

Url ParseUrl(const std::string& url) {
    Url u;
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return u;
    const std::string scheme = url.substr(0, schemeEnd);
    if (scheme == "https") {
        u.https = true;
    } else if (scheme != "http") {
        return u;
    }
    const size_t rest = schemeEnd + 3;
    const size_t slash = url.find('/', rest);
    std::string authority =
        url.substr(rest, slash == std::string::npos ? std::string::npos : slash - rest);
    u.path = (slash == std::string::npos) ? "/" : url.substr(slash);
    const size_t at = authority.find('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return u;

    std::string host = authority;
    std::uint16_t port = u.https ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        host = authority.substr(0, colon);
        const int v = std::atoi(authority.c_str() + colon + 1);
        if (v > 0 && v < 65536) port = static_cast<std::uint16_t>(v);
    }
    if (host.empty()) return u;
    u.host = host;
    u.port = port;
    u.origin = scheme + "://" + host;
    const std::uint16_t defPort = u.https ? 443 : 80;
    if (port != defPort) u.origin += ":" + std::to_string(port);
    u.valid = true;
    return u;
}

HttpResult HttpPerform(const HttpRequest& req) {
    HttpResult res;
    const Url url = ParseUrl(req.url);
    if (!url.valid) {
        res.error = "invalid url: " + req.url;
        return res;
    }
    HINTERNET session = SessionFor(req.userAgent);
    if (!session) {
        res.error = "WinHttpOpen failed";
        return res;
    }
    WinHttpSetTimeouts(session, req.timeoutMs, req.timeoutMs, req.timeoutMs, req.timeoutMs);

    const std::wstring hostW = ToWide(url.host);
    HandleGuard conn(WinHttpConnect(session, hostW.c_str(), url.port, 0));
    if (!conn.h) {
        res.error = LastErrorText("WinHttpConnect");
        return res;
    }

    const std::wstring methodW = ToWide(req.method);
    const std::wstring pathW = ToWide(url.path);
    HandleGuard request(WinHttpOpenRequest(conn.h, methodW.c_str(), pathW.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) {
        res.error = LastErrorText("WinHttpOpenRequest");
        return res;
    }

    if (req.method == "GET") {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    } else {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }

    std::string headers = "Accept: */*\r\nAccept-Encoding: identity\r\n";
    if (req.method == "POST") headers += "Content-Type: " + req.contentType + "\r\n";
    for (const auto& kv : req.headers) headers += kv.first + ": " + kv.second + "\r\n";
    const std::wstring headersW = ToWide(headers);

    const double t0 = NowSeconds();
    BOOL sent = FALSE;
    if (req.method == "POST") {
        sent = WinHttpSendRequest(request.h, headersW.c_str(), static_cast<DWORD>(-1L),
                                  const_cast<char*>(req.postBody.data()),
                                  static_cast<DWORD>(req.postBody.size()),
                                  static_cast<DWORD>(req.postBody.size()), 0);
    } else {
        sent = WinHttpSendRequest(request.h, headersW.c_str(), static_cast<DWORD>(-1L),
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }
    if (!sent) {
        res.error = LastErrorText("WinHttpSendRequest");
        res.seconds = NowSeconds() - t0;
        return res;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        res.error = LastErrorText("WinHttpReceiveResponse");
        res.seconds = NowSeconds() - t0;
        return res;
    }

    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    res.status = static_cast<int>(status);

    const std::size_t requested = req.bufferBytes != 0 ? static_cast<std::size_t>(req.bufferBytes)
                                                       : static_cast<std::size_t>(64 * 1024);
    std::vector<char> buf(std::max<std::size_t>(requested, 4096));
    std::uint64_t total = 0;
    bool readError = false;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            res.error = LastErrorText("WinHttpQueryDataAvailable");
            readError = true;
            break;
        }
        if (available == 0) break;
        const DWORD want = static_cast<DWORD>(
            std::min<std::uint64_t>(available, static_cast<std::uint64_t>(buf.size())));
        if (want == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), want, &got)) {
            res.error = LastErrorText("WinHttpReadData");
            readError = true;
            break;
        }
        if (got == 0) break;
        total += got;
        if (!req.discardBody) {
            if (req.maxBytes != 0 && res.body.size() + got > req.maxBytes) {
                const size_t take = static_cast<size_t>(req.maxBytes - res.body.size());
                res.body.append(buf.data(), take);
            } else {
                res.body.append(buf.data(), got);
            }
        }
        const double elapsed = NowSeconds() - t0;
        if (req.onChunk && !req.onChunk(total, elapsed)) {
            res.aborted = true;
            break;
        }
    }

    res.bytes = total;
    res.seconds = NowSeconds() - t0;
    if (readError) return res;
    res.ok = res.status >= 200 && res.status < 300;
    if (!res.ok && res.error.empty()) res.error = "http status " + std::to_string(res.status);
    return res;
}

}  // namespace st
