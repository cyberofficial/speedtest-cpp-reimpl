// Shared visual constants for the Direct2D UI (task-5).
//
// Every colour below is MEASURED from the original's client-area captures in
// design\screenshots\*.png (design-spec.md section 2) unless the line is tagged
// ASSUMED. Nothing here is copied from an Ookla bitmap: all glyphs are drawn as
// vector shapes at run time.
#pragma once

#include <d2d1.h>

namespace st::ui {

// ---------------------------------------------------------------------------
// Fonts. The ONLY place a face name appears, so it can be swapped in one edit.
// Montserrat-Regular IS installed on this host; DirectWrite synthesises SemiBold
// and Light from it (see docs\ui-notes.md). The original's gauge face is
// "Gauge Mono", which is not installed, so a monospaced face is used instead.
// ---------------------------------------------------------------------------
inline const wchar_t* const kFontUi = L"Montserrat";
inline const wchar_t* const kFontGauge = L"Consolas";

// ---------------------------------------------------------------------------
// Layout, in client pixels. Client area is 384 x 556 (design-spec.md section 0).
// ---------------------------------------------------------------------------
constexpr float kClientW = 384.0f;
constexpr float kClientH = 556.0f;
constexpr float kHeaderH = 48.0f;

// Server list (idle and testing share it).
constexpr float kRowX0 = 20.0f;
constexpr float kRowX1 = 380.0f;
constexpr float kRowTop = 452.0f;
constexpr float kRowH = 50.0f;
constexpr float kIconCx = 44.0f;
constexpr float kIconR = 20.0f;
constexpr float kRowTextX = 68.0f;

// Idle hero dial.
constexpr float kDialCx = 192.0f;
constexpr float kDialCy = 250.0f;
constexpr float kDialOuterR = 110.0f;
constexpr float kDialAccentR = 100.0f;

// Gauge. Centre and band radii measured on testing_mid.png (design-spec.md 3.2).
constexpr float kGaugeCx = 192.0f;
constexpr float kGaugeCy = 288.0f;
constexpr float kGaugeBandR = 110.5f;  // (97 + 124) / 2, band thickness 27
constexpr float kGaugeBandW = 27.0f;
constexpr float kGaugeTrackR = 97.0f;
constexpr float kGaugeStartDeg = 132.0f;  // measured: scale start
constexpr float kGaugeEndDeg = 52.0f;     // measured: scale end; 280 deg clockwise sweep
constexpr float kGaugeTickR = 78.0f;      // tick-label radius, inside the band
constexpr float kGaugeMax = 500.0f;

// ---------------------------------------------------------------------------
// Palette. Hex values as measured.
// ---------------------------------------------------------------------------
constexpr UINT32 kBg = 0x0B0B1B;            // app background, all screens
constexpr UINT32 kArcRemainder = 0x232F4E;  // gauge remainder arc
constexpr UINT32 kArcTrack = 0x171D34;      // gauge inner track
constexpr UINT32 kMagenta = 0xFF71ED;       // progress arc start, upload accent
constexpr UINT32 kMagentaTip = 0xFF71F2;    // progress arc tip
constexpr UINT32 kGreen = 0x75FF9C;         // download accent + selected-server dot
constexpr UINT32 kYellow = 0xFFE400;        // GO button
constexpr UINT32 kYellowHi = 0xFFF38E;      // GO highlight ring
constexpr UINT32 kRingUnlit = 0x33312E;     // idle dial unlit ring
constexpr UINT32 kLink = 0x21B1FF;          // link / selected server name
constexpr UINT32 kWhite = 0xFFFFFF;
constexpr UINT32 kLabelActive = 0xE9E9EA;
constexpr UINT32 kLabelPing = 0xC7C7CA;
constexpr UINT32 kTextDim = 0x464652;   // inactive metric label/value/unit
constexpr UINT32 kTickGray = 0xA9A9AE;
constexpr UINT32 kRowMuted = 0x7B7B84;
constexpr UINT32 kRowSecondary = 0xBDBDC1;
constexpr UINT32 kTextGrey = 0x8B8DA2;  // wordmark and secondary body text
constexpr UINT32 kNeedleTop = 0xDEDEE0;
constexpr UINT32 kNeedleBase = 0xA3A3AB;
constexpr UINT32 kPanel = 0x1A1B2E;    // history body panel
constexpr UINT32 kRowBg = 0x303143;     // history column-header strip
constexpr UINT32 kDivider = 0x232F4E;

inline D2D1_COLOR_F Col(UINT32 rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

}  // namespace st::ui
