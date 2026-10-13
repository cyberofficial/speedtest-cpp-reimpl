// UI state. Plain data; the renderer reads it, the app fills it.
// Every numeric field is filled from a real engine callback or a real config
// lookup. The only synthetic path is demo mode, which is flagged on screen.
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace st::ui {

enum class Screen { Idle, Testing, Results, History, Menu, About };

struct ServerRow {
    std::string id;
    std::string name;      // city / name from the server list
    std::string sponsor;
    std::string host;
    std::string url;       // measurement base url, as delivered (data)
    std::string location;  // display line under the name
};

struct HistRow {
    std::string when;  // real local date/time of the run, or DEMO-tagged
    double ping = 0.0;
    double dl = 0.0;
    double ul = 0.0;
    bool demo = false;
};

struct UiState {
    Screen screen = Screen::Idle;

    // server list (real data from the config / js fallback endpoint)
    std::vector<ServerRow> servers;
    int selected = -1;
    bool discovering = false;
    std::string status;  // honest one-line status: discovery error, demo notice, etc.

    // run state, all from st::Progress / st::Result
    bool running = false;
    bool demo = false;  // demo mode: every drawn number carries the DEMO banner
    st::Phase phase = st::Phase::Idle;
    double percent = 0.0;
    double curMbps = 0.0;  // instantaneous; kept for reference, never displayed
    // THE live throughput value shown during a throughput phase. Fed from
    // st::Progress::avgMbps (the running average), never from the instantaneous rate
    // and never from a max. The header metric, the gauge needle and the centre readout
    // all read this one field, so two numbers for one phase cannot disagree.
    double liveMbps = 0.0;
    double pingMs = 0.0;
    double jitterMs = 0.0;
    double dlMbps = -1.0;  // download phase value, from that same running average
    double ulMbps = -1.0;  // upload phase value, from that same running average
    std::string srvName;
    std::string srvLoc;

    bool hasLast = false;
    st::Result last;

    std::vector<HistRow> history;
    std::string lastWhen;

    // share (task-12). User-initiated only: nothing here is ever set at startup,
    // after a test, or on a timer.
    bool shareInFlight = false;
    std::string shareUrl;  // last link produced for this result
    // Toast: the success string is the original's literal, the failure string is
    // our own honest counterpart.
    bool toastActive = false;
    bool toastError = false;
    std::string toastText;
    std::string toastDetail;
    unsigned long long toastUntilMs = 0;
    unsigned long long lastShareAttemptMs = 0;

    // informational, shown on the About screen
    std::string version = "0.1.0";
    std::string serverListUrl;
    std::string configUrl;
};

// True when the drawn value for this state is synthetic and must be flagged.
inline bool NeedsDemoFlag(const UiState& s) { return s.demo; }

}  // namespace st::ui
