// Measurement engine: config/server discovery, latency/jitter, download and upload
// throughput. UI-agnostic; progress is delivered through a callback.
//
// Protocol contract (per analysis\decompilation.md, verified by the Lead):
//   config + server list : https://www.speedtest.net/api/windows/config.php (XML, the
//                          original's source; overridable, like the original's config url).
//                          https://www.speedtest.net/api/js/servers (JSON) is OUR fallback
//                          choice, not the original's behaviour.
//   measurement target   : each <server url="..."> value is DATA. Upload uses it verbatim;
//                          latency/download swap only the final path segment. The scheme
//                          comes from that url (real servers return http://), so TLS is
//                          never forced on measurement connections. The only other host we
//                          contact is www.speedtest.net, for that config lookup.
//   stage vocabulary     : StageMetadata, PrimerTCP, Latency, DownloadTCP, UploadTCP.
//                          PacketLoss, Traceroute and CpuMetrics are not implemented here.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace st {

// Original stage vocabulary; only the stages this engine implements.
enum class Phase { Idle = 0, StageMetadata, PrimerTCP, Latency, DownloadTCP, UploadTCP, Done, Failed };

const char* PhaseName(Phase p);

struct Progress {
    Phase phase = Phase::Idle;
    double percent = 0.0;       // 0..100 within the current stage
    double currentMbps = 0.0;   // instantaneous rate for the current stage
    double avgMbps = 0.0;       // running average for this stage: total bytes / elapsed
    double pingMs = 0.0;
    double jitterMs = 0.0;
    std::string serverName;     // sponsor (bold line in the UI)
    std::string serverLocation; // city / name (grey line in the UI)
};

struct Server {
    std::string id;
    std::string name;
    std::string sponsor;
    std::string country;
    std::string host;
    std::string url;  // measurement base url, verbatim from the server list (data)
    double lat = 0.0;
    double lon = 0.0;
    bool sponsored = false;
    double adjustment = 0.0;

    std::string displayName() const;
};

// Knobs mirror the original's config keys. Every value is data; none is a compiled
// literal. Where the original's exact semantics are undocumented the interpretation is
// stated inline and reported as assumed.
struct Config {
    // --- discovery -----------------------------------------------------------------
    std::string configUrl = "https://www.speedtest.net/api/windows/config.php";
    // Fallback used only when the XML config lookup fails. OUR choice, not the original's.
    std::string fallbackServerListUrl = "https://www.speedtest.net/api/js/servers?engine=js&limit=20";

    // --- target path fragments ------------------------------------------------------
    std::string latencyFile = "latency.txt";  // confirmed literal in libooklasuite.dll
    std::string uploadFile = "upload.php";    // public Ookla protocol, not a binary literal
    std::vector<int> downloadSizes{400, 1000, 2000, 4000};  // ramped upward

    std::string serverOverride;  // id / host / url / name / sponsor substring; empty = auto

    // --- sizing knobs ---------------------------------------------------------------
    std::uint64_t downloadMaxBytesPerConnection = 0;  // 0 = unlimited
    std::uint64_t uploadMaxBytesPerConnection = 0;    // 0 = unlimited (per stream)
    std::uint32_t tcpDownloadBufferBytes = 64u * 1024u;
    std::uint32_t tcpUploadBufferBytes = 256u * 1024u;
    bool isScalingEnabled = false;   // ramp active connections up to threadCount
    double scalingFactor = 1.5;      // growth per scaling step (assumed semantics)
    bool stableStop = false;         // stop early once throughput is stable
    double stableStopWindowSeconds = 2.0;
    double stableStopTolerance = 0.10;  // relative spread considered stable
    double minTestDurationSeconds = 5.0;
    int threadCount = 4;
    int uploadThreadCount = 2;

    // --- server-provided flags ------------------------------------------------------
    bool enableServerUploadMeasurement = true;
    bool pping = true;

    // --- schedule / limits ----------------------------------------------------------
    int latencySamples = 6;      // samples after the PrimerTCP warm-up
    int serverProbeCount = 5;    // servers probed when no override is given
    int probeTimeoutMs = 2500;
    int requestTimeoutMs = 15000;
    double downloadSeconds = 10.0;  // upper bound for the download stage
    double uploadSeconds = 10.0;

    // Server list values that bound our defaults (0 = no server-provided bound).
    int serverDownloadMaxConnections = 0;
    int serverDownloadConnScalingWindowSize = 0;
};

// Mirrors the original's Ookla::PingDetails fields where they apply here.
struct Result {
    bool ok = false;
    std::string error;
    Server server;
    int serversFound = 0;
    std::vector<std::string> stages;  // stage names that ran, in order
    std::string configSource;
    std::string configFormat;  // "xml" or "json-fallback"

    std::string protocol;   // HTTP or HTTPS, from the selected server url
    std::string remoteIp;   // resolved address of the measurement host
    std::vector<double> pings;
    int pingsSuccess = 0;
    bool tcpError = false;
    std::string tcpErrorText;

    double pingMs = -1.0;   // latency
    double jitterMs = -1.0;
    double downloadMbps = -1.0;
    double uploadMbps = -1.0;
    std::uint64_t downloadBytes = 0;
    std::uint64_t uploadBytes = 0;
    double downloadSeconds = 0.0;
    double uploadSeconds = 0.0;
    int latencySamples = 0;
    int downloadConnectionsPeak = 0;
    std::string downloadStopReason;  // duration | stable | byte-cap | error
    std::string uploadStopReason;
};

class Engine {
public:
    using ProgressFn = std::function<void(const Progress&)>;

    Engine(Config cfg, ProgressFn onProgress);
    Result Run();

private:
    struct Discovery {
        std::vector<Server> servers;
        std::string sourceUrl;
        std::string format;  // xml | json-fallback
    };

    bool DiscoverServers(Discovery& out, std::string& err);
    bool FetchConfigXml(Discovery& out, std::string& err);
    bool FetchServerListJson(const std::string& url, Discovery& out, std::string& err);
    bool Primer(const std::string& latencyUrl, std::string& err);
    bool MeasureLatency(const std::string& latencyUrl, Result& res, std::string& err);
    bool MeasureDownload(const std::string& serverUrl, Result& res, std::string& err);
    bool MeasureUpload(const std::string& uploadUrl, Result& res, std::string& err);
    void Emit();

    Config cfg_;
    ProgressFn cb_;
    Progress prog_;
};

}  // namespace st
