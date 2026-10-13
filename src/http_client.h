// Minimal WinHTTP client used by the measurement engine.
// No third-party code: only OS-shipped WinHTTP.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <functional>
#include <string>

namespace st {

struct Url {
    bool valid = false;
    bool https = false;
    std::string host;    // ASCII host, no brackets
    std::uint16_t port = 0;
    std::string path;    // path + query
    std::string origin;  // scheme://host[:port]
};

Url ParseUrl(const std::string& url);
std::wstring ToWide(const std::string& s);

struct HttpResult {
    bool ok = false;
    bool aborted = false;  // caller callback asked to stop
    int status = 0;
    std::string body;
    std::string error;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
};

struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string postBody;
    std::string contentType = "application/octet-stream";
    int timeoutMs = 20000;
    std::uint64_t maxBytes = 0;  // cap on stored body, 0 = unlimited
    bool discardBody = false;    // do not retain payload (throughput mode)
    std::uint32_t bufferBytes = 0;  // socket read buffer hint, 0 = 64 KiB default
    // "" = client default agent; "-" = no User-Agent header. The config endpoint
    // returns 403 for a bare "Mozilla/5.0", so only use a complete agent string.
    std::string userAgent;
    // Extra request headers (name, value), e.g. X-Cache-Id for the share upload.
    std::vector<std::pair<std::string, std::string>> headers;
    // bytesSoFar = bytes of this request, secondsElapsed = wall clock.
    // Return false to abort the transfer early (bytes so far are still reported).
    std::function<bool(std::uint64_t bytesSoFar, double secondsElapsed)> onChunk;
};

HttpResult HttpPerform(const HttpRequest& req);

}  // namespace st
