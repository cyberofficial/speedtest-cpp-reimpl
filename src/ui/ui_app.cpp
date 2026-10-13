// Window, engine driving, real server-list discovery, history persistence and the
// offscreen screenshot path for the replicated Speedtest UI (task-5).
//
// Numbers shown by the UI come from st::Engine progress/result callbacks or from
// the server list actually fetched over the network. The only synthetic path is
// demo mode (--demo), which draws a permanent DEMO DATA banner.
#include "ui_app.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "http_client.h"
#include "json.h"
#include "ui_draw.h"
#include "ui_share.h"
#include "ui_theme.h"

namespace st::ui {
namespace {

constexpr UINT kWmProgress = WM_APP + 1;
constexpr UINT kWmServers = WM_APP + 2;
constexpr UINT kWmDone = WM_APP + 3;
constexpr UINT kWmShare = WM_APP + 4;
constexpr int kTimerRedraw = 1;
constexpr int kTimerToast = 2;
constexpr int kMaxHistory = 50;
constexpr unsigned long long kToastMs = 6000;

// ---------------------------------------------------------------------------
// Shared state between the UI thread and the worker threads.
// ---------------------------------------------------------------------------
struct Shared {
    std::mutex mtx;
    st::Progress prog;
    bool hasProg = false;
    st::Result result;
    bool hasResult = false;
    std::vector<ServerRow> servers;
    bool serversReady = false;
    std::string status;
    ShareOutcome share;  // task-12 upload outcome
    bool hasShare = false;
};

struct Gui {
    HWND hwnd = nullptr;
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    DrawCache* cache = nullptr;
    UiState state;
    Shared shared;
    std::thread worker;
    std::thread shareThread;
    std::atomic<bool> busy{false};
    bool historyWritable = true;
    AppOptions opt;
};

std::string Narrow(const wchar_t* w) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string NowStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int hour = st.wHour % 12;
    if (hour == 0) hour = 12;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02u.%02u.%02u|%02d:%02d %s", st.wMonth, st.wDay, st.wYear % 100,
                  hour, st.wMinute, st.wHour < 12 ? "AM" : "PM");
    return buf;
}

// Program data lives beside the EXECUTABLE, never in LocalAppData.
// The folder is resolved from the running image (GetModuleFileNameW on our own
// module), not from the process CWD, because a shortcut or launcher can set a
// different working directory. If that folder cannot be written the app keeps
// running WITHOUT history; there is deliberately no LocalAppData fallback.
std::string ExeDirectory() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    const std::wstring image(buf, n);
    const std::size_t slash = image.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return Narrow(image.substr(0, slash).c_str());
}

std::string HistoryPath() {
    const std::string dir = ExeDirectory();
    if (dir.empty()) return std::string();
    return dir + "\\history.tsv";
}

// True when the history file can be created or appended beside the executable.
bool HistoryWritable() {
    const std::string path = HistoryPath();
    if (path.empty()) return false;
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return false;
    std::fclose(f);
    return true;
}

const char* const kHistoryNotWritable =
    "History is not writable next to the executable; results will not be saved. ";

std::string PipeToNewline(std::string s) {
    for (char& ch : s)
        if (ch == '|') ch = '\n';
    return s;
}

std::string NewlineToPipe(std::string s) {
    for (char& ch : s)
        if (ch == '\n' || ch == '\r') ch = '|';
    return s;
}

void LoadHistory(UiState& s) {
    const std::string path = HistoryPath();
    if (path.empty()) return;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        HistRow r;
        char when[256] = {0};
        double ping = 0.0, dl = 0.0, ul = 0.0;
        int demo = 0;
        if (std::sscanf(line, "%255[^\t]\t%lf\t%lf\t%lf\t%d", when, &ping, &dl, &ul, &demo) >= 4) {
            r.when = PipeToNewline(when);
            r.ping = ping;
            r.dl = dl;
            r.ul = ul;
            r.demo = demo != 0;
            s.history.push_back(r);
        }
    }
    std::fclose(f);
    if (static_cast<int>(s.history.size()) > kMaxHistory)
        s.history.erase(s.history.begin(), s.history.end() - kMaxHistory);
}

void AppendHistory(const HistRow& r) {
    const std::string path = HistoryPath();
    if (path.empty()) return;
    std::FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return;
    std::fprintf(f, "%s\t%.3f\t%.3f\t%.3f\t%d\n", NewlineToPipe(r.when).c_str(), r.ping, r.dl, r.ul,
                 r.demo ? 1 : 0);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Real server list discovery. Uses the engine's own JSON fallback endpoint and
// the engine's HTTP client; nothing here is invented.
// ---------------------------------------------------------------------------
void DiscoverServers(Gui* g) {
    st::HttpRequest req;
    req.url = g->state.serverListUrl;
    req.timeoutMs = 8000;
    req.userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    const st::HttpResult res = st::HttpPerform(req);

    std::vector<ServerRow> rows;
    std::string status;
    if (!res.ok || res.status != 200) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Server list unavailable (%s). Scanning for the nearest server at test time.",
                      res.error.empty() ? "http error" : res.error.c_str());
        status = buf;
    } else {
        st::JsonValue root;
        std::string err;
        if (!st::JsonParse(res.body, root, &err)) {
            status = "Server list unavailable (unreadable response). Scanning at test time.";
        } else {
            const st::JsonValue* arr = root.isArray() ? &root : root.find("servers");
            if (arr && arr->isArray()) {
                for (const st::JsonValue& item : arr->items) {
                    if (rows.size() >= 2) break;
                    if (!item.isObject()) continue;
                    ServerRow r;
                    const st::JsonValue* id = item.find("id");
                    r.id = id ? id->strOr() : std::string();
                    const st::JsonValue* sp = item.find("sponsor");
                    r.sponsor = sp ? sp->strOr() : std::string();
                    const st::JsonValue* nm = item.find("name");
                    r.name = nm ? nm->strOr() : std::string();
                    const st::JsonValue* ho = item.find("host");
                    r.host = ho ? ho->strOr() : std::string();
                    const st::JsonValue* ur = item.find("url");
                    r.url = ur ? ur->strOr() : std::string();
                    const st::JsonValue* cy = item.find("country");
                    const std::string country = cy ? cy->strOr() : std::string();
                    // The endpoint carries city + country; the original shows "City, ST".
                    // The state code is not present in this data, so only the city is shown.
                    r.location = r.name;
                    if (!country.empty() && country != "United States") r.location += ", " + country;
                    if (r.id.empty() && r.url.empty()) continue;
                    rows.push_back(r);
                }
            }
            if (rows.empty()) status = "Server list returned no usable entries.";
        }
    }

    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        g->shared.servers = rows;
        g->shared.status = status;
        g->shared.serversReady = true;
    }
    if (g->hwnd) PostMessageW(g->hwnd, kWmServers, 0, 0);
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
void Publish(Gui* g, const st::Progress& p) {
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        g->shared.prog = p;
        g->shared.hasProg = true;
    }
    if (g->hwnd) PostMessageW(g->hwnd, kWmProgress, 0, 0);
}

void RunEngine(Gui* g) {
    st::Config cfg;
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        if (!g->opt.serverOverride.empty()) {
            cfg.serverOverride = g->opt.serverOverride;
        } else if (g->state.selected >= 0 &&
                   g->state.selected < static_cast<int>(g->state.servers.size())) {
            cfg.serverOverride = g->state.servers[static_cast<size_t>(g->state.selected)].id;
        }
    }
    st::Engine engine(cfg, [g](const st::Progress& p) { Publish(g, p); });
    const st::Result r = engine.Run();
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        g->shared.result = r;
        g->shared.hasResult = true;
    }
    if (g->hwnd) PostMessageW(g->hwnd, kWmDone, 0, 0);
}

// Synthetic run used ONLY by --demo. The window draws a permanent DEMO DATA
// banner while this state is active, and saved rows are tagged DEMO.
void RunDemo(Gui* g) {
    const struct {
        st::Phase phase;
        double from;
        double to;
        int steps;
    } plan[] = {
        {st::Phase::StageMetadata, 0.0, 100.0, 6},
        {st::Phase::PrimerTCP, 0.0, 100.0, 5},
        {st::Phase::Latency, 18.0, 24.0, 8},
        {st::Phase::DownloadTCP, 0.0, 135.5, 26},
        {st::Phase::UploadTCP, 0.0, 18.4, 22},
    };
    for (const auto& step : plan) {
        for (int i = 1; i <= step.steps; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(step.steps);
            st::Progress p;
            p.phase = step.phase;
            p.percent = 100.0 * t;
            const double v = step.from + (step.to - step.from) * t;
            if (step.phase == st::Phase::Latency) {
                p.pingMs = v;
                p.jitterMs = 4.2;
                p.currentMbps = v;
            } else {
                p.currentMbps = v;
            }
            p.serverName = "Demo Sponsor";
            p.serverLocation = "Demo City, US";
            Publish(g, p);
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
        }
    }
    st::Result r;
    r.ok = true;
    r.pingMs = 21.0;
    r.jitterMs = 4.2;
    r.downloadMbps = 135.47;
    r.uploadMbps = 18.36;
    r.protocol = "HTTP";
    r.remoteIp = "203.0.113.7";
    r.server.sponsor = "Demo Sponsor";
    r.server.name = "Demo City";
    r.serversFound = 0;
    r.configSource = "demo";
    r.configFormat = "demo";
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        g->shared.result = r;
        g->shared.hasResult = true;
    }
    if (g->hwnd) PostMessageW(g->hwnd, kWmDone, 0, 0);
}

// ---------------------------------------------------------------------------
// Window plumbing
// ---------------------------------------------------------------------------
void StartTest(Gui* g) {
    if (g->busy.load()) return;
    g->busy.store(true);
    g->state.running = true;
    g->state.phase = st::Phase::Idle;
    g->state.percent = 0.0;
    g->state.curMbps = 0.0;
    g->state.pingMs = 0.0;
    g->state.dlMbps = -1.0;
    g->state.ulMbps = -1.0;
    g->state.liveMbps = 0.0;
    g->state.screen = Screen::Testing;
    g->state.status.clear();
    if (g->worker.joinable()) g->worker.join();
    g->worker = std::thread(g->state.demo ? RunDemo : RunEngine, g);
    SetTimer(g->hwnd, kTimerRedraw, 50, nullptr);
}

void ApplyProgress(Gui* g) {
    st::Progress p;
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        if (!g->shared.hasProg) return;
        p = g->shared.prog;
    }
    g->state.phase = p.phase;
    g->state.percent = p.percent;
    g->state.curMbps = p.currentMbps;  // reference only: deliberately not displayed
    g->state.pingMs = p.pingMs;
    g->state.jitterMs = p.jitterMs;
    g->state.srvName = p.serverName;
    g->state.srvLoc = p.serverLocation;
    // The displayed throughput is the engine's running average for the stage, so it
    // converges to the value that gets recorded. The old max-latch is deleted on
    // purpose: no comparison and no parallel max, because that ratchet was the defect.
    if (p.phase == st::Phase::DownloadTCP) {
        g->state.dlMbps = p.avgMbps;
        g->state.liveMbps = p.avgMbps;
    } else if (p.phase == st::Phase::UploadTCP) {
        g->state.ulMbps = p.avgMbps;
        g->state.liveMbps = p.avgMbps;
    }
}

void ApplyResult(Gui* g) {
    st::Result r;
    {
        std::lock_guard<std::mutex> lk(g->shared.mtx);
        if (!g->shared.hasResult) return;
        r = g->shared.result;
    }
    if (g->worker.joinable()) g->worker.join();
    g->busy.store(false);
    g->state.running = false;
    if (g->hwnd) KillTimer(g->hwnd, kTimerRedraw);

    if (!r.ok) {
        g->state.status = "Test failed: " + (r.error.empty() ? std::string("unknown error") : r.error);
        g->state.screen = Screen::Idle;
        return;
    }
    g->state.hasLast = true;
    g->state.last = r;
    g->state.pingMs = r.pingMs;
    g->state.dlMbps = r.downloadMbps;
    g->state.ulMbps = r.uploadMbps;
    g->state.curMbps = r.downloadMbps;
    g->state.lastWhen = PipeToNewline(NowStamp());

    HistRow row;
    row.when = g->state.lastWhen;
    row.ping = r.pingMs;
    row.dl = r.downloadMbps;
    row.ul = r.uploadMbps;
    row.demo = g->state.demo;
    if (row.demo) row.when = "DEMO " + row.when;
    g->state.history.push_back(row);
    if (static_cast<int>(g->state.history.size()) > kMaxHistory)
        g->state.history.erase(g->state.history.begin());
    AppendHistory(row);
    g->state.screen = Screen::Results;
}

void ShowToast(Gui* g, const std::string& text, const std::string& detail, bool error) {
    g->state.toastText = text;
    g->state.toastDetail = detail;
    g->state.toastError = error;
    g->state.toastActive = true;
    g->state.toastUntilMs = GetTickCount64() + kToastMs;
    if (g->hwnd) {
        SetTimer(g->hwnd, kTimerToast, 400, nullptr);
        InvalidateRect(g->hwnd, nullptr, FALSE);
    }
}

// Share (task-12). USER-INITIATED ONLY: nothing calls this at startup, after a
// test, or on a timer. The original copies the link and toasts immediately and
// does not wait for the upload, so the upload runs on its own thread and the
// outcome only ever adjusts the toast afterwards.
void StartShare(Gui* g) {
    if (g->state.shareInFlight) return;
    if (!g->state.hasLast) {
        ShowToast(g, "Could not share result.", "no completed measurement to share", true);
        return;
    }

    const std::string guid = NewGuidString();
    const std::string url = BuildShareUrl(guid);
    if (url.empty()) {
        ShowToast(g, "Could not share result.", "could not create a result id", true);
        return;
    }

    std::string err;
    if (!CopyTextToClipboard(WidenUtf8(url), &err)) {
        ShowToast(g, "Could not share result.", "clipboard: " + err, true);
        return;
    }
    g->state.shareUrl = url;
    g->state.shareInFlight = true;
    g->state.lastShareAttemptMs = GetTickCount64();
    // The original's literal toast string.
    ShowToast(g, "Sharing link copied to clipboard.", url, false);

    if (g->shareThread.joinable()) g->shareThread.join();
    const st::Result snapshot = g->state.last;
    const std::string endpoint = g->opt.shareEndpoint;
    g->shareThread = std::thread([g, snapshot, url, endpoint]() {
        const ShareOutcome o = UiShareUpload(snapshot, url, endpoint);
        {
            std::lock_guard<std::mutex> lk(g->shared.mtx);
            g->shared.share = o;
            g->shared.hasShare = true;
        }
        if (g->hwnd) PostMessageW(g->hwnd, kWmShare, 0, 0);
    });
}

void HandleClick(Gui* g, float x, float y) {
    const Hit hit = HitTest(g->state, x, y);
    switch (hit.kind) {
        case HitKind::Go:
            StartTest(g);
            break;
        case HitKind::MenuIcon:
            g->state.screen = Screen::Menu;
            break;
        case HitKind::Back:
            g->state.screen = Screen::Idle;
            break;
        case HitKind::MenuAbout:
            g->state.screen = Screen::About;
            break;
        case HitKind::MenuHistory:
            g->state.screen = Screen::History;
            break;
        case HitKind::ServerRow:
            if (hit.index >= 0 && hit.index < static_cast<int>(g->state.servers.size())) {
                g->state.selected = hit.index;
            }
            break;
        case HitKind::Share:
            StartShare(g);
            break;
        case HitKind::Gauge:
        case HitKind::None:
            break;
    }
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void ReleaseDevices(Gui* g) {
    DestroyDrawCache(g->cache);
    g->cache = nullptr;
    if (g->rt) {
        g->rt->Release();
        g->rt = nullptr;
    }
}

bool CreateDevices(Gui* g) {
    if (!g->factory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g->factory))) return false;
    }
    if (!g->dwrite) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&g->dwrite))))
            return false;
    }
    if (!g->cache) g->cache = CreateDrawCache(g->factory, g->dwrite);
    if (!g->rt && g->hwnd) {
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(kClientW),
                                             static_cast<UINT32>(kClientH));
        if (FAILED(g->factory->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(g->hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                &g->rt)))
            return false;
    }
    return g->cache != nullptr && (g->rt != nullptr || g->hwnd == nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Gui* g = reinterpret_cast<Gui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_CREATE:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (g && g->rt && g->cache) RenderFrame(g->rt, g->cache, g->state);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            RECT r = {0, 0, static_cast<LONG>(kClientW), static_cast<LONG>(kClientH)};
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            AdjustWindowRectEx(&r, style, FALSE, ex);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            mmi->ptMaxTrackSize = mmi->ptMinTrackSize;
            return 0;
        }
        case WM_TIMER:
            if (g && wp == kTimerRedraw) {
                ApplyProgress(g);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g && wp == kTimerToast) {
                if (!g->state.toastActive || GetTickCount64() > g->state.toastUntilMs) {
                    g->state.toastActive = false;
                    KillTimer(hwnd, kTimerToast);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case kWmProgress:
            if (g) {
                ApplyProgress(g);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case kWmServers:
            if (g) {
                std::lock_guard<std::mutex> lk(g->shared.mtx);
                g->state.servers = g->shared.servers;
                g->state.status =
                    (g->historyWritable ? std::string() : std::string(kHistoryNotWritable)) +
                    g->shared.status;
                g->state.discovering = false;
                if (g->state.selected < 0 && !g->state.servers.empty()) g->state.selected = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case kWmDone:
            if (g) {
                ApplyResult(g);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case kWmShare:
            if (g) {
                ShareOutcome o;
                {
                    std::lock_guard<std::mutex> lk(g->shared.mtx);
                    if (!g->shared.hasShare) return 0;
                    o = g->shared.share;
                }
                if (g->shareThread.joinable()) g->shareThread.join();
                g->state.shareInFlight = false;
                if (o.ok) {
                    // Prefer the link the upload actually published.
                    if (!o.url.empty() && o.url != g->state.shareUrl) {
                        std::string err;
                        if (CopyTextToClipboard(WidenUtf8(o.url), &err)) g->state.shareUrl = o.url;
                    }
                    ShowToast(g, "Sharing link copied to clipboard.", g->state.shareUrl, false);
                } else {
                    // Visible failure: never leave the success toast standing.
                    ShowToast(g, "Could not share result.",
                              o.detail.empty() ? std::string("upload failed") : o.detail, true);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (g) HandleClick(g, static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            return 0;
        case WM_KEYDOWN:
            if (g && wp == VK_ESCAPE) {
                if (!g->state.running && g->state.screen != Screen::Idle) {
                    g->state.screen = Screen::Idle;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_DESTROY:
            if (g) {
                if (g->worker.joinable()) g->worker.join();
                if (g->shareThread.joinable()) g->shareThread.join();
                ReleaseDevices(g);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

Screen ScreenFromName(const std::string& name) {
    if (name == "testing") return Screen::Testing;
    if (name == "results") return Screen::Results;
    if (name == "history") return Screen::History;
    if (name == "menu") return Screen::Menu;
    if (name == "about") return Screen::About;
    return Screen::Idle;
}

int RunApp(HINSTANCE inst, const AppOptions& opt) {
    Gui gui;
    gui.opt = opt;
    gui.state.demo = opt.demo;
    gui.state.serverListUrl = st::Config().fallbackServerListUrl;
    gui.state.configUrl = st::Config().configUrl;
    gui.state.screen = ScreenFromName(opt.screen);
    LoadHistory(gui.state);
    gui.historyWritable = HistoryWritable();
    if (!gui.historyWritable) gui.state.status = kHistoryNotWritable;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"SpeedtestReimplWnd";
    if (!RegisterClassExW(&wc)) return 2;

    RECT r = {0, 0, static_cast<LONG>(kClientW), static_cast<LONG>(kClientH)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int ww = r.right - r.left;
    const int wh = r.bottom - r.top;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Speedtest", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr, nullptr, inst, &gui);
    if (!hwnd) return 3;
    gui.hwnd = hwnd;
    if (!CreateDevices(&gui)) {
        MessageBoxW(hwnd, L"Failed to create the Direct2D devices.", L"Speedtest", MB_ICONERROR);
        DestroyWindow(hwnd);
        return 4;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (opt.fetchServers) {
        gui.state.discovering = true;
        std::thread(DiscoverServers, &gui).detach();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gui.worker.joinable()) gui.worker.join();
    return 0;
}

// ---------------------------------------------------------------------------
// Offscreen screenshot: the same RenderFrame the window uses, into a WIC bitmap.
// ---------------------------------------------------------------------------
int RunScreenshot(const AppOptions& opt) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 10;

    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* bitmap = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    int rc = 0;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) rc = 11;
    if (rc == 0 &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite))))
        rc = 12;
    if (rc == 0 &&
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        rc = 13;

    const UINT32 pw = static_cast<UINT32>(kClientW);
    const UINT32 ph = static_cast<UINT32>(kClientH);
    if (rc == 0 &&
        FAILED(wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 &bitmap)))
        rc = 14;
    if (rc == 0) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap, props, &rt))) rc = 15;
    }

    if (rc == 0) {
        UiState state;
        state.demo = opt.demo;
        state.screen = ScreenFromName(opt.screen);
        state.serverListUrl = st::Config().fallbackServerListUrl;
        state.configUrl = st::Config().configUrl;
        LoadHistory(state);

        if (opt.fetchServers) {
            Gui probe;
            probe.state = state;
            DiscoverServers(&probe);  // returns immediately if the lookup fails
            std::lock_guard<std::mutex> lk(probe.shared.mtx);
            state.servers = probe.shared.servers;
            state.status = (HistoryWritable() ? std::string() : std::string(kHistoryNotWritable)) +
                           probe.shared.status;
            if (state.selected < 0 && !state.servers.empty()) state.selected = 0;
        }

        // For the testing screen without a live run there is nothing real to show,
        // so the gauge rests at zero unless demo mode supplies a flagged sweep.
        if (state.screen == Screen::Testing && state.demo) {
            state.phase = st::Phase::DownloadTCP;
            state.pingMs = 21.0;
            state.dlMbps = 135.47;
            state.curMbps = 135.47;
        }

        DrawCache* cache = CreateDrawCache(factory, dwrite);
        if (!cache) {
            rc = 16;
        } else {
            RenderFrame(rt, cache, state);  // BeginDraw/EndDraw are inside
            DestroyDrawCache(cache);
        }
    }

    if (rc == 0) {
        const std::wstring path = st::ToWide(opt.screenshotPath);
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* bag = nullptr;
        if (FAILED(wic->CreateStream(&stream))) rc = 17;
        if (rc == 0 && FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) rc = 18;
        if (rc == 0 && FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
            rc = 19;
        if (rc == 0 && FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) rc = 20;
        if (rc == 0 && FAILED(encoder->CreateNewFrame(&frame, &bag))) rc = 21;
        if (rc == 0 && FAILED(frame->Initialize(bag))) rc = 22;
        if (rc == 0 && FAILED(frame->SetSize(pw, ph))) rc = 23;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
        if (rc == 0 && FAILED(frame->SetPixelFormat(&fmt))) rc = 24;
        if (rc == 0 && FAILED(frame->WriteSource(bitmap, nullptr))) rc = 25;
        if (rc == 0 && FAILED(frame->Commit())) rc = 26;
        if (rc == 0 && FAILED(encoder->Commit())) rc = 27;
        if (bag) bag->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
    }

    if (rt) rt->Release();
    if (bitmap) bitmap->Release();
    if (wic) wic->Release();
    if (dwrite) dwrite->Release();
    if (factory) factory->Release();
    CoUninitialize();
    return rc;
}

}  // namespace st::ui
