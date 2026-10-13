// Entry point for the replicated Speedtest GUI (task-5).
//
//   speedtest-ui.exe                       run the GUI
//   speedtest-ui.exe --demo                GUI with synthetic, flagged values
//   speedtest-ui.exe --screen about        start on a specific screen
//   speedtest-ui.exe --screenshot FILE [--screen idle] [--demo] [--no-servers]
//                                          render offscreen to a PNG and exit
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "ui_app.h"

namespace {

std::string Narrow(const wchar_t* w) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    st::ui::AppOptions opt;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = Narrow(argv[i]);
            const bool hasNext = (i + 1) < argc;
            if (a == "--demo") {
                opt.demo = true;
            } else if (a == "--no-servers") {
                opt.fetchServers = false;
            } else if (a == "--screen" && hasNext) {
                opt.screen = Narrow(argv[++i]);
            } else if (a == "--server" && hasNext) {
                opt.serverOverride = Narrow(argv[++i]);
            } else if (a == "--share-endpoint" && hasNext) {
                // Test hook: forces the share upload to a specific endpoint so the
                // failure path can be observed. Not used in normal operation.
                opt.shareEndpoint = Narrow(argv[++i]);
            } else if (a == "--screenshot" && hasNext) {
                opt.screenshotPath = Narrow(argv[++i]);
            }
        }
        LocalFree(argv);
    }

    if (!opt.screenshotPath.empty()) return st::ui::RunScreenshot(opt);
    return st::ui::RunApp(inst, opt);
}
