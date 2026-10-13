#include "loopback_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace st {
namespace {

constexpr std::uint64_t kDownloadBodyBytes = 2ull * 1024ull * 1024ull;  // per download request
constexpr int kWorkers = 8;
constexpr int kRecvTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 10000;
constexpr std::size_t kMaxHeaderBytes = 16 * 1024;

bool SendAll(SOCKET s, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const int chunk = static_cast<int>(std::min<std::size_t>(len - off, 1u << 20));
        const int n = send(s, data + off, chunk, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Debug aid: SPEEDTEST_LOOPBACK_DEBUG=1 prints why a request was rejected.
bool DebugEnabled() {
    static const bool on = []() {
        const char* v = std::getenv("SPEEDTEST_LOOPBACK_DEBUG");
        return v != nullptr && *v != '\0' && *v != '0';
    }();
    return on;
}

std::string Snippet(const std::string& raw, std::size_t limit = 160) {
    std::string out;
    for (std::size_t i = 0; i < raw.size() && i < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.');
    }
    return out;
}

void NoteMalformed(const char* reason, const std::string& raw) {
    if (!DebugEnabled()) return;
    std::fprintf(stderr, "[loopback] rejected (%s) bytes=%zu raw=%s\n", reason, raw.size(),
                 Snippet(raw).c_str());
    std::fflush(stderr);
}

std::uint64_t ParseContentLength(const std::string& head) {
    const std::string lower = Lower(head);
    const std::string key = "content-length:";
    const std::size_t at = lower.find(key);
    if (at == std::string::npos) return 0;
    return std::strtoull(head.c_str() + at + key.size(), nullptr, 10);
}

bool SendResponse(SOCKET s, int code, const char* reason, const char* ctype, const std::string& body) {
    char header[512];
    const int n = std::snprintf(header, sizeof(header),
                                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                                "Connection: keep-alive\r\n\r\n",
                                code, reason, ctype, body.size());
    if (n <= 0 || !SendAll(s, header, static_cast<std::size_t>(n))) return false;
    if (!body.empty() && !SendAll(s, body.data(), body.size())) return false;
    return true;
}

bool SendDownloadHeaders(SOCKET s, std::uint64_t bytes) {
    char header[256];
    const int n = std::snprintf(header, sizeof(header),
                                "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                                "Content-Length: %llu\r\nConnection: keep-alive\r\n\r\n",
                                static_cast<unsigned long long>(bytes));
    return n > 0 && SendAll(s, header, static_cast<std::size_t>(n));
}

std::string ServerListJson(std::uint16_t port) {
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    std::string j = "[{\"url\":\"http://" + authority + "/speedtest/upload.php\",";
    j += "\"lat\":\"40.7128\",\"lon\":\"-74.0060\",\"name\":\"Loopback Lab, NY\",";
    j += "\"country\":\"United States\",\"cc\":\"US\",\"sponsor\":\"Loopback Lab\",";
    j += "\"id\":\"9999\",\"host\":\"" + authority + "\"}]";
    return j;
}

// Mirrors the real /api/windows/config.php shape (XML, <server> elements with
// url/lat/lon/name/sponsor/id/sponsor_adj/host) so the offline test exercises the
// same parser and the same primary discovery path as the real endpoint.
std::string ConfigXml(std::uint16_t port) {
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<settings>\n";
    x += " <config enableServerUploadMeasurement=\"true\" pping=\"true\">\n";
    x += "  <dynamic stopCount=\"9\" femaPeriod=\"12\" semaPeriod=\"26\" endStopEnabled=\"true\"";
    x += " enableDownloadConnScaling=\"true\" downloadConnScalingMaxPerAttempt=\"24\"";
    x += " downloadMaxConnections=\"24\" downloadConnScalingWindowSize=\"100000\"/>\n";
    x += " </config>\n <servers>\n";
    x += "  <server url=\"http://" + authority + "/speedtest/upload.php\" lat=\"40.7128\"";
    x += " lon=\"-74.0060\" name=\"Loopback Lab, NY\" sponsor=\"Loopback Lab\" id=\"9999\"";
    x += " sponsor_adj=\"0\" host=\"" + authority + "\"/>\n";
    x += " </servers>\n <packetLoss delayMS=\"50\"/>\n</settings>\n";
    return x;
}

}  // namespace

LoopbackServer::~LoopbackServer() {
    Stop();
}

bool LoopbackServer::Start(std::string* err) {
    auto fail = [&](const char* what) {
        if (err) {
            *err = std::string(what) + " failed (winsock error " + std::to_string(WSAGetLastError()) + ")";
        }
        return false;
    };

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (err) *err = "WSAStartup failed";
        return false;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return fail("socket");
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return fail("bind");
    }
    int addrLen = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return fail("getsockname");
    }
    if (listen(listener, 64) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return fail("listen");
    }
    port_ = ntohs(addr.sin_port);
    listenSock_ = static_cast<std::uintptr_t>(listener);
    stop_.store(false);

    acceptThread_ = std::thread([this]() { AcceptLoop(); });
    for (int i = 0; i < kWorkers; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
    return true;
}

void LoopbackServer::Stop() {
    if (listenSock_ == ~std::uintptr_t(0)) return;
    stop_.store(true);
    const SOCKET listener = static_cast<SOCKET>(listenSock_);
    listenSock_ = ~std::uintptr_t(0);
    closesocket(listener);
    queueCv_.notify_all();
    if (acceptThread_.joinable()) acceptThread_.join();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        for (const std::uintptr_t s : queue_) closesocket(static_cast<SOCKET>(s));
        queue_.clear();
    }
    WSACleanup();
}

std::string LoopbackServer::baseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

void LoopbackServer::AcceptLoop() {
    const SOCKET listener = static_cast<SOCKET>(listenSock_);
    for (;;) {
        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == INVALID_SOCKET) break;
        if (stop_.load()) {
            closesocket(client);
            break;
        }
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        const DWORD rtv = kRecvTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rtv), sizeof(rtv));
        const DWORD stv = kSendTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&stv), sizeof(stv));
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(static_cast<std::uintptr_t>(client));
        }
        queueCv_.notify_one();
    }
}

void LoopbackServer::WorkerLoop() {
    for (;;) {
        std::uintptr_t sock = 0;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this]() { return stop_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_.load()) return;
                continue;
            }
            sock = queue_.front();
            queue_.pop_front();
        }
        HandleConnection(sock);
        closesocket(static_cast<SOCKET>(sock));
    }
}

// Keeps the connection open between requests: the client (WinHTTP) reuses it,
// which removes the reconnect race that otherwise stalls an occasional POST.
void LoopbackServer::HandleConnection(std::uintptr_t sockRaw) {
    const SOCKET s = static_cast<SOCKET>(sockRaw);
    std::string buffered;
    char tmp[64 * 1024];

    for (;;) {
        std::size_t headerEnd = buffered.find("\r\n\r\n");
        while (headerEnd == std::string::npos) {
            const int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                // A client that closes mid-request (the engine stops by closing its
                // in-flight connections) is an abort, not a malformed request.
                if (!buffered.empty()) clientAborts_.fetch_add(1);
                return;
            }
            buffered.append(tmp, static_cast<std::size_t>(n));
            // Search for the terminator BEFORE applying the header cap: one recv can
            // legitimately return the complete headers plus the start of the body.
            headerEnd = buffered.find("\r\n\r\n");
            if (headerEnd == std::string::npos && buffered.size() > kMaxHeaderBytes) {
                badReqs_.fetch_add(1);
                if (DebugEnabled()) {
                    const std::string probe =
                        Lower(buffered.substr(0, std::min<std::size_t>(buffered.size(), 4096)));
                    const std::size_t term = buffered.find("\r\n\r\n");
                    const std::string tail =
                        buffered.size() > 96 ? buffered.substr(buffered.size() - 96) : buffered;
                    std::fprintf(stderr,
                                 "[loopback] header-too-large size=%zu has_content_length=%d "
                                 "terminator=%lld\n  head=%s\n  tail=%s\n",
                                 buffered.size(),
                                 probe.find("content-length:") != std::string::npos ? 1 : 0,
                                 static_cast<long long>(term), Snippet(buffered, 240).c_str(),
                                 Snippet(tail, 96).c_str());
                    std::fflush(stderr);
                }
                SendResponse(s, 400, "Bad Request", "text/plain", "header too large\n");
                return;
            }
            headerEnd = buffered.find("\r\n\r\n");
        }

        const std::size_t bodyStart = headerEnd + 4;
        const std::string head = buffered.substr(0, headerEnd);
        const std::size_t lineEnd = head.find("\r\n");
        const std::string requestLine = head.substr(0, lineEnd);
        const std::size_t sp1 = requestLine.find(' ');
        const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            badReqs_.fetch_add(1);
            NoteMalformed("bad-request-line", head);
            SendResponse(s, 400, "Bad Request", "text/plain", "bad request line\n");
            return;
        }
        std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::size_t query = path.find('?');
        if (query != std::string::npos) path = path.substr(0, query);

        const std::uint64_t contentLength = ParseContentLength(head);
        const std::uint64_t bufferedBody = static_cast<std::uint64_t>(buffered.size() - bodyStart);
        const std::uint64_t fromBuffer = std::min(bufferedBody, contentLength);
        std::uint64_t received = fromBuffer;
        while (received < contentLength) {
            const int want = static_cast<int>(
                std::min<std::uint64_t>(contentLength - received, sizeof(tmp)));
            const int n = recv(s, tmp, want, 0);
            if (n <= 0) {
                clientAborts_.fetch_add(1);
                return;
            }
            received += static_cast<std::uint64_t>(n);
        }
        buffered.erase(0, bodyStart + static_cast<std::size_t>(fromBuffer));

        if (path == "/api/windows/config.php") {
            if (!SendResponse(s, 200, "OK", "application/xml", ConfigXml(port_))) return;
            continue;
        }
        if (path == "/api/js/servers") {
            if (!SendResponse(s, 200, "OK", "application/json", ServerListJson(port_))) return;
            continue;
        }
        if (path == "/speedtest/latency.txt") {
            latencyReqs_.fetch_add(1);
            if (!SendResponse(s, 200, "OK", "text/plain", "test=test\ntest=test")) return;
            continue;
        }
        if (path.rfind("/speedtest/random", 0) == 0) {
            downloadReqs_.fetch_add(1);
            if (!SendDownloadHeaders(s, kDownloadBodyBytes)) return;
            char chunk[64 * 1024];
            for (std::size_t i = 0; i < sizeof(chunk); ++i) {
                chunk[i] = static_cast<char>((i * 31u + 7u) & 0xFF);
            }
            std::uint64_t sent = 0;
            bool failed = false;
            while (sent < kDownloadBodyBytes) {
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(kDownloadBodyBytes - sent, sizeof(chunk)));
                if (!SendAll(s, chunk, want)) {
                    failed = true;
                    break;
                }
                sent += want;
                downloadBytes_.fetch_add(want);
            }
            if (failed) return;
            continue;
        }
        if (path == "/speedtest/upload.php") {
            uploadReqs_.fetch_add(1);
            uploadBytes_.fetch_add(received);
            if (!SendResponse(s, 200, "OK", "text/plain", "size=" + std::to_string(received))) return;
            continue;
        }

        badReqs_.fetch_add(1);
        NoteMalformed("unknown-path", path);
        if (!SendResponse(s, 404, "Not Found", "text/plain", "not found\n")) return;
    }
}

}  // namespace st
