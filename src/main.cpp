// ============================================================================
//  Wi-Fi e-ink label  —  ESP32-C3 SuperMini + 2.13" e-paper (250 x 122)
//
//  Update the panel from a phone browser, or over USB serial:
//      TEXT:<string>   QR:<string>   CLEAR
//
//  Content survives reboots in NVS and is redrawn once on boot.
// ============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Update.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>
#include <qrcode.h>

#include "web_ui.h"

// ─────────────────────────────────────────────────────────────────────────────
//  PIN MAP — the only place in this project where a GPIO number appears.
//
//    Panel pin  | Waveshare cable | ESP32-C3 pin | Direction
//    -----------+-----------------+--------------+------------
//    VCC        | gray            | 3V3          | 3.3 V ONLY, never 5 V
//    GND        | brown           | GND          |
//    DIN (MOSI) | blue            | GPIO7        | C3 → panel
//    CLK (SCK)  | yellow          | GPIO6        | C3 → panel
//    CS         | orange          | GPIO10       | C3 → panel (active low)
//    DC         | green           | GPIO5        | C3 → panel
//    RST        | white           | GPIO3        | C3 → panel (active low)
//    BUSY       | purple          | GPIO4        | panel → C3 (input)
//
//  MISO is unused — e-paper is write-only. Leave it unconnected.
//  Never move these onto GPIO2 / 8 / 9 (strapping pins), GPIO18 / 19 (native
//  USB D-/D+) or GPIO20 / 21 (UART0).
// ─────────────────────────────────────────────────────────────────────────────
static const int8_t PIN_EPD_MOSI = 7;
static const int8_t PIN_EPD_SCK  = 6;
static const int8_t PIN_EPD_CS   = 10;
static const int8_t PIN_EPD_DC   = 5;
static const int8_t PIN_EPD_RST  = 3;
static const int8_t PIN_EPD_BUSY = 4;
static const int8_t PIN_EPD_MISO = -1;   // not connected

// ─────────────────────────────────────────────────────────────────────────────
//  PANEL CLASS — change this one macro if your panel revision differs.
//    Waveshare 2.13" V4  → GxEPD2_213_BN   (DEPG0213BN, SSD1680)  ← configured
//    Waveshare 2.13" V3  → GxEPD2_213_B74  (GDEM0213B74, SSD1680)
//    Waveshare 2.13" V2  → GxEPD2_213_B72  (GDEH0213B72)
//    Waveshare 2.13" V1  → GxEPD2_213      (GDE0213B1)
//    flexible 2.13"      → GxEPD2_213_flex
//  A wrong class here is the #1 cause of a permanently blank panel: the panel
//  ACKs nothing, so there is no error — you just get white.
// ─────────────────────────────────────────────────────────────────────────────
// Stamped by scripts/build_id.py. The fallback only applies if the pre-script
// did not run, which would itself be worth knowing.
#ifndef BUILD_ID
#define BUILD_ID "unstamped"
#endif

#define EPD_PANEL_CLASS GxEPD2_213_BN

GxEPD2_BW<EPD_PANEL_CLASS, EPD_PANEL_CLASS::HEIGHT> display(
    EPD_PANEL_CLASS(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// Landscape: the panel's native portrait 122x250 becomes 250x122 at rotation 1.
// Use WIDTH_VISIBLE, not WIDTH: SSD1680 panels have 128 columns of controller RAM
// but only 122 of them reach the glass. GxEPD2_BW builds its Adafruit_GFX space
// from WIDTH_VISIBLE too, so anything drawn past it is silently clipped.
static const uint16_t PANEL_W = EPD_PANEL_CLASS::HEIGHT;
static const uint16_t PANEL_H = EPD_PANEL_CLASS::WIDTH_VISIBLE;

// Which way up. 1 is landscape with the ribbon cable on the right; 3 is the
// same layout turned 180 degrees, cable on the left. (0 and 2 are portrait and
// would need PANEL_W/PANEL_H swapped, so stick to 1 or 3.)
//
// Nothing else in the firmware cares: the canvas is always 250x122 in reading
// order and GxEPD2 applies the rotation when the frame is blitted, so the web
// preview and the stored image are identical either way. Safe on this panel
// because GxEPD2_BW hands Adafruit_GFX WIDTH_VISIBLE (122) rather than the
// controller's 128 RAM columns, so the 180-degree mirror lands on the glass
// and not 6 px off it.
static const uint8_t PANEL_ROTATION = 3;

// ── Tunables ────────────────────────────────────────────────────────────────
static const uint8_t  FULL_REFRESH_EVERY = 10;   // partial updates between full ones
// How long to leave the panel powered after a draw, in ms.
//
// GxEPD2's partial refresh only works while the panel is STILL POWERED from the
// previous draw. _Update_Part() writes the partial waveform (0x32) and then
// calls _PowerOn(), which issues 0x22/0xf8 — and bit 0x10 of that reloads the
// OTP waveform straight over the partial one. Normally _PowerOn() is a no-op
// because power was never dropped; if we powered off, it runs, and the partial
// refresh executes with the wrong LUT: grey under-driven ink, the old image not
// cleared, sometimes no visible change at all.
//
// 0 (the default) means power the panel down the instant a draw finishes, so it
// is never left under drive voltage. The unavoidable consequence is that every
// update is a full refresh: ~2.4 s and it flashes, but it is always clean.
//
// Set this to e.g. 30000 to keep the charge pump on for 30 s after a draw, so
// edits made in quick succession get fast (~630 ms) partial refreshes. That
// trades the panel sitting powered during the window for the speed.
static const uint32_t PANEL_KEEP_POWERED_MS    = 0;
static const uint32_t PANEL_HIBERNATE_AFTER_MS = 300000;   // 5 min, deep sleep
static const uint16_t MAX_TEXT_LEN       = 400;  // chars accepted in TEXT mode
static const uint8_t  QR_MAX_VERSION     = 10;   // 57x57 modules, the largest that fits
// Quiet zone -- the white border a scanner needs -- in modules. ISO 18004 asks
// for 4, but that is reserved BEFORE the module size is chosen and the division
// rounds down, which is expensive: 33 modules + 4 + 4 = 41 into 122 px gives
// 2.97 -> 2 px per module, and the border that then lands on the glass is 14
// modules. Reserving 2 lets the same code take 3 px per module and still leaves
// 3.7. Checked against a simulated phone camera: 4, 3 and 2 modules all decode
// alike, and only at 1 does detection start to fail. A dark surround behind the
// panel is no worse than its white bezel -- it gives the detector a firmer edge
// -- so this does not depend on how the label is mounted.
static const uint8_t  QR_QUIET_MIN       = 2;
// The border may drop to this, but ONLY when doing so lifts a code off
// QR_MIN_SCALE -- see planQr(). Applied unconditionally it makes things worse:
// with 4 px modules the scanner has no trouble resolving cells and what limits
// it is finding the code's edge, so a 12-byte payload measured 92 cm at border
// 2 and 74 cm at border 1. With 2 px modules the opposite holds -- the scanner
// is struggling to resolve cells at all, and a whole extra pixel per module
// dwarfs the border: an 82-byte payload went 41 cm to 57 cm. Below 3 px per
// module the 1-vs-2 module border barely registers either way.
static const uint8_t  QR_QUIET_RESCUE    = 1;
static const uint8_t  QR_MIN_SCALE       = 2;    // px per module; 1 is unscannable here
// Maximum deviation from centre, per axis, when placing a QR code. Spreads
// panel wear without making the layout look accidental -- see renderQrToCanvas.
static const uint8_t  QR_JITTER_PX       = 8;
static const uint8_t  QR_CAPTION_GAP     = 8;    // px between a code and its caption
static const uint16_t MAX_CAPTION_LEN    = 60;
// Recently displayed items, so the handful of things a label cycles between are
// one tap away instead of retyped on a phone. TEXT and QR only: a browser-
// rendered frame is 3904 bytes and the whole NVS partition is 20 KB, so six of
// them would not fit and the two bitmap tabs are excluded.
static const uint8_t  RECENTS_MAX        = 6;
static const uint16_t RECENTS_MAX_LEN    = 1800;   // total, in NVS
static const char     REC_SEP            = '\x1e';  // between records
static const char     FLD_SEP            = '\x1f';  // between fields
// Thumbnails for the recent list. A drawing has no text to show, so the list
// needs a picture to be usable at all. 4x reduction, and a cell is black if ANY
// source pixel under it is — averaging would thin a one-pixel stroke to nothing.
static const uint8_t  THUMB_DIV          = 4;
static const char*    AP_SSID            = "eink-setup";
// 2.4 GHz channel for the setup AP. Channel 1 is the arduino-esp32 default and
// is usually the most contended; a beacon from a bare-PCB-antenna board can be
// lost behind a strong neighbour there. Pick whatever your SCAN output shows as
// quiet (1, 6 and 11 are the non-overlapping choices).
static const uint8_t  AP_CHANNEL         = 11;
static const char*    HOSTNAME           = "eink";   // → http://eink.local
static const uint8_t  MAX_LINES          = 16;
// Transmit power in dBm. The ESP32-C3 SuperMini defaults to 20 dBm, but this
// board does not radiate cleanly at full power: measured from 30 cm, its SoftAP
// beacon is undetectable at 20 dBm on channels 1-6 yet reads signal 95 at
// 11 dBm on the same channel. The current spike at maximum power is more than
// the board's supply and decoupling can hold up. 11 dBm is far more than enough
// for a desk label and makes the link reliable across the whole band.
static const int8_t   WIFI_TX_DBM        = 11;

// ricmoo/QRCode buffer for the largest version we allow: ((4v+17)^2 + 7) / 8
#define QR_BUF_BYTES  ((((4 * QR_MAX_VERSION + 17) * (4 * QR_MAX_VERSION + 17)) + 7) / 8)

// ── Off-screen framebuffer ──────────────────────────────────────────────────
// Everything is rendered here first. The panel gets a copy, and the web UI gets
// the very same bits as its preview — so the preview is pixel-exact, not a
// browser-side approximation.
static GFXcanvas1 canvas(PANEL_W, PANEL_H);
// Dry-run target for /api/preview. Rendering a preview must not disturb `canvas`,
// which is the record of what is actually on the glass and what /api/status
// reports, or the page would show an unprinted preview as if it were the panel.
static GFXcanvas1 scratch(PANEL_W, PANEL_H);
static const uint16_t CANVAS_STRIDE = (PANEL_W + 7) / 8;
static const uint32_t CANVAS_BYTES  = (uint32_t)CANVAS_STRIDE * PANEL_H;
static const uint16_t THUMB_W       = (PANEL_W + THUMB_DIV - 1) / THUMB_DIV;   // 63
static const uint16_t THUMB_H       = (PANEL_H + THUMB_DIV - 1) / THUMB_DIV;   // 31
static const uint16_t THUMB_STRIDE  = (THUMB_W + 7) / 8;
static const uint16_t THUMB_BYTES   = THUMB_STRIDE * THUMB_H;                  // 248

// ── Content state ───────────────────────────────────────────────────────────
enum Mode : uint8_t { MODE_NONE = 0, MODE_TEXT = 1, MODE_QR = 2, MODE_IMAGE = 3 };

// A 1-bit frame rasterised by the browser: emoji, any font the phone has, an
// uploaded picture, all resolved to the panel's own format before it arrives.
// Same packing as GFXcanvas1 — MSB first, 1 = black ink.
static uint8_t  g_image[CANVAS_BYTES];
static bool     g_haveImage = false;

static Mode     g_mode    = MODE_NONE;
static String   g_text    = "";
static String   g_caption = "";         // QR mode only: label drawn beside the code

// Firmware upload password. Every other endpoint can only change what the panel
// shows; this one replaces the running code, so it is the one thing on this
// server worth a lock. Empty means the endpoint is off entirely — a default
// password would be worse than none, since nobody would change it.
static String   g_recents = "";         // REC_SEP-separated, see pushRecent()
// The frames behind bitmap recents live in the flash filesystem, not in NVS: one
// is 3904 bytes against a 20 KB NVS partition, while the filesystem partition is
// 1.4 MB and was sitting empty. Six frames is 23 KB of it.
static bool     g_fsReady = false;
static uint8_t  g_frameBuf[CANVAS_BYTES];   // scratch for reading one back
static String   g_otaPw   = "";
static bool     g_otaAuthed = false;
static bool     g_otaFailed = false;
static uint8_t  g_size    = 2;          // 1 small, 2 medium, 3 large
static uint32_t g_updates = 0;

static bool     g_drawnSinceBoot   = false;
static uint8_t  g_partialsSinceFull = 0;
static uint32_t g_lastPushedHash   = 0;      // of the last image actually sent
static bool     g_havePushed        = false;
static bool     g_lastPushChanged   = false;  // did the most recent request redraw?
static bool     g_panelPowered      = false;  // charge pump on: partial refresh is safe
static bool     g_panelAsleep       = true;   // controller deep-sleeping
static uint32_t g_lastDrawMs        = 0;

// ── Network state ───────────────────────────────────────────────────────────
static Preferences prefs;
static WebServer   server(80);
static DNSServer   dns;

static String   g_ssid, g_pass;
static bool     g_haveCreds    = false;
// Set once these exact credentials have completed a connection, and cleared
// whenever they change. A credential that has worked is never "wrong", so a
// later auth-shaped failure has to be interference rather than a typo.
//
// Every write to "ssid"/"pass" must clear this alongside, or the failure mode
// runs in reverse: a genuinely mistyped password would inherit the old one's
// good standing and never raise the setup portal. Three places do it —
// handleWifiSave(), and the FORGET and WIFI: serial commands.
static bool     g_credsProven  = false;
static bool     g_apActive     = false;
static bool     g_wasConnected = false;
static uint8_t  g_failures     = 0;
static uint32_t g_lastAttempt  = 0;
static uint32_t g_backoff      = 5000;      // ms, doubles up to 5 min
static const uint32_t BACKOFF_MAX = 300000;
static uint32_t g_rebootAt     = 0;         // 0 = no reboot pending
static uint8_t  g_lastReason   = 0;         // last STA disconnect reason
static uint8_t  g_apChannel    = AP_CHANNEL; // runtime-adjustable via APCH:<n>
static bool     g_staDisabled  = false;     // stop retrying: credentials rejected

// ============================================================================
//  Small helpers
// ============================================================================

static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { char b[7]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

static String base64(const uint8_t* data, size_t len) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String o;
  o.reserve(((len + 2) / 3) * 4 + 1);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
    o += T[(v >> 6) & 63];  o += T[v & 63];
    i += 3;
  }
  if (i < len) {
    uint32_t v = (uint32_t)data[i] << 16;
    bool two = (i + 1 < len);
    if (two) v |= (uint32_t)data[i + 1] << 8;
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    o += two ? T[(v >> 6) & 63] : '=';
    o += '=';
  }
  return o;
}

static int8_t b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;                       // '=' and any whitespace land here
}

// Strict: decodes exactly outLen bytes or fails. Ignores characters outside the
// alphabet so line breaks in transit are harmless.
static bool base64Decode(const String& in, uint8_t* out, size_t outLen) {
  uint32_t acc = 0;
  uint8_t bits = 0;
  size_t written = 0;
  for (size_t i = 0; i < in.length(); i++) {
    int8_t v = b64val(in[i]);
    if (v < 0) continue;
    acc = (acc << 6) | (uint8_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written >= outLen) return false;
      out[written++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return written == outLen;
}

// The built-in GFX font only covers ASCII 32..126. Anything else would draw as
// garbage glyphs, so fold it to '?' rather than lying about what fits.
static String sanitizeForFont(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '\n') o += '\n';
    else if (c >= 32 && c <= 126) o += (char)c;
    else if (c >= 0x80) { o += '?'; while (i + 1 < s.length() && ((uint8_t)s[i + 1] & 0xC0) == 0x80) i++; }
    else o += ' ';
  }
  return o;
}

// ============================================================================
//  Rendering — text
// ============================================================================

static bool emitLine(String* out, uint8_t& n, uint8_t maxRows, const String& l, bool& truncated) {
  if (n >= maxRows) { truncated = true; return false; }
  out[n++] = l;
  return true;
}

// Greedy word wrap on a fixed character grid (the built-in font is monospace).
// Words longer than a line are hard-broken. Returns the number of lines used.
static uint8_t wrapText(const String& s, uint16_t cols, uint8_t maxRows,
                        String* out, bool& truncated) {
  uint8_t n = 0;
  truncated = false;
  String cur, word;
  size_t len = s.length();

  for (size_t i = 0; i <= len; i++) {
    char c = (i < len) ? s[i] : '\0';
    if (c == ' ' || c == '\n' || c == '\0') {
      if (word.length()) {
        while (word.length() > cols) {
          if (cur.length() && !emitLine(out, n, maxRows, cur, truncated)) return n;
          cur = "";
          if (!emitLine(out, n, maxRows, word.substring(0, cols), truncated)) return n;
          word.remove(0, cols);
        }
        if (!cur.length())                                   cur = word;
        else if (cur.length() + 1 + word.length() <= cols) { cur += ' '; cur += word; }
        else { if (!emitLine(out, n, maxRows, cur, truncated)) return n; cur = word; }
        word = "";
      }
      if (c == '\n') {
        if (!emitLine(out, n, maxRows, cur, truncated)) return n;
        cur = "";
      }
    } else {
      word += c;
    }
  }
  if (cur.length() && !emitLine(out, n, maxRows, cur, truncated)) return n;
  return n;
}

// Word-wraps into a rectangle and centres the block vertically. `fixedSize` of
// 0 means auto-fit: try 3, then 2, then 1, and take the first that does not
// have to truncate. Returns the size actually used.
static uint8_t drawTextInRect(GFXcanvas1& c, const String& raw,
                              int16_t rx, int16_t ry, uint16_t rw, uint16_t rh,
                              uint8_t fixedSize) {
  const String s = sanitizeForFont(raw);
  String lines[MAX_LINES];
  uint8_t n = 0, size = 1, pitch = 10;
  uint16_t cols = 4;
  bool truncated = false;

  for (uint8_t trial = (fixedSize ? fixedSize : 3); trial >= 1; trial--) {
    const uint16_t cw = 6 * trial;
    const uint16_t p  = 8 * trial + (trial >= 2 ? 3 : 2);   // a little leading
    uint16_t tcols = rw / cw;
    uint8_t  trows = (uint8_t)(rh / p);
    if (trows > MAX_LINES) trows = MAX_LINES;
    if (tcols < 4) tcols = 4;
    if (trows < 1) trows = 1;

    bool tr = false;
    const uint8_t k = wrapText(s, tcols, trows, lines, tr);
    size = trial; pitch = (uint8_t)p; cols = tcols; n = k; truncated = tr;
    if (!tr || fixedSize || trial == 1) break;
  }

  if (truncated && n > 0 && cols >= 4) {          // "…" as three dots: the
    String& last = lines[n - 1];                  // built-in font has no U+2026
    if (last.length() > (uint16_t)(cols - 3)) last = last.substring(0, cols - 3);
    last += "...";
  }

  c.setFont(NULL);
  c.setTextSize(size);
  c.setTextColor(1);
  c.setTextWrap(false);

  const uint16_t cw = 6 * size;
  int16_t y0 = ry + ((int16_t)rh - (int16_t)(n * pitch)) / 2;
  if (y0 < ry) y0 = ry;
  for (uint8_t i = 0; i < n; i++) {
    int16_t x = rx + ((int16_t)rw - (int16_t)(lines[i].length() * cw)) / 2;
    if (x < rx) x = rx;
    c.setCursor(x, y0 + i * pitch);
    c.print(lines[i]);
  }
  return size;
}

static void renderTextToCanvas(GFXcanvas1& c, const String& raw, uint8_t size) {
  c.fillScreen(0);
  drawTextInRect(c, raw, 0, 0, PANEL_W, PANEL_H, size);
}

// ============================================================================
//  Rendering — QR
// ============================================================================

// Byte-mode capacity in bytes, [ecc][version-1], versions 1..QR_MAX_VERSION.
//
// ricmoo/QRCode does NOT range-check the payload against the version it is
// handed: qrcode_initBytes() only fails when it cannot choose an encoding mode,
// never on overflow. Feeding it too much data yields a structurally invalid,
// unscannable code with no error at all, so the fit test has to live here.
// Byte mode is the conservative choice — a numeric or alphanumeric payload may
// fit a smaller version than this table admits, which only costs module size.
static const uint16_t QR_BYTE_CAPACITY[4][QR_MAX_VERSION] = {
  { 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 },  // ECC_LOW
  { 14, 26, 42, 62,  84, 106, 122, 152, 180, 213 },  // ECC_MEDIUM
  { 11, 20, 32, 46,  60,  74,  86, 108, 130, 151 },  // ECC_QUARTILE
  {  7, 14, 24, 34,  44,  58,  64,  84,  98, 119 },  // ECC_HIGH
};
static const char* const QR_ECC_NAME[4] = { "Low", "Medium", "Quartile", "High" };

struct QrPlan {
  uint8_t  version;    // 1..QR_MAX_VERSION
  uint8_t  ecc;        // ECC_LOW..ECC_HIGH
  uint8_t  scale;      // px per module — what actually decides readability
  uint8_t  quiet;      // border reserved, in modules
  uint16_t modules;    // 4 * version + 17
  uint16_t side;       // modules * scale, in px
};
static QrPlan g_qrPlan      = {0, 0, 0, 0, 0, 0};   // of the frame on the glass
static bool   g_qrPlanValid = false;

// Chooses the (version, error-correction) pair giving the LARGEST module,
// breaking ties toward stronger error correction.
//
// Module size is what decides whether a phone can read the code at all; error
// correction only starts to matter once part of the code is unreadable, which
// on a clean panel behind glass is the rarer failure. Both are quantised — a
// module must be a whole number of pixels — so scale is a step function of the
// module count, and a LARGER grid at STRONGER correction frequently lands on
// the same step. When it does the stronger correction is free, because the
// space it occupies was being discarded to rounding anyway.
//
// Fixing ECC at MEDIUM (as this used to) throws that away: a 60-byte payload
// took version 4 at 2 px per module, 66 px on a 122 px panel, when version 4 at
// 3 px per module — 99 px — was available the whole time.
static bool planQrAt(uint16_t len, uint16_t availW, uint16_t availH,
                     uint8_t quiet, QrPlan& out) {
  bool found = false;
  for (int8_t e = ECC_HIGH; e >= ECC_LOW; e--) {          // strongest first
    uint8_t v = 0;
    for (uint8_t i = 1; i <= QR_MAX_VERSION; i++) {
      if (len <= QR_BYTE_CAPACITY[e][i - 1]) { v = i; break; }
    }
    if (!v) continue;                                     // too long for this level
    const uint16_t modules = 4u * v + 17u;
    const uint16_t total   = modules + 2u * quiet;
    const uint16_t scale   = min(availW / total, availH / total);
    if (scale < QR_MIN_SCALE) continue;
    if (!found || scale > out.scale) {
      out.version = v;
      out.ecc     = (uint8_t)e;
      out.scale   = (uint8_t)scale;
      out.quiet   = quiet;
      out.modules = modules;
      out.side    = modules * scale;
      found = true;
    }
  }
  return found;
}

// Normally reserves QR_QUIET_MIN modules of border. If the best that yields is
// still the bare minimum module size, try again at QR_QUIET_RESCUE and keep the
// result only if it actually buys a bigger module — trading border for module
// size is worth it exactly there, and nowhere else. Rescues payloads of roughly
// 79-106 bytes, with no length made worse.
static bool planQr(uint16_t len, uint16_t availW, uint16_t availH, QrPlan& out) {
  if (!planQrAt(len, availW, availH, QR_QUIET_MIN, out)) return false;
  if (out.scale == QR_MIN_SCALE) {
    QrPlan rescued;
    if (planQrAt(len, availW, availH, QR_QUIET_RESCUE, rescued) &&
        rescued.scale > out.scale) {
      out = rescued;
    }
  }
  return true;
}

static uint32_t strHash(const String& s) {                // FNV-1a
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

// Draws the code into the [areaX, areaX+areaW) column, full panel height. The
// caller has already cleared the canvas; the quiet zone is just the margin left
// around the code.
static bool renderQrToCanvas(GFXcanvas1& c, const String& payload,
                             int16_t areaX, uint16_t areaW,
                             QrPlan& plan, String& err) {
  static uint8_t qrBuf[QR_BUF_BYTES];
  QRCode qr;
  const uint16_t len = payload.length();

  if (!planQr(len, areaW, PANEL_H, plan)) {
    const uint16_t cap = QR_BYTE_CAPACITY[ECC_LOW][QR_MAX_VERSION - 1];
    err = (len > cap)
        ? String("QR payload too long: ") + len + " bytes, limit " + cap
        : String("QR payload too long to stay scannable here: ") + len
          + " bytes needs more modules than " + areaW + "x" + PANEL_H
          + " px can show at " + QR_MIN_SCALE + " px each";
    return false;
  }
  if (qrcode_initText(&qr, qrBuf, plan.version, plan.ecc, payload.c_str()) != 0) {
    err = "QR encoding failed";
    return false;
  }

  // Anti-retention jitter. E-paper does not burn in the way OLED does — there
  // is no emissive material to age — but a pattern held in one spot for a long
  // time can leave a faint residual image as the pigment settles, and repeated
  // switching wears one area of the film harder than the rest. Nudging the code
  // a few pixels spreads both across the glass.
  //
  // The offset comes from the payload, not from a random number generator, so
  // the same content always lands in the same place. Were it to move on every
  // draw, the unchanged-image check could never fire and every repeat print
  // would cost a full 2.4 s refresh. It is also bounded to QR_JITTER_PX rather
  // than filling the available slack: a code wandering 70 px across the panel
  // reads as a bug, not as care.
  // Jitter is bounded by the COMFORTABLE border (QR_QUIET_MIN), not by whatever
  // the plan settled for. When the rescue border is in play the two collide --
  // there is no room for 2 modules on both sides at once -- and the axis is
  // simply centred instead. Letting jitter spend the rescued border would give
  // back the range the bigger module just bought: measured on the panel, an
  // 85-byte code pushed to a 1.0-module margin read to 34 cm against a dark
  // surround, where the centred 1.7-module version reads to 51 cm.
  const uint32_t h  = strHash(payload);
  const int16_t safe = (int16_t)QR_QUIET_MIN * plan.scale;
  const int16_t cx0 = areaX + ((int16_t)areaW   - (int16_t)plan.side) / 2;
  const int16_t cy0 =         ((int16_t)PANEL_H - (int16_t)plan.side) / 2;

  const int16_t xLo = areaX + safe, xHi = areaX + (int16_t)areaW - plan.side - safe;
  const int16_t yLo = safe,         yHi = (int16_t)PANEL_H - plan.side - safe;

  int16_t ox = cx0, oy = cy0;
  if (xLo <= xHi) {
    ox += (int16_t)(h % (2u * QR_JITTER_PX + 1u)) - QR_JITTER_PX;
    if (ox < xLo) ox = xLo;  if (ox > xHi) ox = xHi;
  }
  if (yLo <= yHi) {
    oy += (int16_t)((h >> 16) % (2u * QR_JITTER_PX + 1u)) - QR_JITTER_PX;
    if (oy < yLo) oy = yLo;  if (oy > yHi) oy = yHi;
  }

  for (uint16_t y = 0; y < plan.modules; y++) {
    for (uint16_t x = 0; x < plan.modules; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        c.fillRect(ox + x * plan.scale, oy + y * plan.scale, plan.scale, plan.scale, 1);
      }
    }
  }
  Serial.printf("[qr] v%u %s %ux%u modules, %u px/module, %u px, border %u, at %d,%d%s\n",
                plan.version, QR_ECC_NAME[plan.ecc], plan.modules, plan.modules,
                plan.scale, plan.side, plan.quiet, ox, oy,
                plan.scale <= QR_MIN_SCALE ? "  [at the readability limit]" : "");
  return true;
}

// ============================================================================
//  Panel I/O
// ============================================================================

static void displayBegin() {
  // Claim the SPI bus on our pins BEFORE GxEPD2's init(): SPIClass::begin()
  // returns early if the bus is already up, so GxEPD2's own argument-less call
  // becomes a no-op and the panel talks over the wiring in the pin map above
  // instead of the C3's defaults.
  //
  // MISO is -1 because e-paper is write-only. arduino-esp32 logs
  //   [E] spiAttachMISO(): SPI Does not have default pins on ESP32C3!
  // for that: it means "no default MISO to fall back to", not a fault. One such
  // line at boot is expected and harmless.
  SPI.begin(PIN_EPD_SCK, PIN_EPD_MISO, PIN_EPD_MOSI, PIN_EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(PANEL_ROTATION);        // landscape, 250 x 122
}

// Copies the canvas to the panel.
//
// Partial refresh is only attempted when the panel is still powered from the
// previous draw (see PANEL_KEEP_POWERED_MS). Otherwise, and on the first draw
// after boot, on an explicit clear, and every FULL_REFRESH_EVERY partials to
// sweep up accumulated ghosting, we do a full refresh.
// FNV-1a over the framebuffer. Only used to notice that a request would redraw
// the identical image, which on this panel costs a pointless 2.4 s flash.
static uint32_t canvasHash() {
  const uint8_t* b = canvas.getBuffer();
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < CANVAS_BYTES; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

static void pushCanvas(bool forceFull = false) {
  // E-paper holds its image with no power, so redrawing what is already on the
  // glass buys nothing and just makes the panel flash. Skip it.
  const uint32_t hash = canvasHash();
  if (g_havePushed && hash == g_lastPushedHash) {
    g_lastPushChanged = false;
    Serial.println(F("[epd] image unchanged — refresh skipped"));
    return;
  }

  const bool full = forceFull || !g_drawnSinceBoot || !g_panelPowered ||
                    (g_partialsSinceFull >= FULL_REFRESH_EVERY);

  if (full) display.setFullWindow();
  else      display.setPartialWindow(0, 0, PANEL_W, PANEL_H);

  const uint32_t t0 = millis();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H, GxEPD_BLACK, GxEPD_WHITE);
  } while (display.nextPage());
  const uint32_t ms = millis() - t0;

  g_panelAsleep = false;
  if (PANEL_KEEP_POWERED_MS == 0) {
    display.powerOff();          // never left under drive voltage
    g_panelPowered = false;
  } else {
    g_panelPowered = true;       // kept alive so the next update can be partial
  }
  g_lastDrawMs = millis();

  if (full) g_partialsSinceFull = 0;
  else      g_partialsSinceFull++;
  g_drawnSinceBoot = true;
  g_updates++;
  g_lastPushedHash = hash;
  g_havePushed = true;
  g_lastPushChanged = true;

  Serial.printf("[epd] %s refresh in %lu ms (%u partial since full)\n",
                full ? "full" : "partial", (unsigned long)ms, g_partialsSinceFull);
}

// Drops the panel out of its powered window, then eventually into deep sleep.
static void panelTick() {
  if (!g_drawnSinceBoot) return;
  const uint32_t idle = millis() - g_lastDrawMs;

  if (g_panelPowered && idle >= PANEL_KEEP_POWERED_MS) {
    display.powerOff();
    g_panelPowered = false;
    Serial.println(F("[epd] powered window over — panel off"));
  }
  if (!g_panelAsleep && !g_panelPowered && idle >= PANEL_HIBERNATE_AFTER_MS) {
    display.hibernate();
    g_panelAsleep = true;
    Serial.println(F("[epd] idle — panel hibernated"));
  }
}

// ============================================================================
//  Content: apply / persist / restore
// ============================================================================

static void saveContent() {
  prefs.begin("eink", false);
  prefs.putUChar("mode", (uint8_t)g_mode);
  prefs.putUChar("size", g_size);
  prefs.putString("text", g_text);
  prefs.putString("cap", g_caption);
  prefs.putString("recents", g_recents);
  if (g_mode == MODE_IMAGE && g_haveImage) {
    size_t n = prefs.putBytes("image", g_image, CANVAS_BYTES);
    if (n != CANVAS_BYTES) Serial.printf("[nvs] image save short: %u/%u bytes\n",
                                         (unsigned)n, (unsigned)CANVAS_BYTES);
  }
  prefs.end();
}

static void loadContent() {
  prefs.begin("eink", true);
  g_mode = (Mode)prefs.getUChar("mode", MODE_NONE);
  g_size = prefs.getUChar("size", 2);
  g_text    = prefs.getString("text", "");
  g_caption = prefs.getString("cap", "");
  g_otaPw   = prefs.getString("otapw", "");
  g_recents = prefs.getString("recents", "");
  if (g_mode == MODE_IMAGE) {
    g_haveImage = (prefs.getBytesLength("image") == CANVAS_BYTES) &&
                  (prefs.getBytes("image", g_image, CANVAS_BYTES) == CANVAS_BYTES);
    if (!g_haveImage) Serial.println(F("[nvs] stored image missing or wrong size"));
  }
  prefs.end();
  if (g_size < 1 || g_size > 3) g_size = 2;
  if (g_mode == MODE_IMAGE && !g_haveImage) g_mode = MODE_NONE;
  if (g_mode != MODE_TEXT && g_mode != MODE_QR && g_mode != MODE_IMAGE) g_mode = MODE_NONE;
}

// Renders into the canvas only; err is filled on rejection. `planOut`, when
// given, receives how the QR code was sized so the caller can report it.
static bool renderToCanvas(GFXcanvas1& c, Mode mode, const String& text,
                           const String& caption, uint8_t size, String& err,
                           QrPlan* planOut = nullptr) {
  if (mode == MODE_NONE) { c.fillScreen(0); return true; }

  if (mode == MODE_IMAGE) {
    if (!g_haveImage) { err = "no image stored"; return false; }
    memcpy(c.getBuffer(), g_image, CANVAS_BYTES);
    return true;
  }

  if (text.length() == 0) { err = "nothing to display"; return false; }

  if (mode == MODE_TEXT) {
    if (text.length() > MAX_TEXT_LEN) {
      err = String("text too long: ") + text.length() + " chars, limit " + MAX_TEXT_LEN;
      return false;
    }
    if (size < 1 || size > 3) { err = "font size must be 1, 2 or 3"; return false; }
    renderTextToCanvas(c, text, size);
    return true;
  }

  // QR. A square code on a 250x122 panel can never be taller than 122 px, so
  // more than half the glass is unusable by construction. With a caption the
  // code takes a 122 px square on the left and the text gets the strip that was
  // going to be blank anyway — which is what makes it a label rather than a
  // bare code.
  if (caption.length() > MAX_CAPTION_LEN) {
    err = String("caption too long: ") + caption.length() + " chars, limit " + MAX_CAPTION_LEN;
    return false;
  }
  const bool hasCap    = caption.length() > 0;
  const uint16_t qrW   = hasCap ? PANEL_H : PANEL_W;
  QrPlan plan = {0, 0, 0, 0, 0, 0};
  c.fillScreen(0);
  if (!renderQrToCanvas(c, text, 0, qrW, plan, err)) return false;
  if (hasCap) {
    const int16_t capX = (int16_t)qrW + QR_CAPTION_GAP;
    drawTextInRect(c, caption, capX, 0, PANEL_W - capX - 4, PANEL_H, 0);
  }
  if (planOut) *planOut = plan;
  return true;
}

static void stripSeps(String& s) {
  s.replace(String(REC_SEP), "");
  s.replace(String(FLD_SEP), "");
}

// A record is  mode / text / caption / size / frame-id, FLD_SEP between fields.
// The frame id is an FNV-1a hash of the frame itself, which does double duty:
// it names the file, and it makes an identical redraw produce an identical
// record, so the existing de-duplication catches it with no extra work.
static String framePath(uint32_t id) {
  char b[20];
  snprintf(b, sizeof b, "/r%08x.bin", (unsigned)id);
  return String(b);
}

static uint32_t frameHash(const uint8_t* f) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < CANVAS_BYTES; i++) { h ^= f[i]; h *= 16777619u; }
  return h ? h : 1u;                    // 0 means "no frame"
}

static uint32_t recField(const String& rec, uint8_t idx) {   // numeric fields only
  int start = 0;
  for (uint8_t i = 0; i < idx; i++) {
    start = rec.indexOf(FLD_SEP, start);
    if (start < 0) return 0;
    start++;
  }
  int end = rec.indexOf(FLD_SEP, start);
  if (end < 0) end = rec.length();
  return (uint32_t)strtoul(rec.substring(start, end).c_str(), nullptr, 10);
}

// Deletes any frame file no record points at. Covers eviction, de-duplication
// and a reset midway through a write, in one sweep, rather than trying to keep
// the two in step at every edit.
static void gcFrames() {
  if (!g_fsReady) return;
  String keep = ",";
  int start = 0;
  while (start < (int)g_recents.length()) {
    int end = g_recents.indexOf(REC_SEP, start);
    if (end < 0) end = g_recents.length();
    const uint32_t id = recField(g_recents.substring(start, end), 4);
    if (id) { keep += framePath(id); keep += ','; }
    start = end + 1;
  }
  File dir = LittleFS.open("/");
  if (!dir) return;
  String doomed;                        // collected first: deleting mid-walk is unsafe
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    String n = e.name();
    e.close();
    if (!n.startsWith("/")) n = "/" + n;
    if (!n.startsWith("/r") || !n.endsWith(".bin")) continue;
    if (keep.indexOf("," + n + ",") < 0) { doomed += n; doomed += REC_SEP; }
  }
  dir.close();
  start = 0;
  while (start < (int)doomed.length()) {
    int end = doomed.indexOf(REC_SEP, start);
    if (end < 0) end = doomed.length();
    const String n = doomed.substring(start, end);
    if (n.length()) { LittleFS.remove(n); Serial.printf("[fs] dropped %s\n", n.c_str()); }
    start = end + 1;
  }
}

// Newest first, de-duplicated, oldest dropped on either count or total length.
// `frame` non-null stores the bitmap alongside; without a working filesystem the
// entry is skipped rather than remembered as an empty row.
static void pushRecent(const char* mode, const String& text, const String& cap,
                       uint8_t size, const uint8_t* frame) {
  String t = text, c = cap;
  stripSeps(t); stripSeps(c);

  uint32_t fid = 0;
  if (frame) {
    if (!g_fsReady) return;
    fid = frameHash(frame);
    const String path = framePath(fid);
    if (!LittleFS.exists(path)) {                 // same picture, same file
      File f = LittleFS.open(path, "w");
      if (!f) { Serial.println(F("[fs] frame open failed")); return; }
      const size_t n = f.write(frame, CANVAS_BYTES);
      f.close();
      if (n != CANVAS_BYTES) {
        Serial.printf("[fs] short write %u/%u\n", (unsigned)n, (unsigned)CANVAS_BYTES);
        LittleFS.remove(path);
        return;
      }
    }
  }

  String rec = String(mode);
  rec += FLD_SEP; rec += t;
  rec += FLD_SEP; rec += c;
  rec += FLD_SEP; rec += size;
  rec += FLD_SEP; rec += fid;
  if (rec.length() > RECENTS_MAX_LEN) return;

  String out = rec;
  uint8_t kept = 1;
  int start = 0;
  while (start < (int)g_recents.length() && kept < RECENTS_MAX) {
    int end = g_recents.indexOf(REC_SEP, start);
    if (end < 0) end = g_recents.length();
    const String prev = g_recents.substring(start, end);
    start = end + 1;
    if (prev.length() == 0 || prev == rec) continue;
    if (out.length() + 1 + prev.length() > RECENTS_MAX_LEN) break;
    out += REC_SEP; out += prev; kept++;
  }
  g_recents = out;
  gcFrames();
}

static void makeThumb(const uint8_t* src, uint8_t* out) {
  memset(out, 0, THUMB_BYTES);
  for (uint16_t ty = 0; ty < THUMB_H; ty++) {
    for (uint16_t tx = 0; tx < THUMB_W; tx++) {
      bool ink = false;
      for (uint8_t dy = 0; dy < THUMB_DIV && !ink; dy++) {
        const uint16_t y = ty * THUMB_DIV + dy;
        if (y >= PANEL_H) break;
        for (uint8_t dx = 0; dx < THUMB_DIV; dx++) {
          const uint16_t x = tx * THUMB_DIV + dx;
          if (x >= PANEL_W) break;
          if (src[y * CANVAS_STRIDE + (x >> 3)] & (0x80 >> (x & 7))) { ink = true; break; }
        }
      }
      if (ink) out[ty * THUMB_STRIDE + (tx >> 3)] |= 0x80 >> (tx & 7);
    }
  }
}

static bool readFrame(uint32_t id, uint8_t* into) {
  if (!g_fsReady || !id) return false;
  File f = LittleFS.open(framePath(id), "r");
  if (!f) return false;
  const size_t n = f.read(into, CANVAS_BYTES);
  f.close();
  return n == CANVAS_BYTES;
}

static String recentsJson() {
  static uint8_t thumb[THUMB_BYTES];
  String j = "[";
  int start = 0; bool first = true;
  while (start < (int)g_recents.length()) {
    int end = g_recents.indexOf(REC_SEP, start);
    if (end < 0) end = g_recents.length();
    const String rec = g_recents.substring(start, end);
    start = end + 1;
    const int a = rec.indexOf(FLD_SEP);
    const int b = a < 0 ? -1 : rec.indexOf(FLD_SEP, a + 1);
    const int c = b < 0 ? -1 : rec.indexOf(FLD_SEP, b + 1);
    const int d = c < 0 ? -1 : rec.indexOf(FLD_SEP, c + 1);
    if (d < 0) continue;
    const uint32_t fid = (uint32_t)strtoul(rec.substring(d + 1).c_str(), nullptr, 10);
    if (!first) j += ',';
    first = false;
    j += "{\"mode\":\"";      j += jsonEscape(rec.substring(0, a));
    j += "\",\"text\":\"";    j += jsonEscape(rec.substring(a + 1, b));
    j += "\",\"caption\":\""; j += jsonEscape(rec.substring(b + 1, c));
    j += "\",\"size\":";      j += rec.substring(c + 1, d).toInt();
    if (fid && readFrame(fid, g_frameBuf)) {
      makeThumb(g_frameBuf, thumb);
      j += ",\"thumb\":\"";   j += base64(thumb, THUMB_BYTES); j += "\"";
    }
    j += "}";
  }
  j += "]";
  return j;
}

// Hands back the full frame for one recent, so tapping a drawing brings it back
// at full resolution. Kept out of /api/status because six frames of base64 is
// 31 KB on every poll, against 2 KB of thumbnails.
static bool recentFrame(uint16_t index, uint8_t* into) {
  int start = 0;
  for (uint16_t i = 0; start < (int)g_recents.length(); i++) {
    int end = g_recents.indexOf(REC_SEP, start);
    if (end < 0) end = g_recents.length();
    if (i == index) return readFrame(recField(g_recents.substring(start, end), 4), into);
    start = end + 1;
  }
  return false;
}

// `kind` distinguishes the three browser-rendered tabs, which the device cannot
// tell apart on its own — they all arrive as a bare frame.
static bool applyContent(Mode mode, const String& text, const String& caption,
                         uint8_t size, String& err, const char* kind = "image") {
  QrPlan plan = {0, 0, 0, 0, 0, 0};
  if (!renderToCanvas(canvas, mode, text, caption, size, err, &plan)) return false;
  g_mode    = mode;
  g_text    = (mode == MODE_NONE) ? "" : text;
  g_caption = (mode == MODE_QR)   ? caption : "";
  if (mode == MODE_TEXT) g_size = size;
  g_qrPlanValid = (mode == MODE_QR);
  if (g_qrPlanValid) g_qrPlan = plan;
  if      (mode == MODE_TEXT)  pushRecent("text", text, "",      size, nullptr);
  else if (mode == MODE_QR)    pushRecent("qr",   text, caption, size, nullptr);
  else if (mode == MODE_IMAGE) pushRecent(kind,   text, "",      size, g_image);
  // A clear must leave the glass genuinely blank, so never do it differentially.
  pushCanvas(mode == MODE_NONE);
  saveContent();
  return true;
}

// Boot screen shown only when NVS holds no content yet, so the panel tells you
// where to point your phone instead of sitting blank.
static void drawBootHint() {
  String s = g_apActive
      ? String("Wi-Fi setup\njoin \"") + AP_SSID + "\"\nthen open\n192.168.4.1"
      : String("e-ink label\nhttp://") + WiFi.localIP().toString();
  renderTextToCanvas(canvas, s, 2);
  pushCanvas();
}

// ============================================================================
//  Wi-Fi
// ============================================================================

// Must be called after the radio is started; a mode change can reset it.
static void applyTxPower() {
  if (esp_wifi_set_max_tx_power((int8_t)(WIFI_TX_DBM * 4)) != ESP_OK) return;
  int8_t got = 0;
  esp_wifi_get_max_tx_power(&got);
  Serial.printf("[wifi] tx power %.2f dBm\n", got / 4.0);
}

static void loadCreds() {
  prefs.begin("wifi", true);
  g_ssid       = prefs.getString("ssid", "");
  g_pass       = prefs.getString("pass", "");
  g_credsProven = prefs.getBool("proven", false);
  prefs.end();
  g_haveCreds = g_ssid.length() > 0;
}

// These reasons mean the credentials are wrong, not that the network is away.
// Retrying them is futile, and on a single-radio chip it actively harms us: the
// STA keeps pulling the shared radio onto the target's channel, which moves the
// SoftAP's beacon off AP_CHANNEL and can make the setup portal unreachable.
static bool isAuthFailure(uint8_t r) {
  return r == 2    // AUTH_EXPIRE
      || r == 15   // 4WAY_HANDSHAKE_TIMEOUT
      || r == 202  // AUTH_FAIL
      || r == 204  // HANDSHAKE_TIMEOUT
      || r == 205; // CONNECTION_FAIL
}

static void startAP(bool apOnly = false) {
  const wifi_mode_t want = (g_haveCreds && !apOnly) ? WIFI_AP_STA : WIFI_AP;
  if (!WiFi.mode(want)) Serial.println(F("[wifi] ERROR: WiFi.mode() failed"));

  // Pin the AP subnet rather than trusting the netif default: softAPIP() reports
  // 192.168.4.1 from that default even when the AP never actually started, which
  // is exactly how a dead AP can look healthy in the log.
  const IPAddress apIp(192, 168, 4, 1), apMask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(apIp, apIp, apMask)) Serial.println(F("[wifi] ERROR: softAPConfig() failed"));

  bool ok = WiFi.softAP(AP_SSID, NULL, g_apChannel, 0, 4);   // open, visible
  if (!ok) {
    Serial.println(F("[wifi] softAP() failed — retrying on channel 1"));
    delay(300);
    ok = WiFi.softAP(AP_SSID, NULL, 1, 0, 4);
  }
  g_apActive = ok;
  if (!ok) {
    Serial.println(F("[wifi] ERROR: SoftAP could not be started"));
    return;
  }
  applyTxPower();
  delay(100);

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());         // captive portal: catch every name

  // Report what the driver actually holds, not what we asked for.
  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK) {
    Serial.printf("[wifi] driver AP: ssid=\"%s\" ch=%u hidden=%u auth=%u maxconn=%u\n",
                  (const char*)conf.ap.ssid, conf.ap.channel, conf.ap.ssid_hidden,
                  (unsigned)conf.ap.authmode, conf.ap.max_connection);
  } else {
    Serial.println(F("[wifi] ERROR: esp_wifi_get_config(AP) failed"));
  }
  int8_t txp = 0;
  esp_wifi_get_max_tx_power(&txp);
  Serial.printf("[wifi] SoftAP \"%s\" up | bssid %s | ip %s | mode %d | tx %d (0.25dBm) | heap %u\n",
                AP_SSID, WiFi.softAPmacAddress().c_str(), WiFi.softAPIP().toString().c_str(),
                (int)WiFi.getMode(), txp, (unsigned)ESP.getFreeHeap());
}

static void startMDNS() {
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[wifi] mDNS: http://%s.local\n", HOSTNAME);
  }
}

// esp-idf disconnect reasons worth recognising:
//   201 NO_AP_FOUND        target SSID not seen (wrong name, or 5 GHz-only)
//   205 CONNECTION_FAIL    association rejected
//    15 4WAY_HANDSHAKE_TIMEOUT / 2 AUTH_EXPIRE   almost always a wrong password
static void onStaDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
  g_lastReason = info.wifi_sta_disconnected.reason;
  Serial.printf("[wifi] disconnected, reason %u\n", (unsigned)g_lastReason);
}

static void netBegin() {
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);                // we run our own backoff
  WiFi.setHostname(HOSTNAME);
  loadCreds();

  if (!g_haveCreds) {
    Serial.println(F("[wifi] no saved credentials — starting setup AP"));
    startAP();
    return;
  }

  Serial.printf("[wifi] connecting to \"%s\"", g_ssid.c_str());
  WiFi.mode(WIFI_STA);
  applyTxPower();
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    g_wasConnected = true;
    Serial.printf("[wifi] connected, http://%s\n", WiFi.localIP().toString().c_str());
    startMDNS();
  } else {
    Serial.println(F("[wifi] not connected yet — retrying in the background"));
    g_lastAttempt = millis();
  }
}

// Non-blocking reconnect with exponential backoff. The panel keeps its content
// throughout; network state never touches what is displayed.
static void netTick() {
  if (g_apActive) dns.processNextRequest();
  if (!g_haveCreds || g_staDisabled) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!g_wasConnected) {
      g_wasConnected = true;
      g_failures = 0;
      g_backoff = 5000;
      Serial.printf("[wifi] reconnected, http://%s\n", WiFi.localIP().toString().c_str());
      if (!g_credsProven) {                 // written once, not on every reconnect
        g_credsProven = true;
        prefs.begin("wifi", false);
        prefs.putBool("proven", true);
        prefs.end();
        Serial.println(F("[wifi] credentials confirmed working"));
      }
      startMDNS();
    }
    return;
  }

  if (g_wasConnected) {
    g_wasConnected = false;
    g_lastAttempt = millis();
    Serial.println(F("[wifi] link lost"));
    return;
  }

  if (millis() - g_lastAttempt < g_backoff) return;

  g_lastAttempt = millis();
  g_failures++;
  Serial.printf("[wifi] retry %u (next in %lus)\n", g_failures, (unsigned long)(g_backoff / 1000));
  WiFi.disconnect();
  applyTxPower();
  WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  g_backoff = min(g_backoff * 2, BACKOFF_MAX);

  // After a few failures, raise the setup AP so credentials can be corrected
  // without a serial cable.
  if (g_failures < 3) return;

  // Reasons 2 and 15 are not exclusively "wrong password" — a marginal signal
  // produces them too. This board taught us that the hard way: reason 2 on a
  // perfectly correct password, because it was not radiating cleanly at 20 dBm.
  // Giving up on them unconditionally means a distant, congested or rebooting
  // router can park a working label in AP-only mode until someone power-cycles
  // it, which is no good on a wall. So the test is not what the reason code
  // says, it is whether these credentials have ever connected.
  if (isAuthFailure(g_lastReason) && !g_credsProven) {
    // Never worked: almost certainly a typo. Stop the STA entirely and hold a
    // clean AP-only radio, so the portal actually sits on AP_CHANNEL where it
    // can be seen.
    Serial.printf("[wifi] credentials rejected (reason %u) — STA off, AP-only for setup\n",
                  g_lastReason);
    g_staDisabled = true;
    WiFi.disconnect(true);
    startAP(true);
    return;
  }
  if (isAuthFailure(g_lastReason)) {
    Serial.printf("[wifi] reason %u, but these credentials have connected before — "
                  "treating as interference and still retrying\n", g_lastReason);
  }
  if (!g_apActive) {
    Serial.println(F("[wifi] repeated failures — raising setup AP alongside"));
    startAP();
  }
}

// ============================================================================
//  HTTP
// ============================================================================

// How the code was sized, so the page can show it instead of hiding it in the
// serial log. Module size steps in whole pixels, and the steps are invisible
// otherwise: shortening a URL may buy a bigger module or may buy nothing.
static String qrInfoJson(const QrPlan& p) {
  String j = "{\"version\":";  j += p.version;
  j += ",\"ecc\":\"";         j += QR_ECC_NAME[p.ecc];
  j += "\",\"modules\":";     j += p.modules;
  j += ",\"scale\":";         j += p.scale;
  j += ",\"side\":";          j += p.side;
  j += ",\"quiet\":";         j += p.quiet;
  j += ",\"atLimit\":";       j += (p.scale <= QR_MIN_SCALE) ? "true" : "false";
  j += ",\"mm\":\"";          j += String(p.scale * 48.55f / 250.0f, 2);
  j += "\"}";
  return j;
}

static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendOk() {
  String j = String("{\"ok\":true,\"changed\":") + (g_lastPushChanged ? "true" : "false");
  if (g_qrPlanValid) { j += ",\"qr\":"; j += qrInfoJson(g_qrPlan); }
  j += "}";
  sendJson(200, j);
}
static void sendErr(const String& e)    { sendJson(400, String("{\"ok\":false,\"error\":\"") + jsonEscape(e) + "\"}"); }

static const char* modeName(Mode m) {
  return m == MODE_TEXT ? "text" : (m == MODE_QR ? "qr" : (m == MODE_IMAGE ? "image" : "none"));
}

// ── Firmware upload ─────────────────────────────────────────────────────────
// Authentication is checked at UPLOAD_FILE_START rather than in the completion
// handler: the ESP32 WebServer runs the upload callback while the body streams
// in, so checking afterwards would mean accepting a megabyte from anyone on the
// network before saying no.
static bool otaLocked() {
  if (g_otaPw.length() == 0) {
    server.send(403, "text/plain",
                "Firmware upload is off.\n\n"
                "Set a password over USB serial first:  OTAPW:<password>\n"
                "Clear it again with:                   OTAPW:\n");
    return true;
  }
  if (!server.authenticate("admin", g_otaPw.c_str())) {
    server.requestAuthentication();
    return true;
  }
  return false;
}

static void handleUpdatePage() {
  if (otaLocked()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE_UPDATE);
}

static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_otaAuthed = (g_otaPw.length() > 0) &&
                  server.authenticate("admin", g_otaPw.c_str());
    g_otaFailed = false;
    if (!g_otaAuthed) return;
    Serial.printf("[ota] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); g_otaFailed = true; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_otaAuthed || g_otaFailed) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial); g_otaFailed = true;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!g_otaAuthed || g_otaFailed) return;
    if (Update.end(true)) Serial.printf("[ota] wrote %u bytes\n", (unsigned)up.totalSize);
    else { Update.printError(Serial); g_otaFailed = true; }
  }
}

static void handleUpdateDone() {
  if (!g_otaAuthed) { otaLocked(); return; }
  const bool ok = !g_otaFailed && !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(ok ? 200 : 500, "text/html",
              ok ? "<meta name=viewport content='width=device-width'>"
                   "<body style='font:16px system-ui;padding:24px'>"
                   "<h2>Updated</h2><p>Rebooting. The panel keeps its content.</p>"
                 : "<meta name=viewport content='width=device-width'>"
                   "<body style='font:16px system-ui;padding:24px'>"
                   "<h2>Update failed</h2><p>Nothing was changed. See the serial log.</p>");
  if (ok) { delay(400); ESP.restart(); }
}

static void handleRoot() {
  if (!g_haveCreds) {                     // first boot: go straight to setup
    server.sendHeader("Location", "/wifi");
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE_MAIN);
}

static void handleWifiPage() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE_WIFI);
}

static void handleStatus() {
  String j;
  j.reserve(CANVAS_BYTES * 2 + RECENTS_MAX_LEN + RECENTS_MAX * 400 + 512);
  // Every field closes its own quotes and writes its own trailing comma. The
  // previous style left the closing quote to the start of the NEXT line, which
  // reads fine right up until someone inserts a field in the middle: adding
  // "build" that way shipped a status endpoint returning invalid JSON, and a
  // page that reported the label unreachable because r.json() threw.
  j  = "{";
  j += "\"mode\":\"";    j += modeName(g_mode);                      j += "\",";
  j += "\"text\":\"";    j += jsonEscape(g_text);                    j += "\",";
  j += "\"caption\":\""; j += jsonEscape(g_caption);                 j += "\",";
  j += "\"build\":\"";   j += jsonEscape(BUILD_ID);                  j += "\",";
  j += "\"size\":";      j += g_size;                               j += ",";
  j += "\"w\":";         j += PANEL_W;                              j += ",";
  j += "\"h\":";         j += PANEL_H;                              j += ",";
  j += "\"updates\":";   j += g_updates;                            j += ",";
  j += "\"ap\":";        j += (g_apActive && WiFi.status() != WL_CONNECTED)
                               ? "true" : "false";                   j += ",";
  if (WiFi.status() == WL_CONNECTED) {
    j += "\"ssid\":\"";  j += jsonEscape(WiFi.SSID());               j += "\",";
    j += "\"ip\":\"";    j += WiFi.localIP().toString();             j += "\",";
    j += "\"rssi\":";    j += WiFi.RSSI();                          j += ",";
  } else {
    j += "\"ssid\":\"";  j += jsonEscape(AP_SSID);                   j += "\",";
    j += "\"ip\":\"";    j += WiFi.softAPIP().toString();            j += "\",";
    j += "\"rssi\":0,";
  }
  if (g_qrPlanValid) { j += "\"qr\":"; j += qrInfoJson(g_qrPlan);     j += ","; }
  j += "\"thumbW\":";    j += THUMB_W;                              j += ",";
  j += "\"thumbH\":";    j += THUMB_H;                              j += ",";
  j += "\"recents\":";   j += recentsJson();                        j += ",";
  j += "\"preview\":\""; j += base64(canvas.getBuffer(), CANVAS_BYTES);
  j += "\"}";
  sendJson(200, j);
}

// Renders into the scratch canvas and returns the bits without touching the
// panel, so the page can show exactly what a print would produce.
static void handlePreview() {
  String mode = server.arg("mode");
  String text = server.arg("text");
  String cap  = server.arg("caption");
  uint8_t size = (uint8_t)server.arg("size").toInt();
  if (size < 1 || size > 3) size = g_size;

  Mode m;
  if (mode == "text")    m = MODE_TEXT;
  else if (mode == "qr") m = MODE_QR;
  else { sendErr("mode must be \"text\" or \"qr\""); return; }

  String err;
  QrPlan plan = {0, 0, 0, 0, 0, 0};
  if (!renderToCanvas(scratch, m, text, cap, size, err, &plan)) { sendErr(err); return; }

  String j;
  j.reserve(CANVAS_BYTES * 2);
  j  = "{\"ok\":true,\"w\":"; j += PANEL_W;
  j += ",\"h\":";              j += PANEL_H;
  if (m == MODE_QR) { j += ",\"qr\":"; j += qrInfoJson(plan); }
  j += ",\"preview\":\"";     j += base64(scratch.getBuffer(), CANVAS_BYTES);
  j += "\"}";
  sendJson(200, j);
}

static void handleDisplay() {
  String mode = server.arg("mode");
  String text = server.arg("text");
  String cap  = server.arg("caption");
  uint8_t size = (uint8_t)server.arg("size").toInt();
  if (size < 1 || size > 3) size = g_size;

  Mode m;
  if (mode == "text")    m = MODE_TEXT;
  else if (mode == "qr") m = MODE_QR;
  else { sendErr("mode must be \"text\" or \"qr\""); return; }

  String err;
  if (applyContent(m, text, cap, size, err)) sendOk();
  else                                       sendErr(err);
}

// Accepts a browser-rasterised frame: base64 of exactly CANVAS_BYTES packed
// 1-bit pixels. `text` is the source string, kept only so the page can
// repopulate its field; it is not re-rendered on the device.
static void handleImage() {
  const String bits = server.arg("bits");
  if (bits.length() == 0) { sendErr("missing image data"); return; }
  if (!base64Decode(bits, g_image, CANVAS_BYTES)) {
    sendErr(String("image must decode to exactly ") + CANVAS_BYTES + " bytes ("
            + PANEL_W + "x" + PANEL_H + ", 1 bit per pixel)");
    return;
  }
  g_haveImage = true;

  String err;
  String kind = server.arg("kind");
  if (kind != "draw" && kind != "rich" && kind != "image") kind = "image";
  if (applyContent(MODE_IMAGE, server.arg("text"), "", g_size, err, kind.c_str())) sendOk();
  else                                                                            sendErr(err);
}

static void handleRecentFrame() {
  const int i = server.arg("i").toInt();
  if (i < 0 || !recentFrame((uint16_t)i, g_frameBuf)) { sendErr("no frame for that entry"); return; }
  String j;
  j.reserve(CANVAS_BYTES * 2 + 64);
  j  = "{\"ok\":true,\"w\":"; j += PANEL_W;
  j += ",\"h\":";              j += PANEL_H;
  j += ",\"bits\":\"";         j += base64(g_frameBuf, CANVAS_BYTES);
  j += "\"}";
  sendJson(200, j);
}

static void handleRecents() {
  if (server.arg("clear") == "1") {
    g_recents = "";
    gcFrames();
    prefs.begin("eink", false); prefs.putString("recents", g_recents); prefs.end();
  }
  sendJson(200, String("{\"ok\":true,\"recents\":") + recentsJson() + "}");
}

static void handleClear() {
  String err;
  if (applyContent(MODE_NONE, "", "", g_size, err)) sendOk();
  else                                              sendErr(err);
}

static void handleScan() {
  int n = WiFi.scanNetworks();
  String j = "{\"nets\":[";
  int listed = 0;
  for (int i = 0; i < n && listed < 20; i++) {
    if (WiFi.SSID(i).length() == 0) continue;
    if (listed++) j += ',';
    j += "{\"ssid\":\""; j += jsonEscape(WiFi.SSID(i));
    j += "\",\"rssi\":"; j += WiFi.RSSI(i);
    j += ",\"open\":";   j += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "true" : "false";
    j += '}';
  }
  j += "]}";
  WiFi.scanDelete();
  sendJson(200, j);
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32) { sendErr("SSID must be 1-32 characters"); return; }
  if (pass.length() > 63)                       { sendErr("password may be at most 63 characters"); return; }

  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putBool("proven", false);
  prefs.end();

  sendOk();
  g_rebootAt = millis() + 800;         // let the response flush first
}

static void handleNotFound() {
  if (g_apActive) {                    // captive-portal probe → bounce to the UI
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/wifi");
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "not found");
}

static void serverBegin() {
  server.on("/update",      HTTP_GET,  handleUpdatePage);
  server.on("/update",      HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/wifi",        HTTP_GET,  handleWifiPage);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/scan",    HTTP_GET,  handleScan);
  server.on("/api/preview", HTTP_POST, handlePreview);
  server.on("/api/display", HTTP_POST, handleDisplay);
  server.on("/api/image",   HTTP_POST, handleImage);
  server.on("/api/clear",   HTTP_POST, handleClear);
  server.on("/api/recents", HTTP_POST, handleRecents);
  server.on("/api/recent",  HTTP_GET,  handleRecentFrame);
  server.on("/api/wifi",    HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("[http] server started on :80"));
}

// ============================================================================
//  Serial line protocol:  TEXT:<string> | QR:<string> | CLEAR
// ============================================================================

// Prints what the radio and the content state actually are. Diagnostic only.
static void printStatus() {
  Serial.printf("build=%s\n", BUILD_ID);
  Serial.printf("creds_proven=%d sta_disabled=%d last_reason=%u\n",
                (int)g_credsProven, (int)g_staDisabled, g_lastReason);
  Serial.printf("mode=%d ap_active=%d wifi_mode=%d heap=%u\n",
                (int)g_mode, (int)g_apActive, (int)WiFi.getMode(), (unsigned)ESP.getFreeHeap());
  Serial.printf("ap_ssid=%s ap_ip=%s ap_mac=%s clients=%u\n",
                AP_SSID, WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(), WiFi.softAPgetStationNum());
  Serial.printf("sta_state=%d sta_ssid=%s sta_ip=%s rssi=%d\n",
                (int)WiFi.status(), WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  // What is actually in NVS. The password is never printed, only its length,
  // which is enough to catch an empty or truncated save.
  Serial.printf("stored_ssid=\"%s\" (%u chars) stored_pass_len=%u failures=%u backoff=%lums\n",
                g_ssid.c_str(), g_ssid.length(), g_pass.length(),
                g_failures, (unsigned long)g_backoff);
  Serial.printf("content=\"%s\" size=%u updates=%lu\n",
                g_text.c_str(), g_size, (unsigned long)g_updates);
}

// Scanning proves the radio receives. If this lists neighbours but no other
// device can see our SoftAP, the fault is transmit-side, not configuration.
static void doScan() {
  wifi_mode_t prev = WiFi.getMode();
  if (!(prev & WIFI_MODE_STA)) WiFi.mode((wifi_mode_t)(prev | WIFI_MODE_STA));
  Serial.println(F("[scan] scanning 2.4 GHz..."));
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.printf("[scan] %d networks found — radio receive problem\n", n);
  } else {
    for (int i = 0; i < n; i++) {
      Serial.printf("[scan] %2d  %-32s ch%-3d %4d dBm\n",
                    i + 1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  String err;
  bool ok = false;

  if (line.equalsIgnoreCase("STATUS")) {
    printStatus();
    return;
  } else if (line.equalsIgnoreCase("SCAN")) {
    doScan();
    return;
  } else if (line.startsWith("APCH:")) {
    // Move the SoftAP to another 2.4 GHz channel without reflashing, so the
    // band can be swept to find where this board actually radiates.
    int ch = line.substring(5).toInt();
    if (ch < 1 || ch > 13) { Serial.println(F("ERR channel must be 1-13")); return; }
    g_apChannel = (uint8_t)ch;
    startAP(true);
    return;
  } else if (line.startsWith("TXPW:")) {
    // Transmit power in whole dBm (2..20). A badly matched antenna can radiate
    // better backed off than at full power, so this is worth sweeping too.
    int dbm = line.substring(5).toInt();
    if (dbm < 2 || dbm > 20) { Serial.println(F("ERR power must be 2-20 dBm")); return; }
    esp_err_t e = esp_wifi_set_max_tx_power((int8_t)(dbm * 4));
    int8_t got = 0; esp_wifi_get_max_tx_power(&got);
    Serial.printf("OK tx power set to %d dBm (driver reports %d = %.2f dBm) err=%d\n",
                  dbm, got, got / 4.0, (int)e);
    return;
  } else if (line.equalsIgnoreCase("FORGET")) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", "");
    prefs.putString("pass", "");
    prefs.putBool("proven", false);
    prefs.end();
    Serial.println(F("OK credentials cleared — rebooting into setup AP"));
    g_rebootAt = millis() + 300;
    return;
  } else if (line.equalsIgnoreCase("RECONNECT")) {
    // Force an immediate STA attempt instead of waiting out the backoff.
    g_failures = 0;
    g_backoff = 5000;
    g_lastAttempt = 0;
    g_staDisabled = false;
    WiFi.mode(g_apActive ? WIFI_AP_STA : WIFI_STA);
    Serial.printf("[wifi] forcing reconnect to \"%s\"\n", g_ssid.c_str());
    WiFi.disconnect();
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    return;
  } else if (line.startsWith("WIFI:")) {
    // WIFI:<ssid>,<password> — provisioning that does not need the SoftAP.
    // Split on the first comma only, so passwords may contain commas.
    String rest = line.substring(5);
    int comma = rest.indexOf(',');
    String ssid = (comma < 0) ? rest : rest.substring(0, comma);
    String pass = (comma < 0) ? String("") : rest.substring(comma + 1);
    ssid.trim();
    if (ssid.length() == 0 || ssid.length() > 32) { Serial.println(F("ERR SSID must be 1-32 chars")); return; }
    if (pass.length() > 63)                       { Serial.println(F("ERR password may be at most 63 chars")); return; }
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putBool("proven", false);
    prefs.end();
    Serial.printf("OK saved \"%s\" — rebooting\n", ssid.c_str());
    g_rebootAt = millis() + 300;
    return;
  } else if (line.equalsIgnoreCase("CLEAR")) {
    ok = applyContent(MODE_NONE, "", "", g_size, err);
  } else if (line.equalsIgnoreCase("RECENTS")) {
    Serial.println(g_recents.length() ? recentsJson() : "[]");
    return;
  } else if (line.equalsIgnoreCase("RECENTS:CLEAR")) {
    g_recents = "";
    prefs.begin("eink", false); prefs.putString("recents", ""); prefs.end();
    Serial.println(F("OK recents cleared"));
    return;
  } else if (line.startsWith("OTAPW:")) {
    g_otaPw = line.substring(6);
    g_otaPw.trim();
    prefs.begin("eink", false); prefs.putString("otapw", g_otaPw); prefs.end();
    Serial.println(g_otaPw.length()
      ? "OK firmware upload enabled at http://eink.local/update (user \"admin\")"
      : "OK firmware upload disabled");
    return;
  } else if (line.startsWith("TEXT:")) {
    ok = applyContent(MODE_TEXT, line.substring(5), "", g_size, err);
  } else if (line.startsWith("QRC:")) {
    // QRC:<caption>|<payload> — caption first, split on the FIRST bar, so the
    // payload (a URL, typically) may itself contain one.
    const String rest = line.substring(4);
    const int bar = rest.indexOf('|');
    if (bar < 0) { Serial.println(F("ERR QRC needs QRC:<caption>|<payload>")); return; }
    ok = applyContent(MODE_QR, rest.substring(bar + 1), rest.substring(0, bar), g_size, err);
  } else if (line.startsWith("QR:")) {
    ok = applyContent(MODE_QR, line.substring(3), "", g_size, err);
  } else {
    Serial.println(F("ERR unknown — TEXT:<s> | QR:<s> | QRC:<cap>|<s> | CLEAR | WIFI:<ssid>,<pass> | FORGET | STATUS | SCAN | RECONNECT"));
    return;
  }
  Serial.println(ok ? "OK" : ("ERR " + err));
}

static void serialTick() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    // Accept CR, LF or CRLF: terminals differ, and `pio device monitor` sends a
    // bare CR on Enter. An empty line is ignored, so CRLF fires exactly once.
    if (c == '\n' || c == '\r') { handleSerialLine(buf); buf = ""; }
    else if (buf.length() < MAX_TEXT_LEN + 8) buf += c;
  }
}

// ============================================================================
//  setup / loop
// ============================================================================

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);   // USB CDC needs a moment
  Serial.println();
  Serial.println(F("=== Wi-Fi e-ink label ==="));
  Serial.printf("[fw] build %s\n", BUILD_ID);
  Serial.printf("[epd] panel %ux%u landscape\n", PANEL_W, PANEL_H);

  displayBegin();

  // The frames behind bitmap recents. Formats on first use, a one-off second or
  // two. A failure is not fatal: pushRecent() then declines to remember bitmap
  // entries, exactly as it behaved before this existed.
  g_fsReady = LittleFS.begin(true);
  if (g_fsReady) Serial.printf("[fs] mounted, %u of %u bytes used\n",
                               (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else           Serial.println(F("[fs] mount failed — bitmap recents disabled"));

  // Seed the NVS namespaces and keys on a virgin chip. Preferences logs an [E]
  // line for every read of a key that does not exist yet, even when the read
  // supplies a default, so writing them once keeps the boot log clean.
  prefs.begin("eink", false);
  if (!prefs.isKey("text")) {
    prefs.putUChar("mode", MODE_NONE);
    prefs.putUChar("size", 2);
    prefs.putString("text", "");
    prefs.putString("cap", "");
    prefs.putString("otapw", "");
    prefs.putString("recents", "");
  }
  prefs.end();
  prefs.begin("wifi", false);
  if (!prefs.isKey("ssid")) {
    prefs.putString("ssid", "");
    prefs.putString("pass", "");
  }
  if (!prefs.isKey("proven")) prefs.putBool("proven", false);   // upgrades too
  prefs.end();

  // Restore and redraw once — this is a full refresh, per the boot rule.
  loadContent();
  gcFrames();                       // drop frames orphaned by an interrupted write
  if (g_mode != MODE_NONE) {
    String err;
    QrPlan plan = {0, 0, 0, 0, 0, 0};
    if (renderToCanvas(canvas, g_mode, g_text, g_caption, g_size, err, &plan)) {
      g_qrPlanValid = (g_mode == MODE_QR);
      if (g_qrPlanValid) g_qrPlan = plan;
      pushCanvas();
    } else {
      Serial.printf("[epd] stored content rejected: %s\n", err.c_str());
    }
  }

  netBegin();
  serverBegin();

  if (g_mode == MODE_NONE) drawBootHint();

  Serial.println(F("[rdy] TEXT:<s> | QR:<s> | QRC:<cap>|<s> | CLEAR | WIFI:<ssid>,<pass> | STATUS | SCAN"));
}

void loop() {
  server.handleClient();
  netTick();
  serialTick();
  panelTick();

  if (g_rebootAt && millis() > g_rebootAt) {
    Serial.println(F("[sys] rebooting with new credentials"));
    delay(50);
    ESP.restart();
  }
  delay(2);
}
