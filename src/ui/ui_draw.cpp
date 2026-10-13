// Direct2D drawing for the replicated Speedtest UI (task-5).
//
// Layout, colours and gauge geometry come from design\design-spec.md, measured
// against the original's own client-area captures (design\screenshots\*.png).
// All glyphs (ring mark, sliders, arrows, dial icons, chevrons, gauge needle)
// are drawn as vector shapes here. No Ookla bitmap or logo is embedded.
#include "ui_draw.h"

#include <d2d1helper.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>

#include "ui_theme.h"

namespace st::ui {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kSweepDeg = 280.0f;  // 132 deg -> 52 deg, measured

struct FmtKey {
    float size = 0.0f;
    int weight = 0;
    int gauge = 0;
    int ha = 0;
    int va = 0;
    int wrap = 0;
    bool operator<(const FmtKey& o) const {
        if (size != o.size) return size < o.size;
        if (weight != o.weight) return weight < o.weight;
        if (gauge != o.gauge) return gauge < o.gauge;
        if (ha != o.ha) return ha < o.ha;
        if (va != o.va) return va < o.va;
        return wrap < o.wrap;
    }
};

}  // namespace

struct DrawCache {
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1StrokeStyle* roundStroke = nullptr;
    std::map<FmtKey, IDWriteTextFormat*> formats;
    std::map<std::pair<const void*, UINT32>, ID2D1SolidColorBrush*> brushes;
};

namespace {

float Rad(float deg) { return deg * kPi / 180.0f; }

D2D1_POINT_2F OnCircle(float cx, float cy, float r, float deg) {
    const float a = Rad(deg);
    return D2D1::Point2F(cx + r * std::cos(a), cy + r * std::sin(a));
}

IDWriteTextFormat* Fmt(DrawCache* c, float size, DWRITE_FONT_WEIGHT weight, bool gauge,
                       DWRITE_TEXT_ALIGNMENT ha, DWRITE_PARAGRAPH_ALIGNMENT va, bool wrap) {
    FmtKey k;
    k.size = size;
    k.weight = static_cast<int>(weight);
    k.gauge = gauge ? 1 : 0;
    k.ha = static_cast<int>(ha);
    k.va = static_cast<int>(va);
    k.wrap = wrap ? 1 : 0;
    auto it = c->formats.find(k);
    if (it != c->formats.end()) return it->second;

    IDWriteTextFormat* f = nullptr;
    HRESULT hr = c->dwrite->CreateTextFormat(gauge ? kFontGauge : kFontUi, nullptr, weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             size, L"en-us", &f);
    if (FAILED(hr) || !f) return nullptr;
    f->SetTextAlignment(ha);
    f->SetParagraphAlignment(va);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    c->formats.emplace(k, f);
    return f;
}

ID2D1SolidColorBrush* Brush(DrawCache* c, ID2D1RenderTarget* rt, UINT32 rgb, float a = 1.0f) {
    const UINT32 key32 = (rgb << 8) | static_cast<UINT32>(a * 255.0f + 0.5f);
    const std::pair<const void*, UINT32> key(static_cast<const void*>(rt), key32);
    auto it = c->brushes.find(key);
    if (it != c->brushes.end()) return it->second;
    ID2D1SolidColorBrush* b = nullptr;
    if (FAILED(rt->CreateSolidColorBrush(Col(rgb, a), &b))) return nullptr;
    c->brushes.emplace(key, b);
    return b;
}

void Text(ID2D1RenderTarget* rt, DrawCache* c, const std::wstring& s, float x, float y, float w,
          float h, float size, DWRITE_FONT_WEIGHT weight, UINT32 rgb, float a = 1.0f,
          bool gauge = false, DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_CENTER,
          DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, bool wrap = false) {
    if (s.empty()) return;
    IDWriteTextFormat* f = Fmt(c, size, weight, gauge, ha, va, wrap);
    if (!f) return;
    ID2D1SolidColorBrush* b = Brush(c, rt, rgb, a);
    if (!b) return;
    const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                  DWRITE_MEASURING_MODE_NATURAL);
}

ID2D1PathGeometry* ArcGeo(ID2D1Factory* f, float cx, float cy, float r, float a0, float sweep) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(f->CreatePathGeometry(&g))) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(g->Open(&sink))) {
        g->Release();
        return nullptr;
    }
    const D2D1_POINT_2F p0 = OnCircle(cx, cy, r, a0);
    const D2D1_POINT_2F p1 = OnCircle(cx, cy, r, a0 + sweep);
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(r, r), 0.0f,
                                  sweep >= 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                                : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  std::fabs(sweep) > 180.0f ? D2D1_ARC_SIZE_LARGE
                                                            : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return g;
}

void StrokeArc(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, float r, float a0,
               float sweep, ID2D1Brush* brush, float width, bool round) {
    ID2D1PathGeometry* g = ArcGeo(c->factory, cx, cy, r, a0, sweep);
    if (!g) return;
    rt->DrawGeometry(g, brush, width, round ? c->roundStroke : nullptr);
    g->Release();
}

ID2D1LinearGradientBrush* Gradient(ID2D1RenderTarget* rt, D2D1_POINT_2F s, D2D1_POINT_2F e,
                                   UINT32 c0, UINT32 c1, float a0 = 1.0f, float a1 = 1.0f) {
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f;
    stops[0].color = Col(c0, a0);
    stops[1].position = 1.0f;
    stops[1].color = Col(c1, a1);
    ID2D1GradientStopCollection* gsc = nullptr;
    if (FAILED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                &gsc)))
        return nullptr;
    ID2D1LinearGradientBrush* b = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(s, e), gsc, &b);
    gsc->Release();
    return b;
}

void Line(ID2D1RenderTarget* rt, DrawCache* c, float x0, float y0, float x1, float y1, UINT32 rgb,
          float width, float a = 1.0f, bool round = false) {
    ID2D1SolidColorBrush* b = Brush(c, rt, rgb, a);
    if (!b) return;
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), b, width,
                 round ? c->roundStroke : nullptr);
}

void Ring(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, float r, UINT32 rgb, float width,
          float a = 1.0f) {
    ID2D1SolidColorBrush* b = Brush(c, rt, rgb, a);
    if (!b) return;
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b, width);
}

void Disc(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, float r, UINT32 rgb,
          float a = 1.0f) {
    ID2D1SolidColorBrush* b = Brush(c, rt, rgb, a);
    if (!b) return;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
}

std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring Num(double v, int decimals) {
    if (v < 0.0) return L"...";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return W(buf);
}

std::wstring NumI(double v) {
    if (v < 0.0) return L"...";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return W(buf);
}

// ---------------------------------------------------------------------------
// Gauge scale.
//
// MEASURED anchors (design-spec.md 3.2): value 0 sits at 132 deg and value 20
// sits at 268 deg. The intermediate tick angles are UNKNOWN in the spec, so the
// mapping below is a logarithmic fit solved to pass through BOTH measured
// anchors: f(v) = ln(1 + v/a) / ln(1 + 500/a) with a = 1.06.
// The intermediate angles are therefore INTERPOLATED, not measured.
// ---------------------------------------------------------------------------
constexpr double kScaleA = 1.06;

float ValueFraction(double v) {
    if (v <= 0.0) return 0.0f;
    double f = std::log(1.0 + v / kScaleA) / std::log(1.0 + 500.0 / kScaleA);
    if (f > 1.0) f = 1.0;
    return static_cast<float>(f);
}

float ValueAngle(double v) { return kGaugeStartDeg + kSweepDeg * ValueFraction(v); }

// ---------------------------------------------------------------------------
// Shared chrome
// ---------------------------------------------------------------------------

// The wordmark: our own ring glyph (an open ring with a dot) plus the app name
// in Montserrat. Drawn, never a bitmap.
void Wordmark(ID2D1RenderTarget* rt, DrawCache* c, bool dim) {
    const float cx = 132.0f;
    const float cy = 29.0f;
    const UINT32 col = kTextGrey;
    const float a = dim ? 0.75f : 1.0f;
    StrokeArc(rt, c, cx, cy, 7.5f, 300.0f, 300.0f, Brush(c, rt, col, a), 1.8f, true);
    Disc(rt, c, cx + 7.5f * std::cos(Rad(30.0f)), cy + 7.5f * std::sin(Rad(30.0f)), 1.8f, col, a);
    Text(rt, c, L"SPEEDTEST", 146.0f, 18.0f, 130.0f, 22.0f, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
         kTextGrey, a, false, DWRITE_TEXT_ALIGNMENT_LEADING);
}

void BackArrow(ID2D1RenderTarget* rt, DrawCache* c) {
    Line(rt, c, 27.0f, 21.0f, 18.0f, 29.5f, kTextGrey, 2.0f, 1.0f, true);
    Line(rt, c, 18.0f, 29.5f, 27.0f, 38.0f, kTextGrey, 2.0f, 1.0f, true);
}

// Three slider lines, the original's top-right menu control.
void MenuIcon(ID2D1RenderTarget* rt, DrawCache* c) {
    const float x0 = 342.0f;
    const float x1 = 360.0f;
    const float ys[3] = {22.5f, 29.5f, 36.5f};
    const float knob[3] = {352.0f, 347.0f, 356.0f};
    for (int i = 0; i < 3; ++i) {
        Line(rt, c, x0, ys[i], x1, ys[i], kTextGrey, 1.4f, 1.0f, true);
        Disc(rt, c, knob[i], ys[i], 2.0f, kTextGrey);
    }
}

void ChevronRight(ID2D1RenderTarget* rt, DrawCache* c, float x, float cy, UINT32 rgb) {
    Line(rt, c, x, cy - 6.0f, x + 6.0f, cy, rgb, 2.0f, 1.0f, true);
    Line(rt, c, x + 6.0f, cy, x, cy + 6.0f, rgb, 2.0f, 1.0f, true);
}

// Small dial icon: a ring plus a glyph (down arrow, up arrow, or a tilde).
void DialIcon(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, float r, UINT32 rgb,
              int kind, float a) {
    Ring(rt, c, cx, cy, r, rgb, 1.4f, a);
    const float s = r * 0.45f;
    if (kind == 1) {  // download: arrow down
        Line(rt, c, cx, cy - s, cx, cy + s, rgb, 1.3f, a, true);
        Line(rt, c, cx - s * 0.8f, cy + s * 0.2f, cx, cy + s, rgb, 1.3f, a, true);
        Line(rt, c, cx + s * 0.8f, cy + s * 0.2f, cx, cy + s, rgb, 1.3f, a, true);
    } else if (kind == 2) {  // upload: arrow up
        Line(rt, c, cx, cy + s, cx, cy - s, rgb, 1.3f, a, true);
        Line(rt, c, cx - s * 0.8f, cy - s * 0.2f, cx, cy - s, rgb, 1.3f, a, true);
        Line(rt, c, cx + s * 0.8f, cy - s * 0.2f, cx, cy - s, rgb, 1.3f, a, true);
    } else {  // ping: a small wave
        Line(rt, c, cx - s, cy + s * 0.4f, cx - s * 0.3f, cy - s * 0.6f, rgb, 1.3f, a, true);
        Line(rt, c, cx - s * 0.3f, cy - s * 0.6f, cx + s * 0.3f, cy + s * 0.6f, rgb, 1.3f, a, true);
        Line(rt, c, cx + s * 0.3f, cy + s * 0.6f, cx + s, cy - s * 0.4f, rgb, 1.3f, a, true);
    }
}

// Server row icon: a circle with a small left/right arrow pair inside.
void ServerIcon(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, float r) {
    Ring(rt, c, cx, cy, r, kRowMuted, 1.3f);
    Line(rt, c, cx - 7.0f, cy - 2.5f, cx + 7.0f, cy - 2.5f, kRowMuted, 1.3f, 1.0f, true);
    Line(rt, c, cx - 7.0f, cy + 2.5f, cx + 4.0f, cy + 2.5f, kRowMuted, 1.3f, 1.0f, true);
    Line(rt, c, cx + 7.0f, cy - 2.5f, cx + 4.0f, cy - 5.0f, kRowMuted, 1.3f, 1.0f, true);
    Line(rt, c, cx + 7.0f, cy - 2.5f, cx + 4.0f, cy + 0.0f, kRowMuted, 1.3f, 1.0f, true);
    Line(rt, c, cx - 7.0f, cy + 2.5f, cx - 4.0f, cy + 5.0f, kRowMuted, 1.3f, 1.0f, true);
    Line(rt, c, cx - 7.0f, cy + 2.5f, cx - 4.0f, cy + 0.0f, kRowMuted, 1.3f, 1.0f, true);
}

void ServerList(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    for (size_t i = 0; i < s.servers.size() && i < 2; ++i) {
        const float cy = kRowTop + kRowH * static_cast<float>(i) + 24.0f;
        const bool sel = (static_cast<int>(i) == s.selected);
        ServerIcon(rt, c, kIconCx, cy, kIconR);
        if (sel) Disc(rt, c, kIconCx, cy + kIconR + 4.0f, 3.5f, kGreen);

        const std::wstring name = W(s.servers[i].sponsor.empty() ? s.servers[i].name
                                                                 : s.servers[i].sponsor);
        const UINT32 nameCol = sel ? kLink : kRowSecondary;
        Text(rt, c, name, kRowTextX, cy - 17.0f, 250.0f, 20.0f, 15.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, nameCol, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (sel) {
            // Approximate advance width; the selected row carries a small chevron.
            const float wApprox = 8.6f * static_cast<float>(name.size());
            ChevronRight(rt, c, kRowTextX + wApprox + 8.0f, cy - 7.0f, kLink);
        }
        Text(rt, c, W(s.servers[i].location), kRowTextX, cy + 1.0f, 250.0f, 18.0f, 12.5f,
             DWRITE_FONT_WEIGHT_LIGHT, kRowMuted, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void StatusLine(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    if (s.status.empty()) return;
    Text(rt, c, W(s.status), 20.0f, 430.0f, 344.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_LIGHT,
         kTickGray, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void DemoBanner(ID2D1RenderTarget* rt, DrawCache* c) {
    ID2D1SolidColorBrush* b = Brush(c, rt, 0x7A1F1F);
    if (b) rt->FillRectangle(D2D1::RectF(0.0f, 0.0f, kClientW, 22.0f), b);
    Text(rt, c, L"DEMO DATA - synthetic values, not measured", 0.0f, 4.0f, kClientW, 16.0f, 11.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Metric columns, shared by Testing and Results.
struct ColumnSpec {
    const wchar_t* label;
    int iconKind;
    UINT32 accent;
    float x0;
    float x1;
    float valueSize;
};

const ColumnSpec kColumns[3] = {
    {L"PING", 0, kLabelPing, 10.0f, 120.0f, 26.0f},
    {L"DOWNLOAD", 1, kGreen, 130.0f, 254.0f, 34.0f},
    {L"UPLOAD", 2, kMagenta, 264.0f, 374.0f, 26.0f},
};

void MetricColumns(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s, float top,
                   const std::wstring values[3], const wchar_t* units[3], const bool shown[3],
                   const bool active[3]) {
    for (int i = 0; i < 3; ++i) {
        const ColumnSpec& col = kColumns[i];
        const float w = col.x1 - col.x0;
        const float cxm = (col.x0 + col.x1) * 0.5f;
        const UINT32 labelCol = active[i] ? (i == 0 ? kLabelPing : kLabelActive) : kTextDim;

        // Icons stay legible in every state: full-opacity accent when the column is
        // active, the spec's dimmed grey (#464652) when it is not. They are never
        // alpha-faded into the background and never drawn over the value.
        DialIcon(rt, c, cxm - 34.0f, top + 8.0f, 7.5f, active[i] ? col.accent : kTextDim,
                 col.iconKind, 1.0f);
        Text(rt, c, col.label, cxm - 22.0f, top, 90.0f, 16.0f, 11.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, labelCol, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);

        const std::wstring v = shown[i] ? values[i] : std::wstring(L"...");
        Text(rt, c, v, col.x0, top + 20.0f, w, 42.0f, col.valueSize, DWRITE_FONT_WEIGHT_SEMI_BOLD,
             kWhite, active[i] ? 1.0f : 0.45f, false, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Text(rt, c, units[i], col.x0, top + 62.0f, w, 16.0f, 11.5f, DWRITE_FONT_WEIGHT_LIGHT,
             active[i] ? (i == 2 ? kMagenta : kLabelPing) : kTextDim, 1.0f, false,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

void DrawIdle(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    Wordmark(rt, c, false);
    MenuIcon(rt, c);

    Ring(rt, c, kDialCx, kDialCy, kDialOuterR, kRingUnlit, 1.0f);
    Ring(rt, c, kDialCx, kDialCy, kDialAccentR, kYellow, 2.0f);
    StrokeArc(rt, c, kDialCx, kDialCy, kDialAccentR, 300.0f, 120.0f,
              Brush(c, rt, kYellowHi, 0.9f), 2.0f, true);
    Text(rt, c, L"GO", kDialCx - 80.0f, kDialCy - 26.0f, 160.0f, 52.0f, 40.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kYellow);

    StatusLine(rt, c, s);
    ServerList(rt, c, s);
}

void DrawTesting(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    BackArrow(rt, c);
    Wordmark(rt, c, true);

    const bool pingShown = s.phase != st::Phase::Idle && s.phase != st::Phase::StageMetadata;
    const bool dlShown = s.phase == st::Phase::DownloadTCP || s.phase == st::Phase::UploadTCP ||
                         s.phase == st::Phase::Done;
    const bool ulShown = s.phase == st::Phase::UploadTCP || s.phase == st::Phase::Done;

    std::wstring values[3] = {NumI(s.pingMs), Num(s.dlMbps, 2), Num(s.ulMbps, 2)};
    const wchar_t* units[3] = {L"ms", L"Mbps", L"Mbps"};
    const bool shown[3] = {pingShown, dlShown, ulShown};
    const bool active[3] = {s.phase == st::Phase::PrimerTCP || s.phase == st::Phase::Latency,
                            s.phase == st::Phase::DownloadTCP, s.phase == st::Phase::UploadTCP};
    MetricColumns(rt, c, s, 50.0f, values, units, shown, active);

    // Gauge band: remainder, then the progress arc, then a thin inner track.
    ID2D1SolidColorBrush* rem = Brush(c, rt, kArcRemainder);
    if (rem) StrokeArc(rt, c, kGaugeCx, kGaugeCy, kGaugeBandR, kGaugeStartDeg, kSweepDeg, rem,
                       kGaugeBandW, false);

    // ONE live value drives the header metric, the gauge needle and the readout below:
    // the engine's running average. The instantaneous rate is never drawn here, so two
    // numbers for one phase cannot disagree on screen.
    const bool isPing = (s.phase == st::Phase::Latency);
    const double live = isPing ? s.pingMs : s.liveMbps;
    const float sweep = kSweepDeg * ValueFraction(live);
    if (sweep > 0.5f) {
        const D2D1_POINT_2F ps = OnCircle(kGaugeCx, kGaugeCy, kGaugeBandR, kGaugeStartDeg);
        const D2D1_POINT_2F pe = OnCircle(kGaugeCx, kGaugeCy, kGaugeBandR, kGaugeStartDeg + sweep);
        ID2D1LinearGradientBrush* pg = Gradient(rt, ps, pe, kMagenta, kMagentaTip);
        if (pg) {
            StrokeArc(rt, c, kGaugeCx, kGaugeCy, kGaugeBandR, kGaugeStartDeg, sweep, pg,
                      kGaugeBandW, false);
            pg->Release();
        }
    }
    Ring(rt, c, kGaugeCx, kGaugeCy, kGaugeTrackR, kArcTrack, 2.0f);

    // Tick labels, Gauge Mono replaced by a monospaced face.
    const double ticks[9] = {0, 2, 5, 10, 20, 50, 150, 300, 500};
    for (double t : ticks) {
        const D2D1_POINT_2F p = OnCircle(kGaugeCx, kGaugeCy, kGaugeTickR, ValueAngle(t));
        Text(rt, c, Num(t, 0), p.x - 20.0f, p.y - 8.0f, 40.0f, 16.0f, 11.0f,
             DWRITE_FONT_WEIGHT_NORMAL, kTickGray, 1.0f, true, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Needle: tapered bar, light at the tip, darker at the hub.
    {
        const float ang = Rad(ValueAngle(live));
        const float dx = std::cos(ang);
        const float dy = std::sin(ang);
        const float px = -dy;
        const float py = dx;
        const float rBase = 8.0f;
        const float rTip = 95.0f;
        const float wBase = 3.0f;
        const float wTip = 1.2f;
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(c->factory->CreatePathGeometry(&g)) && g) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(g->Open(&sink))) {
                D2D1_POINT_2F pts[4];
                pts[0] = D2D1::Point2F(kGaugeCx + dx * rBase + px * wBase,
                                       kGaugeCy + dy * rBase + py * wBase);
                pts[1] = D2D1::Point2F(kGaugeCx + dx * rTip + px * wTip,
                                       kGaugeCy + dy * rTip + py * wTip);
                pts[2] = D2D1::Point2F(kGaugeCx + dx * rTip - px * wTip,
                                       kGaugeCy + dy * rTip - py * wTip);
                pts[3] = D2D1::Point2F(kGaugeCx + dx * rBase - px * wBase,
                                       kGaugeCy + dy * rBase - py * wBase);
                sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLines(pts + 1, 3);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                ID2D1LinearGradientBrush* nb = Gradient(
                    rt, D2D1::Point2F(kGaugeCx + dx * rTip, kGaugeCy + dy * rTip),
                    D2D1::Point2F(kGaugeCx, kGaugeCy), kNeedleTop, kNeedleBase);
                if (nb) {
                    rt->FillGeometry(g, nb);
                    nb->Release();
                }
            }
            g->Release();
        }
        Disc(rt, c, kGaugeCx, kGaugeCy, 4.5f, kBg);
        Ring(rt, c, kGaugeCx, kGaugeCy, 4.5f, kNeedleBase, 1.2f);
    }

    // Centred value readout below the dial: the same value as the needle above.
    const double readout = live;
    Text(rt, c, Num(readout, isPing ? 0 : 2), 92.0f, 386.0f, 200.0f, 36.0f, 30.0f,
         DWRITE_FONT_WEIGHT_NORMAL, kWhite, 1.0f, true, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DialIcon(rt, c, 168.0f, 428.0f, 6.0f, isPing ? kTickGray : kMagenta, isPing ? 0 : 1, 1.0f);
    Text(rt, c, isPing ? L"ms" : L"Mbps", 178.0f, 420.0f, 60.0f, 16.0f, 12.0f,
         DWRITE_FONT_WEIGHT_LIGHT, isPing ? kTickGray : kMagenta, 1.0f, false,
         DWRITE_TEXT_ALIGNMENT_LEADING);

    StatusLine(rt, c, s);
    ServerList(rt, c, s);
}

// Share control (task-12). Glyph plus label in the established palette: the
// divider colour outlines it, kLink marks it as the actionable accent. No new
// visual style is introduced.
void ShareGlyph(ID2D1RenderTarget* rt, DrawCache* c, float cx, float cy, UINT32 rgb, float a) {
    Line(rt, c, cx - 7.0f, cy + 1.0f, cx - 7.0f, cy + 7.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx + 7.0f, cy + 1.0f, cx + 7.0f, cy + 7.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx - 7.0f, cy + 7.0f, cx + 7.0f, cy + 7.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx - 7.0f, cy + 1.0f, cx - 3.0f, cy + 1.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx + 3.0f, cy + 1.0f, cx + 7.0f, cy + 1.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx, cy - 8.0f, cx, cy + 2.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx - 4.0f, cy - 4.0f, cx, cy - 8.0f, rgb, 1.5f, a, true);
    Line(rt, c, cx + 4.0f, cy - 4.0f, cx, cy - 8.0f, rgb, 1.5f, a, true);
}

void ShareControl(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s, float y0) {
    const bool enabled = s.hasLast;
    const UINT32 rgb = enabled ? kLink : kTextDim;
    ID2D1SolidColorBrush* b = Brush(c, rt, kDivider, enabled ? 1.0f : 0.6f);
    if (b) {
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(288.0f, y0, 368.0f, y0 + 26.0f), 3.0f, 3.0f), b, 1.0f);
    }
    ShareGlyph(rt, c, 302.0f, y0 + 14.0f, rgb, 1.0f);
    Text(rt, c, s.shareInFlight ? L"SHARING" : L"SHARE", 314.0f, y0 + 6.0f, 50.0f, 16.0f, 11.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, rgb, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
}

// Toast: the original shows one on share. Success is its literal string; the
// failure variant is ours and is drawn with a warm border so it is not mistaken
// for success. The detail line carries the raw reason.
void DrawToast(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    if (!s.toastActive) return;
    const bool withDetail = !s.toastDetail.empty();
    const float h = withDetail ? 62.0f : 44.0f;
    const float y0 = kClientH - h - 16.0f;
    const D2D1_ROUNDED_RECT rr =
        D2D1::RoundedRect(D2D1::RectF(28.0f, y0, kClientW - 28.0f, y0 + h), 4.0f, 4.0f);
    ID2D1SolidColorBrush* fill = Brush(c, rt, kPanel);
    if (fill) rt->FillRoundedRectangle(rr, fill);
    ID2D1SolidColorBrush* border = Brush(c, rt, s.toastError ? 0xB03A2E : kGreen);
    if (border) rt->DrawRoundedRectangle(rr, border, 1.2f);
    Text(rt, c, W(s.toastText), 34.0f, y0 + 8.0f, kClientW - 68.0f, 20.0f, 12.5f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (withDetail) {
        Text(rt, c, W(s.toastDetail), 34.0f, y0 + 30.0f, kClientW - 68.0f, 26.0f, 10.5f,
             DWRITE_FONT_WEIGHT_LIGHT, kRowMuted, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    }
}

void DrawResults(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    BackArrow(rt, c);
    Wordmark(rt, c, true);
    Text(rt, c, L"RESULT", 0.0f, 52.0f, kClientW, 24.0f, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
         kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    ShareControl(rt, c, s, 50.0f);

    std::wstring values[3] = {NumI(s.last.pingMs), Num(s.last.downloadMbps, 2),
                              Num(s.last.uploadMbps, 2)};
    const wchar_t* units[3] = {L"ms", L"Mbps", L"Mbps"};
    const bool shown[3] = {true, true, true};
    const bool active[3] = {true, true, true};
    MetricColumns(rt, c, s, 88.0f, values, units, shown, active);

    // Server block. Line 1 is the sponsor, line 2 the city (name), exactly the
    // mapping the idle server rows use. The sponsor is never printed twice, and
    // date / time / protocol each get their own line with a sane line height.
    const float y = 208.0f;
    const std::string& sponsor = s.last.server.sponsor;
    std::string line1 = sponsor.empty() ? s.last.server.name : sponsor;
    std::string line2 = s.last.server.name;
    if (line2.empty()) line2 = s.last.server.country;
    if (line2.empty()) line2 = s.last.server.host;
    if (line2 == line1) line2.clear();

    Text(rt, c, W(line1), 20.0f, y, 344.0f, 22.0f, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!line2.empty()) {
        Text(rt, c, W(line2), 20.0f, y + 24.0f, 344.0f, 18.0f, 12.5f, DWRITE_FONT_WEIGHT_LIGHT,
             kRowMuted, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    std::string date1 = s.lastWhen;
    std::string time1;
    const std::size_t nl = s.lastWhen.find('\n');
    if (nl != std::string::npos) {
        date1 = s.lastWhen.substr(0, nl);
        time1 = s.lastWhen.substr(nl + 1);
    }
    Text(rt, c, W(date1), 20.0f, y + 48.0f, 344.0f, 18.0f, 12.0f, DWRITE_FONT_WEIGHT_LIGHT,
         kRowMuted, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!time1.empty()) {
        Text(rt, c, W(time1), 20.0f, y + 66.0f, 344.0f, 18.0f, 12.0f, DWRITE_FONT_WEIGHT_LIGHT,
             kRowMuted, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    if (!s.last.protocol.empty()) {
        Text(rt, c, W(s.last.protocol + "  " + s.last.remoteIp), 20.0f, y + 88.0f, 344.0f, 18.0f,
             11.0f, DWRITE_FONT_WEIGHT_LIGHT, kTickGray, 1.0f, false,
             DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    Line(rt, c, 20.0f, 320.0f, 364.0f, 320.0f, kDivider, 1.0f);
    Text(rt, c, L"RESULT HISTORY", 32.0f, 336.0f, 300.0f, 22.0f, 13.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    ChevronRight(rt, c, 350.0f, 347.0f, kTextGrey);
    StatusLine(rt, c, s);
}

void DrawHistoryTable(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s, float top, int maxRows) {
    ID2D1SolidColorBrush* strip = Brush(c, rt, kRowBg);
    if (strip) rt->FillRectangle(D2D1::RectF(0.0f, top, kClientW, top + 45.0f), strip);
    ID2D1SolidColorBrush* body = Brush(c, rt, kPanel);
    if (body) rt->FillRectangle(D2D1::RectF(0.0f, top + 45.0f, kClientW, kClientH), body);

    const float hy = top + 12.0f;
    const UINT32 hc = kLabelPing;
    DialIcon(rt, c, 40.0f, hy + 8.0f, 6.0f, kTickGray, 0, 1.0f);
    Text(rt, c, L"Date", 52.0f, hy, 60.0f, 16.0f, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, hc, 1.0f,
         false, DWRITE_TEXT_ALIGNMENT_LEADING);
    DialIcon(rt, c, 148.0f, hy + 8.0f, 6.0f, kTickGray, 0, 1.0f);
    Text(rt, c, L"ms", 160.0f, hy, 60.0f, 16.0f, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, hc, 1.0f, false,
         DWRITE_TEXT_ALIGNMENT_LEADING);
    DialIcon(rt, c, 226.0f, hy + 8.0f, 6.0f, kGreen, 1, 1.0f);
    Text(rt, c, L"Mbps", 238.0f, hy, 70.0f, 16.0f, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, hc, 1.0f,
         false, DWRITE_TEXT_ALIGNMENT_LEADING);
    DialIcon(rt, c, 306.0f, hy + 8.0f, 6.0f, kMagenta, 2, 1.0f);
    Text(rt, c, L"Mbps", 318.0f, hy, 70.0f, 16.0f, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, hc, 1.0f,
         false, DWRITE_TEXT_ALIGNMENT_LEADING);

    const int shown = std::min<int>(maxRows, static_cast<int>(s.history.size()));
    for (int i = 0; i < shown; ++i) {
        const HistRow& r = s.history[s.history.size() - 1 - static_cast<size_t>(i)];
        const float ry = top + 45.0f + 64.0f * static_cast<float>(i) + 12.0f;
        Ring(rt, c, 40.0f, ry + 20.0f, 16.0f, kRowMuted, 1.2f);
        Line(rt, c, 34.0f, ry + 20.0f, 46.0f, ry + 20.0f, kRowMuted, 1.2f);
        Text(rt, c, W(r.when), 64.0f, ry, 90.0f, 40.0f, 11.5f, DWRITE_FONT_WEIGHT_LIGHT, kRowMuted,
             1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
        Text(rt, c, NumI(r.ping), 140.0f, ry + 6.0f, 60.0f, 24.0f, 17.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(rt, c, Num(r.dl, 2), 210.0f, ry + 6.0f, 90.0f, 24.0f, 17.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(rt, c, Num(r.ul, 2), 300.0f, ry + 6.0f, 74.0f, 24.0f, 17.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, kRowSecondary, 1.0f, false,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (shown == 0) {
        Text(rt, c, L"No tests yet - run a speed test to fill this list.", 20.0f,
             top + 45.0f + 24.0f, 344.0f, 20.0f, 12.0f, DWRITE_FONT_WEIGHT_LIGHT, kRowMuted, 1.0f,
             false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void DrawHistory(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    BackArrow(rt, c);
    Wordmark(rt, c, true);
    Text(rt, c, L"RESULT HISTORY", 0.0f, 52.0f, kClientW, 24.0f, 16.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    // Export (share) and delete glyphs, drawn as shapes. The export glyph is the
    // share control on this screen, so it carries the actionable accent colour.
    const UINT32 glyphCol = s.hasLast ? kLink : kTextDim;
    Line(rt, c, 312.0f, 62.0f, 312.0f, 74.0f, glyphCol, 1.6f);
    Line(rt, c, 306.0f, 68.0f, 318.0f, 68.0f, glyphCol, 1.6f);
    Line(rt, c, 305.0f, 62.0f, 319.0f, 62.0f, glyphCol, 1.6f);
    Line(rt, c, 348.0f, 58.0f, 356.0f, 58.0f, kTextGrey, 1.6f);
    Line(rt, c, 349.0f, 60.0f, 355.0f, 60.0f, kTextGrey, 1.6f);
    Line(rt, c, 350.0f, 60.0f, 350.0f, 76.0f, kTextGrey, 1.6f);
    Line(rt, c, 354.0f, 60.0f, 354.0f, 76.0f, kTextGrey, 1.6f);
    Line(rt, c, 346.0f, 76.0f, 358.0f, 76.0f, kTextGrey, 1.6f);

    DrawHistoryTable(rt, c, s, 95.0f, 6);
    StatusLine(rt, c, s);
}

void DrawMenu(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    BackArrow(rt, c);
    Wordmark(rt, c, true);

    // Stats band, computed from the real history only.
    double highest = -1.0;
    double sum = 0.0;
    int n = 0;
    for (const HistRow& r : s.history) {
        if (r.dl >= 0.0) {
            highest = std::max(highest, r.dl);
            sum += r.dl;
            ++n;
        }
    }
    const double avg = n > 0 ? sum / n : -1.0;
    const UINT32 lc = kWhite;
    const float cols[3] = {64.0f, 192.0f, 320.0f};
    const wchar_t* labels[3] = {L"TESTS", L"HIGHEST SPEED", L"AVG SPEED"};
    DialIcon(rt, c, 20.0f, 74.0f, 6.0f, kTickGray, 0, 1.0f);
    DialIcon(rt, c, 148.0f, 74.0f, 6.0f, kTickGray, 0, 1.0f);
    DialIcon(rt, c, 276.0f, 74.0f, 6.0f, kTickGray, 0, 1.0f);
    Text(rt, c, labels[0], 32.0f, 66.0f, 70.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lc,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, labels[1], 160.0f, 66.0f, 90.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lc,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, labels[2], 288.0f, 66.0f, 80.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, lc,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, NumI(static_cast<double>(n)), 0.0f, 88.0f, 128.0f, 40.0f, 30.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(rt, c, Num(highest, 2), 128.0f, 88.0f, 128.0f, 40.0f, 30.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(rt, c, Num(avg, 2), 256.0f, 88.0f, 128.0f, 40.0f, 30.0f,
         DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(rt, c, L"Mbps", 128.0f, 130.0f, 128.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_LIGHT,
         kLabelPing, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(rt, c, L"Mbps", 256.0f, 130.0f, 128.0f, 16.0f, 11.0f, DWRITE_FONT_WEIGHT_LIGHT,
         kLabelPing, 1.0f, false, DWRITE_TEXT_ALIGNMENT_CENTER);

    Line(rt, c, 0.0f, 160.0f, kClientW, 160.0f, kDivider, 1.0f);
    const wchar_t* rows[2] = {L"ABOUT SPEEDTEST", L"RESULT HISTORY"};
    for (int i = 0; i < 2; ++i) {
        const float y = 160.0f + 52.0f * static_cast<float>(i);
        Text(rt, c, rows[i], 32.0f, y + 16.0f, 300.0f, 22.0f, 13.0f,
             DWRITE_FONT_WEIGHT_SEMI_BOLD, kWhite, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        ChevronRight(rt, c, 350.0f, y + 27.0f, kTextGrey);
        Line(rt, c, 20.0f, y + 52.0f, 364.0f, y + 52.0f, kDivider, 1.0f);
    }

    DrawHistoryTable(rt, c, s, 268.0f, 3);
    StatusLine(rt, c, s);
}

void DrawAbout(ID2D1RenderTarget* rt, DrawCache* c, const UiState& s) {
    BackArrow(rt, c);
    Wordmark(rt, c, true);

    const std::wstring body =
        L"This application is an independent reimplementation of the Speedtest desktop client for "
        L"Windows. It measures the latency, download and upload throughput of your internet "
        L"connection against a nearby measurement server, and keeps a local history of the runs "
        L"performed on this machine.\n"
        L"Measurement results are produced by the bundled measurement engine and are not adjusted, "
        L"rounded up, or estimated. History is stored only on this computer; nothing is uploaded to "
        L"us and no analytics are collected.";
    Text(rt, c, body, 20.0f, 88.0f, 344.0f, 300.0f, 13.0f, DWRITE_FONT_WEIGHT_LIGHT, kRowSecondary,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    Text(rt, c, L"Privacy Policy", 20.0f, 452.0f, 120.0f, 20.0f, 13.0f,
         DWRITE_FONT_WEIGHT_NORMAL, kLink, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, L"|", 138.0f, 452.0f, 12.0f, 20.0f, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, kTickGray,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, L"Terms of Use", 152.0f, 452.0f, 140.0f, 20.0f, 13.0f,
         DWRITE_FONT_WEIGHT_NORMAL, kLink, 1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);

    const std::wstring ver = L"v" + W(s.version);
    Text(rt, c, ver, 20.0f, 486.0f, 80.0f, 20.0f, 12.5f, DWRITE_FONT_WEIGHT_LIGHT, kRowMuted, 1.0f,
         false, DWRITE_TEXT_ALIGNMENT_LEADING);
    Text(rt, c, L"(Update)", 76.0f, 486.0f, 90.0f, 20.0f, 12.5f, DWRITE_FONT_WEIGHT_LIGHT, kLink,
         1.0f, false, DWRITE_TEXT_ALIGNMENT_LEADING);

    Text(rt, c, L"2026 Speedtest reimplementation. All rights reserved.", 20.0f, 520.0f, 344.0f,
         20.0f, 12.5f, DWRITE_FONT_WEIGHT_LIGHT, kRowSecondary, 1.0f, false,
         DWRITE_TEXT_ALIGNMENT_LEADING);
}

}  // namespace

DrawCache* CreateDrawCache(ID2D1Factory* factory, IDWriteFactory* dwrite) {
    DrawCache* c = new DrawCache();
    c->factory = factory;
    c->dwrite = dwrite;
    factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f),
        nullptr, 0, &c->roundStroke);
    return c;
}

void DestroyDrawCache(DrawCache* cache) {
    if (!cache) return;
    for (auto& kv : cache->formats) kv.second->Release();
    for (auto& kv : cache->brushes) kv.second->Release();
    if (cache->roundStroke) cache->roundStroke->Release();
    delete cache;
}

void RenderFrame(ID2D1RenderTarget* rt, DrawCache* cache, const UiState& s) {
    if (!rt || !cache) return;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(Col(kBg));

    switch (s.screen) {
        case Screen::Idle: DrawIdle(rt, cache, s); break;
        case Screen::Testing: DrawTesting(rt, cache, s); break;
        case Screen::Results: DrawResults(rt, cache, s); break;
        case Screen::History: DrawHistory(rt, cache, s); break;
        case Screen::Menu: DrawMenu(rt, cache, s); break;
        case Screen::About: DrawAbout(rt, cache, s); break;
    }
    if (s.demo) DemoBanner(rt, cache);
    DrawToast(rt, cache, s);
    rt->EndDraw();
}

Hit HitTest(const UiState& s, float x, float y) {
    const float dx = x - kDialCx;
    const float dy = y - kDialCy;
    const bool overGo = (dx * dx + dy * dy) <= (kDialOuterR * kDialOuterR);

    switch (s.screen) {
        case Screen::Idle:
            if (overGo) return {HitKind::Go, -1};
            if (x >= 330.0f && x <= 372.0f && y >= 10.0f && y <= 46.0f)
                return {HitKind::MenuIcon, -1};
            break;
        case Screen::Testing:
            if (x >= 10.0f && x <= 40.0f && y >= 12.0f && y <= 44.0f) return {HitKind::Back, -1};
            if (overGo || (dx * dx + dy * dy) <= 124.0f * 124.0f) return {HitKind::Gauge, -1};
            break;
        case Screen::Menu:
            if (x >= 10.0f && x <= 40.0f && y >= 12.0f && y <= 44.0f) return {HitKind::Back, -1};
            if (y >= 160.0f && y < 212.0f) return {HitKind::MenuAbout, -1};
            if (y >= 212.0f && y < 264.0f) return {HitKind::MenuHistory, -1};
            break;
        case Screen::Results:
            if (x >= 10.0f && x <= 40.0f && y >= 12.0f && y <= 44.0f) return {HitKind::Back, -1};
            if (x >= 288.0f && x <= 372.0f && y >= 48.0f && y <= 80.0f) return {HitKind::Share, -1};
            if (y >= 320.0f && y <= 372.0f) return {HitKind::MenuHistory, -1};
            break;
        case Screen::History:
            if (x >= 10.0f && x <= 40.0f && y >= 12.0f && y <= 44.0f) return {HitKind::Back, -1};
            // the export glyph in this header is the share control
            if (x >= 300.0f && x <= 332.0f && y >= 52.0f && y <= 84.0f) return {HitKind::Share, -1};
            break;
        case Screen::About:
            if (x >= 10.0f && x <= 40.0f && y >= 12.0f && y <= 44.0f) return {HitKind::Back, -1};
            break;
    }
    // Server rows are present on the idle and testing screens.
    if (s.screen == Screen::Idle || s.screen == Screen::Testing) {
        if (x >= kRowX0 && x <= kRowX1 && y >= kRowTop && y < kRowTop + kRowH)
            return {HitKind::ServerRow, 0};
        if (x >= kRowX0 && x <= kRowX1 && y >= kRowTop + kRowH && y < kRowTop + 2.0f * kRowH)
            return {HitKind::ServerRow, 1};
    }
    return {HitKind::None, -1};
}

}  // namespace st::ui
