// UI application entry points.
#pragma once

#include <windows.h>

#include <string>

#include "ui_model.h"

namespace st::ui {

struct AppOptions {
    bool demo = false;            // synthetic values, flagged on screen
    std::string screenshotPath;   // when set: render offscreen, save PNG, exit
    std::string screen = "idle";  // startup screen for the app / for the screenshot
    std::string serverOverride;   // force a server: id / host / name substring
    std::string shareEndpoint;    // test hook: override the share upload endpoint
    bool fetchServers = true;     // attempt the real server-list lookup
};

// Parses "idle|testing|results|history|menu|about" into a screen.
Screen ScreenFromName(const std::string& name);

// Runs the GUI until the window closes. Returns the process exit code.
int RunApp(HINSTANCE inst, const AppOptions& opt);

// Renders the requested screen offscreen and writes it as a PNG. Returns 0 on success.
int RunScreenshot(const AppOptions& opt);

}  // namespace st::ui
