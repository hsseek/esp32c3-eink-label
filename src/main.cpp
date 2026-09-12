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
#define EPD_PANEL_CLASS GxEPD2_213_BN

GxEPD2_BW<EPD_PANEL_CLASS, EPD_PANEL_CLASS::HEIGHT> display(
    EPD_PANEL_CLASS(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// Landscape: the panel's native portrait 122x250 becomes 250x122 at rotation 1.
// Use WIDTH_VISIBLE, not WIDTH: SSD1680 panels have 128 columns of controller RAM
// but only 122 of them reach the glass. GxEPD2_BW builds its Adafruit_GFX space
// from WIDTH_VISIBLE too, so anything drawn past it is silently clipped.
static const uint16_t PANEL_W = EPD_PANEL_CLASS::HEIGHT;
static const uint16_t PANEL_H = EPD_PANEL_CLASS::WIDTH_VISIBLE;

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
static const uint8_t  QR_MAX_VERSION     = 10;   // 57x57 modules; bigger is unreadable here
static const uint8_t  QR_ECC             = ECC_MEDIUM;
static const uint8_t  QR_QUIET           = 4;    // quiet zone, in modules
static const uint8_t  QR_MIN_SCALE       = 2;    // px per module; 1 is unscannable here
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

// ── Content state ───────────────────────────────────────────────────────────
enum Mode : uint8_t { MODE_NONE = 0, MODE_TEXT = 1, MODE_QR = 2, MODE_IMAGE = 3 };

// A 1-bit frame rasterised by the browser: emoji, any font the phone has, an
// uploaded picture, all resolved to the panel's own format before it arrives.
// Same packing as GFXcanvas1 — MSB first, 1 = black ink.
static uint8_t  g_image[CANVAS_BYTES];
static bool     g_haveImage = false;

static Mode     g_mode    = MODE_NONE;
static String   g_text    = "";
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

static void renderTextToCanvas(GFXcanvas1& c, const String& raw, uint8_t size) {
  String s = sanitizeForFont(raw);

  const uint16_t cw    = 6 * size;               // built-in font cell
  const uint16_t chh   = 8 * size;
  const uint16_t pitch = chh + (size >= 2 ? 3 : 2);   // a little leading
  uint16_t cols    = PANEL_W / cw;
  uint8_t  maxRows = PANEL_H / pitch;
  if (maxRows > MAX_LINES) maxRows = MAX_LINES;
  if (cols < 4) cols = 4;

  String lines[MAX_LINES];
  bool truncated = false;
  uint8_t n = wrapText(s, cols, maxRows, lines, truncated);

  if (truncated && n > 0) {                       // "…" as three dots: the
    String& last = lines[n - 1];                  // built-in font has no U+2026
    if (last.length() > (uint16_t)(cols - 3)) last = last.substring(0, cols - 3);
    last += "...";
  }

  c.fillScreen(0);
  c.setFont(NULL);
  c.setTextSize(size);
  c.setTextColor(1);
  c.setTextWrap(false);

  int16_t y0 = ((int16_t)PANEL_H - (int16_t)(n * pitch)) / 2;
  if (y0 < 0) y0 = 0;
  for (uint8_t i = 0; i < n; i++) {
    int16_t x = ((int16_t)PANEL_W - (int16_t)(lines[i].length() * cw)) / 2;
    if (x < 0) x = 0;
    c.setCursor(x, y0 + i * pitch);
    c.print(lines[i]);
  }
}

// ============================================================================
//  Rendering — QR
// ============================================================================

// Picks the smallest QR version that holds the payload, then the largest whole
// module size that still leaves a 4-module quiet zone. Centered.
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

static bool renderQrToCanvas(GFXcanvas1& c, const String& payload, String& err) {
  static uint8_t qrBuf[QR_BUF_BYTES];
  QRCode qr;
  uint8_t version = 0;
  const uint16_t len = payload.length();

  for (uint8_t v = 1; v <= QR_MAX_VERSION; v++) {
    if (len <= QR_BYTE_CAPACITY[QR_ECC][v - 1]) { version = v; break; }
  }
  if (!version) {
    err = String("QR payload too long: ") + len + " bytes, limit "
        + QR_BYTE_CAPACITY[QR_ECC][QR_MAX_VERSION - 1];
    return false;
  }
  if (qrcode_initText(&qr, qrBuf, version, QR_ECC, payload.c_str()) != 0) {
    err = "QR encoding failed";
    return false;
  }

  const uint16_t modules = qr.size;
  const uint16_t total   = modules + 2 * QR_QUIET;
  uint16_t scale = min(PANEL_W / total, PANEL_H / total);
  if (scale < QR_MIN_SCALE) {
    // One pixel per module on a 0.19 mm pitch panel cannot be scanned; refuse
    // rather than draw a code nothing can read.
    err = String("QR payload too long to stay scannable: ") + len
        + " bytes needs version " + version + " (" + modules + " modules), "
        "which leaves only " + scale + " px per module";
    return false;
  }

  const int16_t side = modules * scale;
  const int16_t ox   = ((int16_t)PANEL_W - side) / 2;
  const int16_t oy   = ((int16_t)PANEL_H - side) / 2;

  c.fillScreen(0);                            // white; quiet zone is just margin
  for (uint16_t y = 0; y < modules; y++) {
    for (uint16_t x = 0; x < modules; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        c.fillRect(ox + x * scale, oy + y * scale, scale, scale, 1);
      }
    }
  }
  Serial.printf("[qr] version %u, %ux%u modules, scale %u\n", version, modules, modules, scale);
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
  display.setRotation(1);                     // landscape, 250 x 122
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
  g_text = prefs.getString("text", "");
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

// Renders into the canvas only; err is filled on rejection.
static bool renderToCanvas(GFXcanvas1& c, Mode mode, const String& text, uint8_t size, String& err) {
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
  return renderQrToCanvas(c, text, err);
}

static bool applyContent(Mode mode, const String& text, uint8_t size, String& err) {
  if (!renderToCanvas(canvas, mode, text, size, err)) return false;
  g_mode = mode;
  g_text = (mode == MODE_NONE) ? "" : text;
  if (mode == MODE_TEXT) g_size = size;
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
  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
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

  if (isAuthFailure(g_lastReason)) {
    // Wrong password: stop the STA entirely and hold a clean AP-only radio, so
    // the portal actually sits on AP_CHANNEL where it can be seen.
    Serial.printf("[wifi] credentials rejected (reason %u) — STA off, AP-only for setup\n",
                  g_lastReason);
    g_staDisabled = true;
    WiFi.disconnect(true);
    startAP(true);
    return;
  }
  if (!g_apActive) {
    Serial.println(F("[wifi] repeated failures — raising setup AP alongside"));
    startAP();
  }
}

// ============================================================================
//  HTTP
// ============================================================================

static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendOk() {
  sendJson(200, String("{\"ok\":true,\"changed\":") +
                (g_lastPushChanged ? "true" : "false") + "}");
}
static void sendErr(const String& e)    { sendJson(400, String("{\"ok\":false,\"error\":\"") + jsonEscape(e) + "\"}"); }

static const char* modeName(Mode m) {
  return m == MODE_TEXT ? "text" : (m == MODE_QR ? "qr" : (m == MODE_IMAGE ? "image" : "none"));
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
  j.reserve(CANVAS_BYTES * 2);
  j  = "{\"mode\":\"";   j += modeName(g_mode);
  j += "\",\"text\":\""; j += jsonEscape(g_text);
  j += "\",\"size\":";   j += g_size;
  j += ",\"w\":";        j += PANEL_W;
  j += ",\"h\":";        j += PANEL_H;
  j += ",\"updates\":";  j += g_updates;
  j += ",\"ap\":";       j += (g_apActive && WiFi.status() != WL_CONNECTED) ? "true" : "false";
  if (WiFi.status() == WL_CONNECTED) {
    j += ",\"ssid\":\""; j += jsonEscape(WiFi.SSID());
    j += "\",\"ip\":\""; j += WiFi.localIP().toString();
    j += "\",\"rssi\":"; j += WiFi.RSSI();
  } else {
    j += ",\"ssid\":\""; j += AP_SSID;
    j += "\",\"ip\":\""; j += WiFi.softAPIP().toString();
    j += "\",\"rssi\":0";
  }
  j += ",\"preview\":\""; j += base64(canvas.getBuffer(), CANVAS_BYTES);
  j += "\"}";
  sendJson(200, j);
}

// Renders into the scratch canvas and returns the bits without touching the
// panel, so the page can show exactly what a print would produce.
static void handlePreview() {
  String mode = server.arg("mode");
  String text = server.arg("text");
  uint8_t size = (uint8_t)server.arg("size").toInt();
  if (size < 1 || size > 3) size = g_size;

  Mode m;
  if (mode == "text")    m = MODE_TEXT;
  else if (mode == "qr") m = MODE_QR;
  else { sendErr("mode must be \"text\" or \"qr\""); return; }

  String err;
  if (!renderToCanvas(scratch, m, text, size, err)) { sendErr(err); return; }

  String j;
  j.reserve(CANVAS_BYTES * 2);
  j  = "{\"ok\":true,\"w\":"; j += PANEL_W;
  j += ",\"h\":";              j += PANEL_H;
  j += ",\"preview\":\"";     j += base64(scratch.getBuffer(), CANVAS_BYTES);
  j += "\"}";
  sendJson(200, j);
}

static void handleDisplay() {
  String mode = server.arg("mode");
  String text = server.arg("text");
  uint8_t size = (uint8_t)server.arg("size").toInt();
  if (size < 1 || size > 3) size = g_size;

  Mode m;
  if (mode == "text")    m = MODE_TEXT;
  else if (mode == "qr") m = MODE_QR;
  else { sendErr("mode must be \"text\" or \"qr\""); return; }

  String err;
  if (applyContent(m, text, size, err)) sendOk();
  else                                  sendErr(err);
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
  if (applyContent(MODE_IMAGE, server.arg("text"), g_size, err)) sendOk();
  else                                                           sendErr(err);
}

static void handleClear() {
  String err;
  if (applyContent(MODE_NONE, "", g_size, err)) sendOk();
  else                                          sendErr(err);
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
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/wifi",        HTTP_GET,  handleWifiPage);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/scan",    HTTP_GET,  handleScan);
  server.on("/api/preview", HTTP_POST, handlePreview);
  server.on("/api/display", HTTP_POST, handleDisplay);
  server.on("/api/image",   HTTP_POST, handleImage);
  server.on("/api/clear",   HTTP_POST, handleClear);
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
    prefs.end();
    Serial.printf("OK saved \"%s\" — rebooting\n", ssid.c_str());
    g_rebootAt = millis() + 300;
    return;
  } else if (line.equalsIgnoreCase("CLEAR")) {
    ok = applyContent(MODE_NONE, "", g_size, err);
  } else if (line.startsWith("TEXT:")) {
    ok = applyContent(MODE_TEXT, line.substring(5), g_size, err);
  } else if (line.startsWith("QR:")) {
    ok = applyContent(MODE_QR, line.substring(3), g_size, err);
  } else {
    Serial.println(F("ERR unknown — TEXT:<s> | QR:<s> | CLEAR | WIFI:<ssid>,<pass> | FORGET | STATUS | SCAN | RECONNECT"));
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
  Serial.printf("[epd] panel %ux%u landscape\n", PANEL_W, PANEL_H);

  displayBegin();

  // Seed the NVS namespaces and keys on a virgin chip. Preferences logs an [E]
  // line for every read of a key that does not exist yet, even when the read
  // supplies a default, so writing them once keeps the boot log clean.
  prefs.begin("eink", false);
  if (!prefs.isKey("text")) {
    prefs.putUChar("mode", MODE_NONE);
    prefs.putUChar("size", 2);
    prefs.putString("text", "");
  }
  prefs.end();
  prefs.begin("wifi", false);
  if (!prefs.isKey("ssid")) {
    prefs.putString("ssid", "");
    prefs.putString("pass", "");
  }
  prefs.end();

  // Restore and redraw once — this is a full refresh, per the boot rule.
  loadContent();
  if (g_mode != MODE_NONE) {
    String err;
    if (renderToCanvas(canvas, g_mode, g_text, g_size, err)) pushCanvas();
    else Serial.printf("[epd] stored content rejected: %s\n", err.c_str());
  }

  netBegin();
  serverBegin();

  if (g_mode == MODE_NONE) drawBootHint();

  Serial.println(F("[rdy] TEXT:<s> | QR:<s> | CLEAR | WIFI:<ssid>,<pass> | STATUS | SCAN"));
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
