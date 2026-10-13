// Direct2D renderer for the replicated Speedtest UI.
// RenderFrame is used both by the window and by the offscreen screenshot path,
// so a saved PNG is exactly what the window shows.
#pragma once

#include <d2d1.h>

#include "ui_model.h"

struct IDWriteFactory;  // declared in <dwrite.h>; only a pointer is needed here

namespace st::ui {

struct DrawCache;  // owns D2D/DWrite objects created once per render target

DrawCache* CreateDrawCache(ID2D1Factory* factory, IDWriteFactory* dwrite);
void DestroyDrawCache(DrawCache* cache);

// Draws one frame of the current screen into rt at the fixed 384x556 layout.
void RenderFrame(ID2D1RenderTarget* rt, DrawCache* cache, const UiState& s);

enum class HitKind { None, Go, Back, MenuIcon, ServerRow, MenuAbout, MenuHistory, Gauge, Share };

struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;
};

// Hit test in client coordinates against the layout RenderFrame uses.
Hit HitTest(const UiState& s, float x, float y);

}  // namespace st::ui
