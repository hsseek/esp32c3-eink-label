# Wi-Fi e-ink label

Firmware for an **ESP32-C3 SuperMini** driving a **Waveshare 2.13" e-Paper V4**
(SSD1680 / DEPG0213BN, 250 × 122). Set the label's content from your phone's
browser, or over USB serial. Content is stored in NVS and redrawn on boot.

- Web UI served from PROGMEM — no CDN, no internet needed
- TEXT mode (word-wrapped, 3 font sizes) and QR mode (auto version + scaling)
- Pixel-exact preview in the browser: the phone renders the same 1-bit buffer
  that was sent to the panel
- Partial refresh, with a full refresh every 10 updates and on first boot draw
- Panel hibernates after every draw — never left powered idle
- USB serial fallback: `TEXT:<string>`, `QR:<string>`, `CLEAR`

---

## Wiring

| Panel pin  | Waveshare cable | ESP32-C3 pin | Notes |
|------------|-----------------|--------------|-------|
| VCC        | gray            | **3V3**      | 3.3 V only — never the 5V pin |
| GND        | brown           | GND          | |
| DIN (MOSI) | blue            | GPIO7        | |
| CLK (SCK)  | yellow          | GPIO6        | |
| CS         | orange          | GPIO10       | active low |
| DC         | green           | GPIO5        | |
| RST        | white           | GPIO3        | active low |
| BUSY       | purple          | GPIO4        | input |

MISO is unused — e-paper is write-only.

Pins are declared in one block at the top of [`src/main.cpp`](src/main.cpp)
(`PIN_EPD_*`); nothing else in the project hard-codes a GPIO number.

**Do not use:** GPIO9 (BOOT strap — held low at power-up puts the chip in
download mode), GPIO8 and GPIO2 (straps that must read high at boot; GPIO8 is
also the onboard LED), GPIO20/21 (UART0), GPIO18/19 (native USB D-/D+).

If the panel sits on a **Waveshare e-Paper Driver HAT**: "Display config"
jumper to **A**, "Interface config" jumper to **0** (4-wire SPI).

---

## Build and flash

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/):

```bash
python3 -m pip install --user platformio     # or: pipx install platformio
```

Then, from the project root:

```bash
pio run                 # build
pio run -t upload       # build + flash over USB-C
pio device monitor      # 115200 baud, native USB CDC
```

The C3's native USB is USB Serial/JTAG, so a plain USB-C cable both flashes and
carries `Serial`. That works because `platformio.ini` sets
`-D ARDUINO_USB_CDC_ON_BOOT=1`.

If upload fails to find the port, force download mode once: hold **BOOT**, tap
**RESET**, release **BOOT**, then `pio run -t upload`.

---

## First-time Wi-Fi setup

1. Power the board. With no saved credentials it starts an open access point
   named **`eink-setup`**, and the panel shows the setup hint.
2. Join `eink-setup` from your phone. The captive portal should open by itself;
   if it doesn't, browse to **http://192.168.4.1**.
3. Pick your network from the scan list (2.4 GHz only — the C3 has no 5 GHz
   radio), enter the password, tap **Save & reboot**.
4. Reconnect your phone to your home Wi-Fi. The label prints its address to
   serial and, when it has no content yet, shows it on the panel:
   **http://&lt;ip&gt;** — or **http://eink.local** via mDNS.

If the saved network later fails, the firmware retries with exponential backoff
(5 s doubling to 5 min) and, after three failures, raises the `eink-setup` AP
again alongside so you can fix the credentials without a cable. The panel keeps
showing its last content the whole time.

---

## Using it

**Web UI** — open the label's address:

- text field for the content
- TEXT / QR / IMAGE toggle
- font size small / medium / large (TEXT only)
- **Preview** — renders on the device and shows the result in the page *without
  touching the panel*, so you can check the wrap, the truncation or the QR size
  before spending a 2.4 s refresh. The button then becomes **Print to panel**;
  editing any field marks the preview out of date and it reverts to **Preview**.
- **Print now** — skips the preview and draws immediately
- **Clear panel**
- the preview box is outlined while it shows an unprinted preview

Preview runs the real renderer on the device into a separate scratch buffer, so
what you see is the exact bitmap a print would produce — and payloads that are
too long are rejected at preview time rather than after a wasted refresh.

Submitting content identical to what is already displayed is a no-op: the panel
is left alone and the UI says so, rather than flashing for 2.4 s to draw the
same thing.

**Serial** — one command per line, at 115200 baud:

```
TEXT:Hello from the desk
QR:https://example.com/thing
CLEAR
WIFI:<ssid>,<password>     save credentials and reboot
FORGET                     clear credentials, reboot into the setup AP
RECONNECT                  force an immediate retry, re-enabling the STA
STATUS                     radio + content state, incl. stored SSID
SCAN                       list nearby 2.4 GHz networks
APCH:<1-13>                move the SoftAP channel, live
TXPW:<2-20>                set transmit power in dBm, live
```

`TEXT:` uses the font size last chosen in the web UI. The device answers `OK`
or `ERR <reason>`.

### Limits and rejections

Oversized payloads are refused with a message rather than drawn as garbage:

| Mode | Limit |
|------|-------|
| TEXT | 400 characters (`MAX_TEXT_LEN`). Text that wraps past the panel height is truncated with `...` |
| QR   | whatever fits QR version 10 at ECC-M (~270 bytes). Longer payloads are rejected — bigger versions would be too dense to scan at 122 px |
| IMAGE | exactly 3904 bytes once decoded (250 × 122, 1 bit per pixel). Anything else is rejected |

### Emoji, Hangul and pictures — IMAGE mode

The device has no glyph data beyond a 5 × 7 ASCII table, so **TEXT mode folds
anything outside ASCII 32–126 to `?`** — emoji, Hangul, accented Latin. QR mode
is unaffected, since it encodes raw bytes.

IMAGE mode sidesteps the problem entirely: the browser already has every font
and emoji on the phone, so the page rasterises there. It renders your text (or a
picture you choose) onto a 250 × 122 canvas, converts it to 1 bit with
Floyd–Steinberg dithering, and posts the finished 3904-byte frame as base64 to
`POST /api/image`. The device decodes it, blits it, and keeps it in NVS, so it
comes back after a reboot. Nothing is re-rendered on the device.

Dithering rather than a hard threshold matters here: a bright yellow emoji is
high-luminance and would simply disappear under a threshold, but becomes a
recognisable dot pattern when the error is diffused. Plain black text has no
error to diffuse, so it stays crisp.

Two things to expect:

- **The panel is 1 bit, so emoji arrive as silhouettes.** High-contrast
  pictograms (✓ ★ ♥ ⚠ ↑) read well. Detailed or pale ones often do not.
- **IMAGE mode is browser-only.** The serial protocol has no way to send a
  frame, so `TEXT:` and `QR:` remain ASCII-only. The stored source string is
  kept purely so the page can repopulate its field.

To render Hangul from the device itself instead, you would need an Adafruit GFX
bitmap font containing those glyphs; note that `renderTextToCanvas()` wraps on a
fixed 6 × 8 cell and would need reworking for a proportional font.

---

## Project layout

```
platformio.ini      board, USB-CDC flag, library pins
src/main.cpp        pin map, panel class, rendering, Wi-Fi, HTTP, serial
src/web_ui.h        both HTML pages as PROGMEM strings
```

Tunables at the top of `main.cpp`: `FULL_REFRESH_EVERY`, `MAX_TEXT_LEN`,
`QR_MAX_VERSION`, `QR_ECC`, `QR_QUIET`, `AP_SSID`, `HOSTNAME`.

---

## Troubleshooting: the display stays blank

An e-paper panel never reports errors — a wrong setting just leaves you with a
white rectangle. Work down this list; the first two cover most cases.

### "Update display" finished instantly and nothing flashed

That is correct behaviour, not the old fault. E-paper holds its image with no
power, so redrawing pixels that are already on the glass buys nothing and just
costs a 2.4 s flash. The firmware hashes the rendered framebuffer and skips the
refresh when it matches what was last drawn; the API returns
`{"ok":true,"changed":false}` and the web UI says *"no change — the panel already
shows this"*.

The hash covers the rendered image, not the submitted text, so the same string at
a different font size still redraws. If the UI reports a change but the glass
does not follow, that is the real fault — see the next section.

### Ghosting, grey text, or "Update display" doing nothing

All three are the same fault: **a partial refresh running with the wrong
waveform**, and on this hardware they are unavoidable if the panel is powered
down after each draw. The giveaway is that *Clear panel* always works — that is
the forced full-refresh path.

GxEPD2's partial update assumes the panel is still powered from the previous
draw:

```cpp
void GxEPD2_213_BN::_Update_Part() {
  if (!_using_partial_mode) _Init_Part();   // writes the partial LUT via 0x32
  _PowerOn();                               // 0x22 0xf8 — bit 0x10 loads the OTP LUT
  _writeCommand(0x22); _writeData(0xcc);    // display
}
```

`_PowerOff()` clears both `_power_is_on` and `_using_partial_mode`. So after a
power-off, `_Init_Part()` writes the partial waveform and then `_PowerOn()`
*actually runs* and reloads the OTP waveform right over it. The update then
executes with a full-refresh LUT under a partial-refresh drive command: ink
comes out grey and under-driven, the previous image is not cleared, and
sometimes nothing visibly changes. Normally `_PowerOn()` is a no-op there, which
is why stock GxEPD2 examples work — they never drop power between updates.

`hibernate()` is worse still: it deep-sleeps the controller, so waking it
hardware-resets the chip and wipes the previous-image RAM (`0x26`) that a
partial refresh transitions *from*.

So on this controller, **partial refresh and powering the panel down after every
draw are mutually exclusive.** Pick one with `PANEL_KEEP_POWERED_MS`:

| Value | Behaviour |
|-------|-----------|
| `0` (default) | Panel powered off the instant a draw finishes, never left under drive voltage. Every update is a full refresh: ~2.4 s, flashes, always clean. |
| e.g. `30000` | Charge pump stays on for 30 s after a draw, so edits in quick succession get ~630 ms partial refreshes. The panel sits powered for that window. |

With a non-zero window, a draw arriving after the window has closed is
automatically promoted to a full refresh, because partial would be unsafe.

A full refresh is also forced on the first draw after boot, on `CLEAR`, and every
`FULL_REFRESH_EVERY` partial updates.

### QR codes that will not scan

`ricmoo/QRCode` does **not** check that a payload fits the version it is given.
`qrcode_initBytes()` returns an error only when it cannot choose an encoding
mode, never on overflow, so asking it for version 1 with 30 bytes of data
produces a structurally valid-looking but unscannable code with no error at all.

Do not write "try version 1, then 2, then 3 until it stops failing" — it never
fails. This firmware carries its own `QR_BYTE_CAPACITY` table and picks the
version from the payload length before calling the library. If you raise
`QR_MAX_VERSION`, extend that table to match.

Codes are also refused below `QR_MIN_SCALE` (2 px per module). At this panel's
~0.19 mm pitch a one-pixel module cannot be read by a phone, so an over-long
payload returns an error rather than drawing something useless.

### 1. Wrong GxEPD2 panel class

The most common cause. Every 2.13" revision uses a different controller init
sequence, and the wrong one silently does nothing. Change the single macro near
the top of `src/main.cpp`:

```cpp
#define EPD_PANEL_CLASS GxEPD2_213_BN     // Waveshare V4  (DEPG0213BN, SSD1680)
```

| Panel | Class |
|-------|-------|
| Waveshare 2.13" **V4** | `GxEPD2_213_BN` |
| Waveshare 2.13" **V3** | `GxEPD2_213_B74` |
| Waveshare 2.13" **V2** | `GxEPD2_213_B72` |
| Waveshare 2.13" **V1** | `GxEPD2_213` |
| flexible 2.13"         | `GxEPD2_213_flex` |
| Good Display GDEY0213B74 | `GxEPD2_213_B74` |

The revision is printed on the flat cable or the back of the panel. If it is
unmarked, try `GxEPD2_213_BN` then `GxEPD2_213_B74` — those two cover nearly
all panels sold today. Nothing else in the code has to change; panel dimensions
are derived from the class.

### 2. BUSY stuck high

If BUSY never goes low, GxEPD2 waits on it and gives up after its timeout, so
the refresh silently never happens. Watch the serial monitor: GxEPD2 prints
`_PowerOn : ... busy timeout!` or `busy Timeout!` when this happens.

- Confirm BUSY is on **GPIO4** and the wire is seated.
- Measure BUSY against GND: idle should be **low** (~0 V), going high only
  during a refresh. Permanently high means the panel is not being reset —
  check RST on GPIO3.
- A missing or broken **RST** wire looks exactly like this. So does a floating
  panel VCC.
- Some clone panels invert BUSY. If BUSY reads high at idle *and* the wiring is
  right, the panel needs a different class (see 1).

### 3. Swapped DC and CS

DC on GPIO5, CS on GPIO10 — green and orange. Swapping them is easy on a
crimped Waveshare cable and produces no error at all: every command byte gets
interpreted as data, the panel accepts it, and stays white.

Signs it is this: `pio device monitor` shows the refresh completing normally
and the BUSY line does toggle, but nothing appears. Verify by continuity from
each header pin to the panel connector rather than by eye — the cable's colour
order is not the same as the header order on every HAT revision.

Also check DIN/CLK are not swapped (blue = GPIO7, yellow = GPIO6); that failure
looks identical.

### 4. Driver HAT jumpers in the wrong position

On the Waveshare e-Paper Driver HAT:

- **Display config** jumper → **A**. Position B rewires the panel's on-board
  boost circuit for a different family; in B, a 2.13" panel gets no drive
  voltage and stays blank.
- **Interface config** jumper → **0** (4-wire SPI). Position 1 selects 3-wire
  SPI, where DC is folded into the data stream — the panel then ignores
  everything this firmware sends.

Both are silent failures with no serial output difference.

### 5. Power

Panel VCC must come from the board's **3V3** pin, not 5V. Tapping 5V can leave
the panel in an undefined state (or damage it) and shows as a blank or
half-drawn screen. Peak draw during refresh is 10–20 mA, well inside the
SuperMini regulator's budget — an external supply is not needed and adds a
ground-offset failure mode if its ground is not tied to the board's.

### The board won't transmit: invisible AP, or `reason 2` joining Wi-Fi

**This bit us hard, and it is the first thing to suspect on an ESP32-C3
SuperMini.** The symptoms look like three unrelated faults:

- the `eink-setup` AP never appears in any scan, even from 30 cm away
- joining your network fails with `[wifi] disconnected, reason 2`
  (`WIFI_REASON_AUTH_EXPIRE`) on every attempt
- `STATUS` nonetheless shows `ap_active=1`, a valid BSSID and `tx 80` (20 dBm)

They are one fault: **the board does not radiate cleanly at full transmit
power.** Measured on this hardware from 30 cm, with the SoftAP moved across the
band and power stepped down:

| Channel | 20 dBm | 15 dBm | 11 dBm | 8 dBm | 5 dBm |
|---------|--------|--------|--------|-------|-------|
| 1       | invisible | invisible | **95** | **95** | **92** |
| 6       | invisible | invisible | **95** | **95** | **87** |
| 11      | 100    | **100** | **100** | **100** | **90** |

At 20 dBm the current spike exceeds what the SuperMini's regulator and
decoupling can hold up, and the output is garbage — worst at the low end of the
band, where channels 1–6 vanish entirely. Receive is unaffected, which is why
`SCAN` happily lists every neighbour while nothing the board sends gets through.
That asymmetry is what makes it look like a wrong password.

The fix is `WIFI_TX_DBM` near the top of `src/main.cpp`, set to **11**:

```cpp
static const int8_t WIFI_TX_DBM = 11;
```

11 dBm is ~12 mW and still gave RSSI −39 at the router. If you have a board that
behaves at full power, raise it; if yours is worse, `TXPW:<dBm>` sets it live
over serial so you can sweep without reflashing, and `APCH:<n>` moves the SoftAP
channel the same way.

Independently of power, the setup AP ships on **channel 11** rather than the
arduino-esp32 default of 1, since a beacon on a channel occupied by a −30 dBm
neighbour is easy to miss. Use `SCAN` to see what is busy where you are.

### Reading `reason` codes

`[wifi] disconnected, reason N` distinguishes causes that otherwise look alike:

| Reason | Meaning | Usually |
|--------|---------|---------|
| 2      | `AUTH_EXPIRE` | no reply to authentication — **transmit problem**, or a router ignoring the client |
| 15 / 202 / 204 | handshake / auth failure | genuinely the wrong password |
| 201    | `NO_AP_FOUND` | wrong SSID, or a 5 GHz-only network |

The firmware treats 2, 15, 202, 204 and 205 as credential failures: it stops the
STA and holds an AP-only radio so the setup portal stays reachable. Reason 201
and plain link drops keep retrying with backoff, since those can recover on
their own.

### Provisioning Wi-Fi without the AP

If the setup AP is unreachable for any reason, credentials can be set straight
over USB serial — no captive portal needed:

```
WIFI:<ssid>,<password>
```

Split on the first comma only, so passwords may contain commas (SSIDs may not).
The device saves to NVS and reboots.

### Log lines that are *not* problems

```
[E][esp32-hal-spi.c:227] spiAttachMISO(): SPI Does not have default pins on ESP32C3!
```

Expected, once per boot. MISO is deliberately `-1` (e-paper is write-only) and
arduino-esp32 logs this to say there is no default MISO to fall back to. The
only way to remove it is to hand SPI a real pin we don't need.

```
_PowerOn : 95999
_Update_Part : 450001
_PowerOff : 140001
```

GxEPD2 timing diagnostics, in **microseconds** (not milliseconds). Healthy
values on this panel: power-on ~96 ms, partial refresh ~450 ms, full refresh
~2130 ms. **Values near zero mean the BUSY line never went busy** — normal with
no panel attached, since GPIO4 floats low, but with the panel wired it means
BUSY is not reaching GPIO4. See section 2.

### 6. Nothing at all on serial

If `pio device monitor` shows no boot banner, the firmware is not running:
check `-D ARDUINO_USB_CDC_ON_BOOT=1` is present in `platformio.ini`, and that
no wire is pulling **GPIO9** low at power-up — that puts the C3 into download
mode, where it sits forever.
