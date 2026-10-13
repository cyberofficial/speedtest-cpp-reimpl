#include <winsock2.h>
#include <ws2tcpip.h>

#include "engine.h"

#include "http_client.h"
#include "json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace st {
namespace {

double NowSeconds() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

// Single source of truth for Mbps: the final Result and the running Progress average both
// use this, so the number on screen converges to the number that gets recorded.
double Mbps(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds / 1e6 : 0.0;
}

// The server list gives one url per server, for example
//   http://phi.speedtest.t-mobile.com:8080/speedtest/upload.php
// Every measurement target keeps that scheme, host and directory and swaps only the
// final path segment (latency.txt / random<W>x<H>.jpg / upload.php).
std::string DeriveEndpoint(const std::string& serverUrl, const std::string& finalSegment) {
    const std::size_t cut = serverUrl.find_first_of("?#");
    const std::string base = cut == std::string::npos ? serverUrl : serverUrl.substr(0, cut);
    const std::size_t schemeEnd = base.find("://");
    const std::size_t slash = base.rfind('/');
    if (slash == std::string::npos) return std::string();
    if (schemeEnd != std::string::npos && slash <= schemeEnd + 2) return std::string();
    return base.substr(0, slash + 1) + finalSegment;
}

std::string DownloadSegment(int pixels) {
    return "random" + std::to_string(pixels) + "x" + std::to_string(pixels) + ".jpg";
}

std::string HttpFailureText(const HttpResult& r) {
    if (!r.error.empty()) return r.error;
    return "http status " + std::to_string(r.status);
}

std::string ResolveRemoteIp(const std::string& host) {
    static std::once_flag wsaOnce;
    std::call_once(wsaOnce, []() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &list) != 0 || list == nullptr) return std::string();
    std::string ip;
    for (addrinfo* p = list; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET) {
            const auto* in4 = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
            if (inet_ntop(AF_INET, &in4->sin_addr, buf, sizeof(buf))) {
                ip = buf;
                break;
            }
        } else if (p->ai_family == AF_INET6 && ip.empty()) {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
            if (inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf))) ip = buf;
        }
    }
    freeaddrinfo(list);
    return ip;
}

// --- tiny attribute scanner for the XML config response (no third-party code) --------

struct XmlAttr {
    std::string name;
    std::string value;
};

// Returns every element whose tag name matches `tag`, with its attributes in document order.
std::vector<std::vector<XmlAttr>> ScanElements(const std::string& xml, const std::string& tag) {
    std::vector<std::vector<XmlAttr>> out;
    const std::string open = "<" + tag;
    std::size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        const char next = pos + open.size() < xml.size() ? xml[pos + open.size()] : '\0';
        if (next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != '/' && next != '>') {
            pos += open.size();
            continue;  // e.g. <servers> when looking for <server>
        }
        const std::size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        const std::string body = xml.substr(pos + open.size(), end - pos - open.size());
        std::vector<XmlAttr> attrs;
        std::size_t i = 0;
        while (i < body.size()) {
            while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
            const std::size_t nameStart = i;
            while (i < body.size() && body[i] != '=' && body[i] != '/' &&
                   !std::isspace(static_cast<unsigned char>(body[i]))) {
                ++i;
            }
            if (i == nameStart) {
                ++i;
                continue;
            }
            XmlAttr a;
            a.name = body.substr(nameStart, i - nameStart);
            while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
            if (i < body.size() && body[i] == '=') {
                ++i;
                while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
                if (i < body.size() && (body[i] == '"' || body[i] == '\'')) {
                    const char quote = body[i++];
                    const std::size_t vStart = i;
                    while (i < body.size() && body[i] != quote) ++i;
                    a.value = body.substr(vStart, i - vStart);
                    if (i < body.size()) ++i;
                }
            }
            if (!a.name.empty()) attrs.push_back(std::move(a));
        }
        out.push_back(std::move(attrs));
        pos = end + 1;
    }
    return out;
}

const std::string* Attr(const std::vector<XmlAttr>& attrs, const std::string& name) {
    for (const XmlAttr& a : attrs) {
        if (a.name == name) return &a.value;
    }
    return nullptr;
}

std::string AttrOr(const std::vector<XmlAttr>& attrs, const std::string& name,
                   const std::string& fallback = std::string()) {
    const std::string* v = Attr(attrs, name);
    return v ? *v : fallback;
}

double AttrNum(const std::vector<XmlAttr>& attrs, const std::string& name, double fallback = 0.0) {
    const std::string* v = Attr(attrs, name);
    if (!v || v->empty()) return fallback;
    return std::strtod(v->c_str(), nullptr);
}

bool AttrBool(const std::vector<XmlAttr>& attrs, const std::string& name, bool fallback) {
    const std::string* v = Attr(attrs, name);
    if (!v) return fallback;
    return *v == "true" || *v == "1" || *v == "True";
}

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle";
        case Phase::StageMetadata: return "StageMetadata";
        case Phase::PrimerTCP: return "PrimerTCP";
        case Phase::Latency: return "Latency";
        case Phase::DownloadTCP: return "DownloadTCP";
        case Phase::UploadTCP: return "UploadTCP";
        case Phase::Done: return "Done";
        case Phase::Failed: return "Failed";
    }
    return "unknown";
}

std::string Server::displayName() const {
    if (!sponsor.empty()) return sponsor;
    if (!name.empty()) return name;
    if (!host.empty()) return host;
    return url;
}

Engine::Engine(Config cfg, ProgressFn onProgress) : cfg_(std::move(cfg)), cb_(std::move(onProgress)) {}

void Engine::Emit() {
    if (cb_) cb_(prog_);
}

bool Engine::FetchConfigXml(Discovery& out, std::string& err) {
    HttpRequest req;
    req.url = cfg_.configUrl;
    req.timeoutMs = cfg_.requestTimeoutMs;
    req.maxBytes = 8u * 1024u * 1024u;
    const HttpResult resp = HttpPerform(req);
    if (!resp.ok) {
        err = "config request failed: " + HttpFailureText(resp) + " (" + req.url + ")";
        return false;
    }

    const auto serverElements = ScanElements(resp.body, "server");
    if (serverElements.empty()) {
        err = "config XML contained no <server> elements";
        return false;
    }
    // Server-provided tuning, applied only where the caller left the default.
    const auto dynamicElements = ScanElements(resp.body, "dynamic");
    if (!dynamicElements.empty()) {
        const auto& dyn = dynamicElements.front();
        const double maxConn = AttrNum(dyn, "downloadMaxConnections", 0.0);
        if (maxConn > 0.0) cfg_.serverDownloadMaxConnections = static_cast<int>(maxConn);
        const double window = AttrNum(dyn, "downloadConnScalingWindowSize", 0.0);
        if (window > 0.0) cfg_.serverDownloadConnScalingWindowSize = static_cast<int>(window);
        cfg_.isScalingEnabled = AttrBool(dyn, "enableDownloadConnScaling", cfg_.isScalingEnabled);
    }
    const auto configElements = ScanElements(resp.body, "config");
    if (!configElements.empty()) {
        const auto& conf = configElements.front();
        cfg_.enableServerUploadMeasurement =
            AttrBool(conf, "enableServerUploadMeasurement", cfg_.enableServerUploadMeasurement);
        cfg_.pping = AttrBool(conf, "pping", cfg_.pping);
    }

    for (const auto& attrs : serverElements) {
        Server s;
        s.url = AttrOr(attrs, "url");
        if (s.url.empty()) continue;
        s.lat = AttrNum(attrs, "lat");
        s.lon = AttrNum(attrs, "lon");
        s.name = AttrOr(attrs, "name");
        s.sponsor = AttrOr(attrs, "sponsor");
        s.id = AttrOr(attrs, "id");
        s.host = AttrOr(attrs, "host");
        s.adjustment = AttrNum(attrs, "sponsor_adj");
        s.sponsored = s.adjustment != 0.0;
        if (s.host.empty()) {
            const Url u = ParseUrl(s.url);
            if (u.valid) s.host = u.host + ":" + std::to_string(u.port);
        }
        out.servers.push_back(std::move(s));
    }
    if (out.servers.empty()) {
        err = "config XML <server> elements had no usable url";
        return false;
    }
    out.sourceUrl = cfg_.configUrl;
    out.format = "xml";
    return true;
}

bool Engine::FetchServerListJson(const std::string& url, Discovery& out, std::string& err) {
    HttpRequest req;
    req.url = url;
    req.timeoutMs = cfg_.requestTimeoutMs;
    req.maxBytes = 4u * 1024u * 1024u;
    const HttpResult resp = HttpPerform(req);
    if (!resp.ok) {
        err = "server list request failed: " + HttpFailureText(resp) + " (" + url + ")";
        return false;
    }
    JsonValue root;
    std::string jerr;
    if (!JsonParse(resp.body, root, &jerr)) {
        err = "server list JSON parse failed: " + jerr;
        return false;
    }
    const JsonValue* arr = nullptr;
    if (root.isArray()) {
        arr = &root;
    } else if (root.isObject()) {
        const JsonValue* s = root.find("servers");
        if (s && s->isArray()) arr = s;
    }
    if (!arr) {
        err = "server list JSON contains no server array";
        return false;
    }
    for (const JsonValue& item : arr->items) {
        if (!item.isObject()) continue;
        Server s;
        const JsonValue* v = item.find("url");
        s.url = v ? v->strOr() : std::string();
        if (s.url.empty()) continue;
        if ((v = item.find("id")) != nullptr) s.id = v->strOr();
        if ((v = item.find("name")) != nullptr) s.name = v->strOr();
        if ((v = item.find("sponsor")) != nullptr) s.sponsor = v->strOr();
        if ((v = item.find("country")) != nullptr) s.country = v->strOr();
        if ((v = item.find("host")) != nullptr) s.host = v->strOr();
        if ((v = item.find("lat")) != nullptr) s.lat = v->numOr();
        if ((v = item.find("lon")) != nullptr) s.lon = v->numOr();
        if (s.host.empty()) {
            const Url u = ParseUrl(s.url);
            if (u.valid) s.host = u.host + ":" + std::to_string(u.port);
        }
        out.servers.push_back(std::move(s));
    }
    if (out.servers.empty()) {
        err = "server list contained no usable entries";
        return false;
    }
    out.sourceUrl = url;
    out.format = "json-fallback";
    return true;
}

bool Engine::DiscoverServers(Discovery& out, std::string& err) {
    std::string xmlError;
    if (FetchConfigXml(out, xmlError)) return true;
    std::string jsonError;
    Discovery fallback;
    if (FetchServerListJson(cfg_.fallbackServerListUrl, fallback, jsonError)) {
        std::fprintf(stderr, "[engine] config XML failed (%s); using JSON fallback %s\n",
                     xmlError.c_str(), cfg_.fallbackServerListUrl.c_str());
        out = std::move(fallback);
        return true;
    }
    err = "discovery failed. xml: " + xmlError + " | json fallback: " + jsonError;
    return false;
}

bool Engine::Primer(const std::string& latencyUrl, std::string& err) {
    HttpRequest req;
    req.url = latencyUrl;
    req.timeoutMs = cfg_.requestTimeoutMs;
    req.maxBytes = 8192;
    req.discardBody = true;
    const HttpResult resp = HttpPerform(req);
    if (!resp.ok) {
        err = "primer request failed: " + HttpFailureText(resp);
        return false;
    }
    return true;
}

bool Engine::MeasureLatency(const std::string& latencyUrl, Result& res, std::string& err) {
    std::string lastError;
    for (int i = 0; i < cfg_.latencySamples; ++i) {
        HttpRequest req;
        req.url = latencyUrl;
        req.timeoutMs = cfg_.requestTimeoutMs;
        req.maxBytes = 8192;
        req.discardBody = true;
        const double t0 = NowSeconds();
        const HttpResult resp = HttpPerform(req);
        const double ms = (NowSeconds() - t0) * 1000.0;
        if (resp.ok) {
            res.pings.push_back(ms);
        } else {
            lastError = HttpFailureText(resp);
        }
        prog_.percent = 100.0 * static_cast<double>(i + 1) / std::max(1, cfg_.latencySamples);
        if (!res.pings.empty()) {
            double sum = 0.0;
            for (double v : res.pings) sum += v;
            prog_.pingMs = sum / static_cast<double>(res.pings.size());
        }
        Emit();
    }
    if (res.pings.empty()) {
        err = "all latency probes failed: " + lastError;
        res.tcpError = true;
        res.tcpErrorText = lastError;
        return false;
    }
    double sum = 0.0;
    for (double v : res.pings) sum += v;
    res.pingMs = sum / static_cast<double>(res.pings.size());
    double jitter = 0.0;
    for (std::size_t i = 1; i < res.pings.size(); ++i) {
        jitter += std::fabs(res.pings[i] - res.pings[i - 1]);
    }
    res.jitterMs = res.pings.size() > 1 ? jitter / static_cast<double>(res.pings.size() - 1) : 0.0;
    res.pingsSuccess = static_cast<int>(res.pings.size());
    res.latencySamples = res.pingsSuccess;
    if (res.pingsSuccess < cfg_.latencySamples) {
        res.tcpError = true;
        res.tcpErrorText = lastError;
    }
    return true;
}

bool Engine::MeasureDownload(const std::string& serverUrl, Result& res, std::string& err) {
    std::vector<std::string> urls;
    for (const int px : cfg_.downloadSizes) {
        const std::string u = DeriveEndpoint(serverUrl, DownloadSegment(px));
        if (!u.empty()) urls.push_back(u);
    }
    if (urls.empty()) {
        err = "could not derive download url from " + serverUrl;
        return false;
    }

    // Connection scaling: the live config advertises downloadMaxConnections="24" with
    // enableDownloadConnScaling="true", so raise our ceiling toward that instead of the
    // old fixed 4. An absolute cap keeps a run terminating; --threads overrides it.
    // cfg_.threadCount is the requested ceiling (main.cpp defaults it to the config
    // era's 24). The server's advertised downloadMaxConnections only ever LOWERS it, so
    // an explicit --threads N is honoured downward as well as upward.
    constexpr int kHardConnectionCap = 32;
    int ceiling = cfg_.threadCount > 0 ? cfg_.threadCount : 4;
    if (cfg_.serverDownloadMaxConnections > 0) {
        ceiling = std::min(ceiling, cfg_.serverDownloadMaxConnections);
    }
    ceiling = std::min(std::max(ceiling, 1), kHardConnectionCap);
    const int connections = ceiling;

    std::atomic<std::uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<int> activeConnections{0};
    std::atomic<int> peakConnections{0};
    std::mutex errMutex;
    std::string firstError;

    const double seconds = std::max(cfg_.minTestDurationSeconds, cfg_.downloadSeconds);
    const double phaseStart = NowSeconds();
    const std::size_t sizeCount = urls.size();
    const std::uint64_t perConnectionCap = cfg_.downloadMaxBytesPerConnection;

    auto bumpActive = [&]() {
        const int now = activeConnections.fetch_add(1, std::memory_order_relaxed) + 1;
        int peak = peakConnections.load(std::memory_order_relaxed);
        while (now > peak &&
               !peakConnections.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
        }
    };

    // Ramp-up: start small and add the rest across the first half of the stage. The
    // original scales connections during the stage and bounds how many are added per
    // attempt (downloadConnScalingMaxPerAttempt); its exact maths is undocumented, so
    // this is a documented approximation.
    const int startConnections = cfg_.isScalingEnabled ? std::min(2, connections) : connections;
    const double startSpacing = cfg_.isScalingEnabled && connections > startConnections
                                    ? (0.5 * seconds) / (connections - startConnections)
                                    : 0.0;

    auto worker = [&](int index) {
        if (startSpacing > 0.0 && index >= startConnections) {
            const double startAt = phaseStart + (index - startConnections + 1) * startSpacing;
            while (!stop.load(std::memory_order_relaxed) && NowSeconds() < startAt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (stop.load(std::memory_order_relaxed)) return;
        bumpActive();
        // One payload request. Real servers do not host every random<W>x<H>.jpg size,
        // so the worker ramps upward but moves the floor up / ceiling down when a size
        // is not available instead of failing the whole stage.
        auto fetchOnce = [&](const std::string& url, bool& cappedOut) {
            HttpRequest req;
            req.url = url;
            req.timeoutMs = cfg_.requestTimeoutMs;
            req.discardBody = true;
            req.bufferBytes = cfg_.tcpDownloadBufferBytes;
            std::uint64_t last = 0;
            req.onChunk = [&](std::uint64_t soFar, double) {
                if (soFar > last) {
                    total.fetch_add(soFar - last, std::memory_order_relaxed);
                    last = soFar;
                }
                if (stop.load(std::memory_order_relaxed)) return false;
                if (perConnectionCap != 0 && soFar >= perConnectionCap) return false;
                return true;
            };
            const HttpResult resp = HttpPerform(req);
            cappedOut = perConnectionCap != 0 && resp.bytes >= perConnectionCap;
            return resp;
        };

        std::size_t floorIdx = 0;
        std::size_t ceilingIdx = sizeCount - 1;
        bool haveGood = false;
        while (!stop.load(std::memory_order_relaxed)) {
            const double frac =
                std::min(1.0, (NowSeconds() - phaseStart) / std::max(0.001, seconds));
            std::size_t idx = std::min(sizeCount - 1, static_cast<std::size_t>(frac * sizeCount));
            idx = std::max(idx, floorIdx);
            idx = std::min(idx, ceilingIdx);
            idx = std::min(idx, sizeCount - 1);
            bool capped = false;
            const HttpResult resp = fetchOnce(urls[idx], capped);
            if (resp.ok || resp.aborted || capped) {
                haveGood = true;
                continue;
            }
            const std::string failure = HttpFailureText(resp);
            if (!haveGood && idx + 1 < sizeCount) {
                floorIdx = idx + 1;  // smallest payloads are not hosted; try bigger
                continue;
            }
            if (haveGood && idx > floorIdx) {
                ceilingIdx = idx - 1;  // this larger payload is not hosted; back off
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(errMutex);
                if (firstError.empty()) firstError = failure;
            }
            const int n = errors.fetch_add(1) + 1;
            if (n >= connections * 3) stop.store(true, std::memory_order_relaxed);
        }
        activeConnections.fetch_sub(1);
    };

    prog_.phase = Phase::DownloadTCP;
    prog_.percent = 0.0;
    prog_.currentMbps = 0.0;
    prog_.avgMbps = 0.0;
    Emit();

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(connections));
    for (int i = 0; i < connections; ++i) threads.emplace_back(worker, i);

    const int stableNeeded = 3;
    std::vector<double> recentRates;
    std::uint64_t lastTotal = 0;
    double lastT = phaseStart;
    double instant = 0.0;
    std::string stopReason = "duration";
    while (NowSeconds() - phaseStart < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::uint64_t cur = total.load(std::memory_order_relaxed);
        const double now = NowSeconds();
        const double dt = now - lastT;
        if (dt > 0.0) {
            instant = static_cast<double>(cur - lastTotal) * 8.0 / dt / 1e6;
            recentRates.push_back(instant);
            if (recentRates.size() > 8) recentRates.erase(recentRates.begin());
        }
        lastTotal = cur;
        lastT = now;
        prog_.percent = std::min(100.0, 100.0 * (now - phaseStart) / seconds);
        prog_.currentMbps = instant;
        prog_.avgMbps = Mbps(cur, now - phaseStart);
        Emit();
        if (stop.load(std::memory_order_relaxed)) {
            stopReason = "error";
            break;
        }
        if (cfg_.stableStop && now - phaseStart >= cfg_.minTestDurationSeconds &&
            recentRates.size() >= static_cast<std::size_t>(stableNeeded)) {
            double mean = 0.0;
            for (double v : recentRates) mean += v;
            mean /= static_cast<double>(recentRates.size());
            bool stable = mean > 0.0;
            for (double v : recentRates) {
                if (mean > 0.0 && std::fabs(v - mean) / mean > cfg_.stableStopTolerance) stable = false;
            }
            if (stable) {
                stopReason = "stable";
                break;
            }
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : threads) th.join();

    const double elapsed = NowSeconds() - phaseStart;
    res.downloadBytes = total.load(std::memory_order_relaxed);
    res.downloadSeconds = elapsed;
    res.downloadMbps = Mbps(res.downloadBytes, elapsed);
    res.downloadConnectionsPeak = peakConnections.load(std::memory_order_relaxed);
    res.downloadStopReason = stopReason;
    prog_.percent = 100.0;
    prog_.currentMbps = res.downloadMbps;
    prog_.avgMbps = res.downloadMbps;  // final value equals the recorded Result, same formula
    Emit();
    std::fprintf(stderr, "[engine] DownloadTCP final: avgMbps=%.3f result=%.3f\n", prog_.avgMbps,
                 res.downloadMbps);
    if (res.downloadBytes == 0) {
        err = firstError.empty() ? "download transferred no bytes" : ("download failed: " + firstError);
        res.tcpError = true;
        res.tcpErrorText = firstError;
        return false;
    }
    return true;
}

bool Engine::MeasureUpload(const std::string& uploadUrl, Result& res, std::string& err) {
    const int connections = std::max(1, cfg_.uploadThreadCount);
    const std::uint64_t chunkBytes = std::max<std::uint64_t>(1024, cfg_.tcpUploadBufferBytes);
    const std::uint64_t perConnectionCap = cfg_.uploadMaxBytesPerConnection;

    std::atomic<std::uint64_t> total{0};
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<int> cappedStreams{0};
    std::mutex errMutex;
    std::string firstError;

    std::string payload(static_cast<std::size_t>(chunkBytes), '\0');
    std::uint32_t lcg = 0x12345678u;
    for (char& c : payload) {
        lcg = lcg * 1664525u + 1013904223u;
        c = static_cast<char>((lcg >> 16) & 0xFF);
    }

    auto worker = [&]() {
        std::uint64_t mine = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (perConnectionCap != 0 && mine >= perConnectionCap) {
                cappedStreams.fetch_add(1);
                return;
            }
            HttpRequest req;
            req.url = uploadUrl;
            req.method = "POST";
            req.postBody = payload;
            req.timeoutMs = cfg_.requestTimeoutMs;
            req.maxBytes = 4096;
            const HttpResult resp = HttpPerform(req);
            if (resp.ok) {
                total.fetch_add(chunkBytes, std::memory_order_relaxed);
                mine += chunkBytes;
            } else {
                const int n = errors.fetch_add(1) + 1;
                {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (firstError.empty()) firstError = HttpFailureText(resp);
                }
                if (n >= connections * 3) stop.store(true, std::memory_order_relaxed);
            }
        }
    };

    prog_.phase = Phase::UploadTCP;
    prog_.percent = 0.0;
    prog_.currentMbps = 0.0;
    prog_.avgMbps = 0.0;
    Emit();

    const double seconds = std::max(cfg_.minTestDurationSeconds, cfg_.uploadSeconds);
    const double phaseStart = NowSeconds();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(connections));
    for (int i = 0; i < connections; ++i) threads.emplace_back(worker);

    std::uint64_t lastTotal = 0;
    double lastT = phaseStart;
    double instant = 0.0;
    std::string stopReason = "duration";
    while (NowSeconds() - phaseStart < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::uint64_t cur = total.load(std::memory_order_relaxed);
        const double now = NowSeconds();
        const double dt = now - lastT;
        if (dt > 0.0) instant = static_cast<double>(cur - lastTotal) * 8.0 / dt / 1e6;
        lastTotal = cur;
        lastT = now;
        prog_.percent = std::min(100.0, 100.0 * (now - phaseStart) / seconds);
        prog_.currentMbps = instant;
        prog_.avgMbps = Mbps(cur, now - phaseStart);
        Emit();
        if (stop.load(std::memory_order_relaxed)) {
            stopReason = "error";
            break;
        }
        if (perConnectionCap != 0 && cappedStreams.load() >= connections) {
            stopReason = "byte-cap";
            break;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : threads) th.join();

    const double elapsed = NowSeconds() - phaseStart;
    res.uploadBytes = total.load(std::memory_order_relaxed);
    res.uploadSeconds = elapsed;
    res.uploadMbps = Mbps(res.uploadBytes, elapsed);
    res.uploadStopReason = stopReason;
    prog_.percent = 100.0;
    prog_.currentMbps = res.uploadMbps;
    prog_.avgMbps = res.uploadMbps;  // final value equals the recorded Result, same formula
    Emit();
    std::fprintf(stderr, "[engine] UploadTCP final: avgMbps=%.3f result=%.3f\n", prog_.avgMbps,
                 res.uploadMbps);
    if (res.uploadBytes == 0) {
        err = firstError.empty() ? "upload transferred no bytes" : ("upload failed: " + firstError);
        res.tcpError = true;
        res.tcpErrorText = firstError;
        return false;
    }
    return true;
}

Result Engine::Run() {
    Result res;
    std::string err;

    prog_.phase = Phase::StageMetadata;
    prog_.percent = 0.0;
    prog_.avgMbps = 0.0;
    Emit();
    res.stages.push_back(PhaseName(Phase::StageMetadata));

    Discovery discovery;
    if (!DiscoverServers(discovery, err)) {
        res.error = err;
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }
    res.serversFound = static_cast<int>(discovery.servers.size());
    res.configSource = discovery.sourceUrl;
    res.configFormat = discovery.format;

    Server chosen;
    bool haveServer = false;
    if (!cfg_.serverOverride.empty()) {
        for (const Server& s : discovery.servers) {
            if (s.id == cfg_.serverOverride || s.host == cfg_.serverOverride ||
                (!s.name.empty() && s.name.find(cfg_.serverOverride) != std::string::npos) ||
                (!s.sponsor.empty() && s.sponsor.find(cfg_.serverOverride) != std::string::npos) ||
                s.url.find(cfg_.serverOverride) != std::string::npos) {
                chosen = s;
                haveServer = true;
                break;
            }
        }
        if (!haveServer) {
            res.error = "server override '" + cfg_.serverOverride + "' did not match any server";
            prog_.phase = Phase::Failed;
            Emit();
            return res;
        }
    } else {
        const int probes = std::min<int>(std::max(1, cfg_.serverProbeCount),
                                         static_cast<int>(discovery.servers.size()));
        double bestMs = 1e18;
        for (int i = 0; i < probes; ++i) {
            const Server& s = discovery.servers[static_cast<std::size_t>(i)];
            const std::string latencyUrl = DeriveEndpoint(s.url, cfg_.latencyFile);
            if (latencyUrl.empty()) continue;
            HttpRequest req;
            req.url = latencyUrl;
            req.timeoutMs = cfg_.probeTimeoutMs;
            req.maxBytes = 4096;
            req.discardBody = true;
            const HttpResult resp = HttpPerform(req);
            const double ms = resp.seconds * 1000.0;
            if ((resp.ok || resp.aborted) && ms < bestMs) {
                bestMs = ms;
                chosen = s;
                haveServer = true;
            }
            prog_.serverName = s.displayName();
            prog_.serverLocation = s.name;
            prog_.percent = 100.0 * (i + 1) / probes;
            Emit();
        }
        if (haveServer) {
            std::fprintf(stderr, "[engine] probed %d server(s), picked %s (%s) %.1f ms\n", probes,
                         chosen.displayName().c_str(), chosen.name.c_str(), bestMs);
        }
    }
    if (!haveServer) {
        res.error = "no reachable speed test server found";
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }

    const std::string latencyUrl = DeriveEndpoint(chosen.url, cfg_.latencyFile);
    const std::string uploadUrl = DeriveEndpoint(chosen.url, cfg_.uploadFile);
    if (latencyUrl.empty() || uploadUrl.empty()) {
        res.error = "server url is not usable: " + chosen.url;
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }

    res.server = chosen;
    res.protocol = chosen.url.rfind("https://", 0) == 0 ? "HTTPS" : "HTTP";
    res.remoteIp = ResolveRemoteIp(chosen.host.empty() ? ParseUrl(chosen.url).host : chosen.host);
    prog_.serverName = chosen.displayName();
    prog_.serverLocation = chosen.name;

    prog_.phase = Phase::PrimerTCP;
    prog_.percent = 0.0;
    prog_.avgMbps = 0.0;
    Emit();
    res.stages.push_back(PhaseName(Phase::PrimerTCP));
    if (!Primer(latencyUrl, err)) {
        res.error = err;
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }
    prog_.percent = 100.0;
    Emit();

    prog_.phase = Phase::Latency;
    prog_.percent = 0.0;
    prog_.avgMbps = 0.0;
    Emit();
    res.stages.push_back(PhaseName(Phase::Latency));
    if (!MeasureLatency(latencyUrl, res, err)) {
        res.error = err;
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }
    prog_.pingMs = res.pingMs;
    prog_.jitterMs = res.jitterMs;
    prog_.percent = 100.0;
    Emit();

    res.stages.push_back(PhaseName(Phase::DownloadTCP));
    if (!MeasureDownload(chosen.url, res, err)) {
        res.error = err;
        prog_.phase = Phase::Failed;
        Emit();
        return res;
    }

    if (cfg_.enableServerUploadMeasurement) {
        res.stages.push_back(PhaseName(Phase::UploadTCP));
        if (!MeasureUpload(uploadUrl, res, err)) {
            res.error = err;
            prog_.phase = Phase::Failed;
            Emit();
            return res;
        }
    }

    res.ok = true;
    prog_.phase = Phase::Done;
    prog_.percent = 100.0;
    Emit();
    return res;
}

}  // namespace st
