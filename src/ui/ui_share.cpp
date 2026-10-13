// Share plumbing for the Direct2D UI (task-12): clipboard + the task-11 call site.
#include "ui_share.h"

#include <windows.h>

#include <objbase.h>

#include <cstdio>
#include <cstring>

// ===========================================================================
// THE ONE CALL SITE for the task-11 share upload.
//
// Expected interface, per the task brief and task-12's description:
//     struct st::ShareOutcome { bool ok; std::string url; std::string detail; };
//     st::ShareOutcome st::ShareUpload(const st::Result& result, const std::string& url);
//
// When impl-app publishes reimpl\src\share.h this picks it up automatically. If the
// real signature differs, THIS function is the only place that needs changing.
// While the header is absent the UI reports an honest failure instead of pretending
// a link was published.
// ===========================================================================
#if defined(__has_include)
#if __has_include("share.h")
#include "share.h"
#define ST_UI_HAS_SHARE_HEADER 1
#endif
#endif
#ifndef ST_UI_HAS_SHARE_HEADER
#define ST_UI_HAS_SHARE_HEADER 0
#endif

namespace st::ui {
namespace {

const char* const kUrlPrefix = "https://www.speedtest.net/my-result/d/";

std::string NarrowW(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

}  // namespace

std::string ShareUrlPrefix() { return kUrlPrefix; }

std::string BuildShareUrl(const std::string& guid) {
    if (guid.empty()) return std::string();
    return std::string(kUrlPrefix) + guid;
}

std::string NewGuidString() {
    GUID g;
    std::memset(&g, 0, sizeof(g));
    if (FAILED(CoCreateGuid(&g))) return std::string();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(g.Data1), static_cast<unsigned>(g.Data2),
                  static_cast<unsigned>(g.Data3), g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string(buf);
}

// Opens, empties, sets and ALWAYS closes the clipboard. Ownership of the global
// memory block passes to the clipboard only when SetClipboardData succeeds.
bool CopyTextToClipboard(const std::wstring& text, std::string* error) {
    if (text.empty()) {
        if (error) *error = "nothing to copy";
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        if (error) *error = "OpenClipboard failed";
        return false;
    }
    bool ok = false;
    do {
        if (!EmptyClipboard()) {
            if (error) *error = "EmptyClipboard failed";
            break;
        }
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!mem) {
            if (error) *error = "GlobalAlloc failed";
            break;
        }
        void* dst = GlobalLock(mem);
        if (!dst) {
            GlobalFree(mem);
            if (error) *error = "GlobalLock failed";
            break;
        }
        std::memcpy(dst, text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) {
            GlobalFree(mem);  // ownership was not transferred
            if (error) *error = "SetClipboardData failed";
            break;
        }
        ok = true;
    } while (false);

    CloseClipboard();  // every path, success or failure
    return ok;
}

ShareOutcome UiShareUpload(const st::Result& result, const std::string& url,
                           const std::string& endpoint) {
    ShareOutcome out;
#if ST_UI_HAS_SHARE_HEADER && defined(ST_UI_HAVE_SHARE_IMPL)
    // ---- THE task-11 call site -------------------------------------------------
    // st::ShareOutcome st::ShareUpload(const st::Result&, const st::ShareContext& = {});
    // The result already carries its server, so no extra context is needed unless a
    // test hook overrides the endpoint.
    st::ShareContext ctx;
    if (!endpoint.empty()) ctx.endpoint = endpoint;
    const st::ShareOutcome r = st::ShareUpload(result, ctx);
    out.ok = r.ok;
    out.url = r.url.empty() ? url : r.url;
    out.detail = r.detail;
    // ---------------------------------------------------------------------------
#else
    (void)endpoint;
    out.ok = false;
    out.url = url;
#if ST_UI_HAS_SHARE_HEADER
    out.detail = "share upload not linked: reimpl\\src\\share.cpp is not in this build yet";
#else
    out.detail = "share upload not available: reimpl\\src\\share.h is missing";
#endif
#endif
    return out;
}

}  // namespace st::ui
