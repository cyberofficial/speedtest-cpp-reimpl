// Share plumbing for the Direct2D UI (task-12).
//
// Two responsibilities:
//   1. the Win32 clipboard path, which closes the clipboard on every exit path;
//   2. the SINGLE call site for the task-11 upload entry point (st::ShareUpload).
//
// The link the original copies is "https://www.speedtest.net/my-result/d/" + guid
// (analysis\share-protocol.md section 7).
#pragma once

#include <string>

#include "engine.h"

namespace st::ui {

// Mirrors the agreed task-11 outcome shape.
struct ShareOutcome {
    bool ok = false;
    std::string url;
    std::string detail;
};

// Link prefix, verbatim from the original.
std::string ShareUrlPrefix();

// "https://www.speedtest.net/my-result/d/" + guid
std::string BuildShareUrl(const std::string& guid);

// A real OS GUID string. Empty when CoCreateGuid fails; never invented locally.
std::string NewGuidString();

// CF_UNICODETEXT copy. Closes the clipboard on every path, including every failure.
bool CopyTextToClipboard(const std::wstring& text, std::string* error);

// THE ONE call site for the task-11 upload. Called from a worker thread, never
// from the UI thread. Returns ok=false with a detail string when the upload fails.
// endpoint: empty uses the default from st::ShareContext. A non-empty value is a
// test hook (--share-endpoint) so the failure path can be forced and observed.
ShareOutcome UiShareUpload(const st::Result& result, const std::string& url,
                           const std::string& endpoint = std::string());

}  // namespace st::ui
