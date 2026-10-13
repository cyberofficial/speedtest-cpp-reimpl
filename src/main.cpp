// speedtest.exe - headless self-test front end for the measurement engine.
// The GUI (impl-ui) links the same engine; nothing here is UI specific.
#include "engine.h"
#include "http_client.h"
#include "json.h"
#include "loopback_server.h"
#include "share.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* kVersion = "0.2.0";

void Usage(std::FILE* out) {
    std::fprintf(out,
                 "speedtest %s (C++20 reimplementation, headless)\n"
                 "usage: speedtest.exe --selftest [--offline [--force-json-fallback]] [--duration SEC]\n"
                 "                     [--server ID] [--threads N] [--upload-threads N] [--config-url URL]\n"
                 "                     [--quiet]\n"
                 "       speedtest.exe --http-check\n"
                 "       speedtest.exe --share-test\n"
                 "\n"
                 "  --selftest          run one full measurement, print JSON on stdout\n"
                 "  --offline           run against the built-in loopback HTTP server\n"
                 "  --force-json-fallback  make the XML config lookup fail so the JSON fallback runs\n"
                 "  --duration SEC      download and upload stage length (default 10, offline 3)\n"
                 "  --server ID         server id / host / url / name / sponsor substring\n"
                 "  --threads N         parallel download connections (default 24, server-capped)\n"
                 "  --upload-threads N  parallel upload connections (default 2)\n"
                 "  --config-url URL    override the config endpoint (default api/windows/config.php)\n"
                 "  --http-check        User-Agent regression check against the config endpoint\n"
                 "  --share-test        upload a SYNTHETIC result to the share endpoint and report\n"
                 "  --quiet             do not print progress lines to stderr\n"
                 "\nThe share upload is the only call made to a host other than the measurement\n"
                 "server and www.speedtest.net, and it runs ONLY when the user asks to share.\n"
                 "\nexit codes: 0 ok, 2 usage, 3 measurement failed, 4 offline self-check failed,\n"
                 "            5 http-check failed, 6 share upload failed\n",
                 kVersion);
}

std::string NumOrNull(double v) {
    if (v < 0.0) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

std::string U64(std::uint64_t v) { return std::to_string(v); }

std::string ResultJson(const st::Result& r, const char* mode, const st::LoopbackServer* srv,
                       const std::vector<std::string>& failures) {
    std::string j = "{";
    j += std::string("\"ok\":") + (r.ok && failures.empty() ? "true" : "false");
    j += std::string(",\"mode\":\"") + mode + "\"";
    if (r.error.empty()) {
        j += ",\"error\":null";
    } else {
        j += ",\"error\":\"" + st::JsonEscape(r.error) + "\"";
    }
    j += ",\"config_source\":\"" + st::JsonEscape(r.configSource) + "\"";
    j += ",\"config_format\":\"" + st::JsonEscape(r.configFormat) + "\"";
    j += ",\"stages\":[";
    for (std::size_t i = 0; i < r.stages.size(); ++i) {
        if (i) j += ",";
        j += "\"" + st::JsonEscape(r.stages[i]) + "\"";
    }
    j += "]";
    j += ",\"server\":{";
    j += "\"id\":\"" + st::JsonEscape(r.server.id) + "\"";
    j += ",\"name\":\"" + st::JsonEscape(r.server.name) + "\"";
    j += ",\"sponsor\":\"" + st::JsonEscape(r.server.sponsor) + "\"";
    j += ",\"country\":\"" + st::JsonEscape(r.server.country) + "\"";
    j += ",\"host\":\"" + st::JsonEscape(r.server.host) + "\"";
    j += ",\"url\":\"" + st::JsonEscape(r.server.url) + "\"";
    j += "}";
    j += ",\"servers_found\":" + std::to_string(r.serversFound);
    j += ",\"protocol\":\"" + st::JsonEscape(r.protocol) + "\"";
    j += ",\"remote_ip\":\"" + st::JsonEscape(r.remoteIp) + "\"";
    j += ",\"pings\":[";
    for (std::size_t i = 0; i < r.pings.size(); ++i) {
        if (i) j += ",";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", r.pings[i]);
        j += buf;
    }
    j += "]";
    j += ",\"pings_success\":" + std::to_string(r.pingsSuccess);
    j += std::string(",\"tcp_error\":") + (r.tcpError ? "true" : "false");
    if (r.tcpErrorText.empty()) {
        j += ",\"tcp_error_text\":null";
    } else {
        j += ",\"tcp_error_text\":\"" + st::JsonEscape(r.tcpErrorText) + "\"";
    }
    j += ",\"latency_samples\":" + std::to_string(r.latencySamples);
    j += ",\"ping_ms\":" + NumOrNull(r.pingMs);
    j += ",\"jitter_ms\":" + NumOrNull(r.jitterMs);
    j += ",\"download_mbps\":" + NumOrNull(r.downloadMbps);
    j += ",\"upload_mbps\":" + NumOrNull(r.uploadMbps);
    j += ",\"download_bytes\":" + U64(r.downloadBytes);
    j += ",\"upload_bytes\":" + U64(r.uploadBytes);
    j += ",\"download_seconds\":" + NumOrNull(r.downloadSeconds);
    j += ",\"upload_seconds\":" + NumOrNull(r.uploadSeconds);
    j += ",\"download_connections_peak\":" + std::to_string(r.downloadConnectionsPeak);
    j += ",\"download_stop_reason\":\"" + st::JsonEscape(r.downloadStopReason) + "\"";
    j += ",\"upload_stop_reason\":\"" + st::JsonEscape(r.uploadStopReason) + "\"";
    if (srv) {
        j += ",\"loopback_server\":{";
        j += "\"latency_requests\":" + U64(srv->latencyRequests());
        j += ",\"download_requests\":" + U64(srv->downloadRequests());
        j += ",\"download_bytes_served\":" + U64(srv->downloadBytesServed());
        j += ",\"upload_requests\":" + U64(srv->uploadRequests());
        j += ",\"upload_bytes_received\":" + U64(srv->uploadBytesReceived());
        j += ",\"bad_requests\":" + U64(srv->badRequests());
        j += ",\"client_aborts\":" + U64(srv->clientAborts());
        j += "}";
    }
    j += ",\"selfcheck_failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i) j += ",";
        j += "\"" + st::JsonEscape(failures[i]) + "\"";
    }
    j += "]}";
    return j;
}

double NowSeconds() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

const char* kConfigUrl = "https://www.speedtest.net/api/windows/config.php";

// The config endpoint returns 403 for a bare "Mozilla/5.0" and 200 with no agent or a
// complete browser agent. This check keeps that gotcha from regressing silently.
int HttpCheck() {
    struct Case {
        const char* label;
        const char* agent;
    };
    const Case cases[] = {
        {"default-app-ua", ""},
        {"no-user-agent", "-"},
        {"bare-mozilla", "Mozilla/5.0"},
        {"full-browser-ua",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/124.0.0.0 Safari/537.36"},
    };
    std::string out = "{\"mode\":\"http-check\",\"url\":\"";
    out += kConfigUrl;
    out += "\",\"cases\":[";
    int defaultStatus = 0;
    int bareStatus = 0;
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        st::HttpRequest req;
        req.url = kConfigUrl;
        req.timeoutMs = 20000;
        req.maxBytes = 65536;
        req.userAgent = cases[i].agent;
        const st::HttpResult resp = st::HttpPerform(req);
        if (i == 0) defaultStatus = resp.status;
        if (i == 2) bareStatus = resp.status;
        if (i) out += ",";
        out += "{\"label\":\"" + std::string(cases[i].label) + "\",\"status\":" +
               std::to_string(resp.status) + ",\"bytes\":" + std::to_string(resp.bytes) +
               ",\"error\":" +
               (resp.error.empty() ? std::string("null") : "\"" + st::JsonEscape(resp.error) + "\"") +
               "}";
        std::fprintf(stderr, "[http-check] %-16s status=%d bytes=%llu\n", cases[i].label,
                     resp.status, static_cast<unsigned long long>(resp.bytes));
    }
    out += "]";
    out += std::string(",\"production_agent_ok\":") + (defaultStatus == 200 ? "true" : "false");
    out += std::string(",\"bare_mozilla_blocked\":") + (bareStatus == 403 ? "true" : "false");
    out += "}";
    std::printf("%s\n", out.c_str());
    std::fflush(stdout);
    return defaultStatus == 200 ? 0 : 5;
}

// --share-test: synthetic result, real endpoint. Exercises the whole share path
// (payload, gzip-stored, XOR, Base64, headers) without running a measurement.
int ShareTest() {
    st::Result r;
    r.ok = true;
    r.server.id = "1875";
    r.server.name = "West Norriton, PA";
    r.server.sponsor = "T-Mobile";
    r.server.host = "phi.speedtest.t-mobile.com:8080";
    r.server.url = "http://phi.speedtest.t-mobile.com:8080/speedtest/upload.php";
    r.protocol = "HTTP";
    r.serversFound = 100;
    r.pingMs = 23.007;
    r.jitterMs = 10.111;
    r.pings = {24.107, 22.604, 23.011, 22.418, 23.512, 22.905};
    r.pingsSuccess = 6;
    r.latencySamples = 6;
    r.downloadMbps = 135.49;
    r.downloadBytes = 169362500;
    r.downloadSeconds = 10.0;
    r.downloadConnectionsPeak = 24;
    r.uploadMbps = 18.35;
    r.uploadBytes = 22937500;
    r.uploadSeconds = 10.0;

    const st::ShareOutcome out = st::ShareUpload(r);

    std::string j = "{\"mode\":\"share-test\",\"ok\":";
    j += out.ok ? "true" : "false";
    j += ",\"url\":";
    j += out.ok ? "\"" + st::JsonEscape(out.url) + "\"" : std::string("null");
    j += ",\"response\":\"" + st::JsonEscape(out.detail) + "\"}";
    std::printf("%s\n", j.c_str());
    std::fflush(stdout);
    if (!out.ok) std::fprintf(stderr, "[share-test] FAILED: %s\n", out.detail.c_str());
    return out.ok ? 0 : 6;
}

}  // namespace

int main(int argc, char** argv) {
    bool selftest = false;
    bool offline = false;
    bool forceJsonFallback = false;
    bool quiet = false;
    bool httpCheck = false;
    bool shareTest = false;
    double duration = -1.0;
    std::string serverOverride;
    std::string configUrl;
    int threads = 0;
    int uploadThreads = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") {
            selftest = true;
        } else if (a == "--offline") {
            offline = true;
        } else if (a == "--force-json-fallback") {
            forceJsonFallback = true;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--http-check") {
            httpCheck = true;
        } else if (a == "--share-test") {
            shareTest = true;
        } else if ((a == "--duration" || a == "-d") && i + 1 < argc) {
            duration = std::atof(argv[++i]);
        } else if (a == "--server" && i + 1 < argc) {
            serverOverride = argv[++i];
        } else if (a == "--config-url" && i + 1 < argc) {
            configUrl = argv[++i];
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (a == "--upload-threads" && i + 1 < argc) {
            uploadThreads = std::atoi(argv[++i]);
        } else if (a == "--help" || a == "-h") {
            Usage(stdout);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            Usage(stderr);
            return 2;
        }
    }
    if (httpCheck) return HttpCheck();
    if (shareTest) return ShareTest();
    if (!selftest) {
        Usage(stdout);
        return 0;
    }

    double lastEmit = -1.0;
    st::Phase lastPhase = st::Phase::Idle;
    st::Engine::ProgressFn onProgress = [&](const st::Progress& p) {
        if (quiet) return;
        const double now = NowSeconds();
        if (p.phase == lastPhase && now - lastEmit < 0.5 && p.percent < 100.0) return;
        lastEmit = now;
        lastPhase = p.phase;
        std::fprintf(stderr, "[%-13s] %6.1f%%  %10.2f Mbps  ping %7.2f ms  jitter %6.2f ms  %s%s%s\n",
                     st::PhaseName(p.phase), p.percent, p.currentMbps, p.pingMs, p.jitterMs,
                     p.serverName.c_str(), p.serverLocation.empty() ? "" : " / ",
                     p.serverLocation.c_str());
        std::fflush(stderr);
    };

    st::Config cfg;
    if (duration > 0.0) {
        cfg.downloadSeconds = duration;
        cfg.uploadSeconds = duration;
    }
    // Requested download connection ceiling. 24 matches the era's config
    // (downloadMaxConnections="24"); the engine lowers it to whatever the server
    // actually advertises. --threads N overrides the request either way.
    cfg.threadCount = threads > 0 ? threads : 24;
    if (uploadThreads > 0) cfg.uploadThreadCount = uploadThreads;
    cfg.serverOverride = serverOverride;
    if (!configUrl.empty()) cfg.configUrl = configUrl;

    st::LoopbackServer server;
    const st::LoopbackServer* srvView = nullptr;
    if (offline) {
        std::string err;
        if (!server.Start(&err)) {
            const st::Result empty;
            std::vector<std::string> failures{"loopback server start failed: " + err};
            std::printf("%s\n", ResultJson(empty, "offline", nullptr, failures).c_str());
            return 4;
        }
        srvView = &server;
        cfg.configUrl = server.baseUrl() + (forceJsonFallback ? "/missing-config.php"
                                                             : "/api/windows/config.php");
        cfg.fallbackServerListUrl = server.baseUrl() + "/api/js/servers";
        if (duration <= 0.0) {
            cfg.downloadSeconds = 3.0;
            cfg.uploadSeconds = 3.0;
        }
        cfg.latencySamples = 4;
        cfg.serverProbeCount = 1;
        cfg.minTestDurationSeconds = std::min(cfg.minTestDurationSeconds, cfg.downloadSeconds);
        cfg.requestTimeoutMs = 10000;
        cfg.probeTimeoutMs = 5000;
        if (threads <= 0) cfg.threadCount = 2;
        if (uploadThreads <= 0) cfg.uploadThreadCount = 2;
    } else {
        // Keep the download stage from being stretched by the min-test-duration floor.
        cfg.minTestDurationSeconds = std::min(cfg.minTestDurationSeconds, cfg.downloadSeconds);
    }

    st::Engine engine(cfg, onProgress);
    const st::Result res = engine.Run();
    if (offline) server.Stop();

    std::vector<std::string> failures;
    if (offline) {
        if (!res.ok) failures.push_back("engine: " + res.error);
        if (res.pingMs < 0.0) failures.push_back("ping missing");
        if (res.downloadMbps <= 0.0) failures.push_back("download mbps <= 0");
        if (res.uploadMbps <= 0.0) failures.push_back("upload mbps <= 0");
        if (res.serversFound < 1) failures.push_back("server list empty");
        const char* wantFormat = forceJsonFallback ? "json-fallback" : "xml";
        if (res.configFormat != wantFormat) {
            failures.push_back(std::string("config format is '") + res.configFormat + "', expected '" +
                               wantFormat + "'");
        }
        if (server.latencyRequests() < 1) failures.push_back("loopback latency endpoint never hit");
        if (server.downloadBytesServed() == 0) failures.push_back("loopback served no download bytes");
        if (server.uploadRequests() < 1) failures.push_back("loopback upload endpoint never hit");
        if (server.uploadBytesReceived() == 0) failures.push_back("loopback received no upload bytes");
        if (server.badRequests() != 0) {
            failures.push_back("loopback saw " + std::to_string(server.badRequests()) +
                               " malformed request(s)");
        }
    }

    std::printf("%s\n", ResultJson(res, offline ? "offline" : "network", srvView, failures).c_str());
    std::fflush(stdout);
    if (offline) return failures.empty() ? 0 : 4;
    return res.ok ? 0 : 3;
}
