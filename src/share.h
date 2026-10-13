// Result SHARE upload (task-11).
//
// Protocol recovered from the original's managed assembly (analysis\share-protocol.md)
// and validated live by the Lead:
//   POST https://napi.speedtest.net/reports/ -> {"guid":"...","save":{"desktop_result_id":N},"status":"ok"}
// No auth, no account, no Ookla binary: the seed travels in X-Cache-Id, so the salt is ours.
//
// PRIVACY: this is the only call the app makes to a host other than the measurement
// server and www.speedtest.net, and it must run ONLY when the user asks to share.
// Nothing here runs at startup, after a test, or on a timer.
#pragma once
#include <string>

#include "engine.h"

namespace st {

struct ShareOutcome {
    bool ok = false;
    std::string url;     // https://www.speedtest.net/my-result/d/<guid> on success
    std::string detail;  // raw server response text, or the failure reason
};

struct ShareContext {
    std::string appVersion = "1.15.200.1";
    std::string deviceId;   // empty = derived, stable per machine
    std::string osVersion;  // empty = Windows build string
    std::string endpoint = "https://napi.speedtest.net/reports/";
    // Empty = read <client ip="..."> from the config endpoint below (the server needs a
    // real IP: an empty externalIp makes the insert fail with ip_address cannot be null).
    std::string externalIp;
    std::string configUrl = "https://www.speedtest.net/api/windows/config.php";
    // Overrides for the start/end timestamps the payload sends. The wire form is a decimal
    // string of Unix seconds; set these only to pin them (tests).
    std::string startIso;   // empty = derived from the result's stage durations
    std::string endIso;     // empty = now
    int serverId = 0;       // 0 = take from result.server.id
};

// One entry point for the UI.
ShareOutcome ShareUpload(const Result& result, const ShareContext& ctx = ShareContext());

}  // namespace st
