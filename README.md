# Wi-Fi e-ink label

A **2.13" e-paper name tag you retype from your phone.** Plug it into any USB-C
port, open a page, type, tap print. The panel holds the image with no power, so
it keeps showing your text whether or not the Wi-Fi, the router, or the firmware
is still alive.

Firmware for an **ESP32-C3 SuperMini** driving a **Waveshare 2.13" e-Paper V4**
(SSD1680 / DEPG0213BN, 250 × 122 px). About $12 of parts and two hours.

```
┌──────────────────────────────┐
│                              │   phone ──http──►  eink.local
│      MEETING  IN  PROGRESS   │                        │
│         back at 15:30        │                        ▼
│                              │              250 × 122 · 1-bit · 2.4 s
└──────────────────────────────┘
```

**Features**

- **Web UI served from PROGMEM** — no CDN, no npm, no internet. One HTML string.
- **Five modes, two rendered on the device and three by your phone** — TEXT
  (word-wrapped, 3 sizes) and QR (auto version + scaling) render in firmware;
  UNICODE (emoji and any script), IMAGE (any picture, dithered to 1 bit) and
  DRAW (finger sketching) are rasterised by the browser.
  [What that changes](#where-each-mode-renders).
- **Pixel-exact preview** before you spend a refresh — the device renders into a
  scratch buffer and ships the real bitmap back to the browser.
- **Content survives reboots** — stored in NVS, redrawn once on boot.
- **Zero-config networking** — captive-portal setup AP, mDNS, exponential
  backoff, and a rescue AP if the saved network stops working.
- **QR codes sized for the panel, not for a spec sheet** — the cell size and
  error-correction level are chosen together, and an optional caption fills the
  half of the display a square code can never reach.
- **Recent list** — the handful of things a label cycles between, one tap away.
- **Firmware updates over Wi-Fi**, once you have mounted it somewhere a USB
  cable will not reach.
- **USB serial fallback** — `TEXT:`, `QR:`, `QRC:`, `CLEAR`, plus Wi-Fi
  provisioning without ever touching the portal.
- **Panel-safe** — never left under drive voltage in an idle state.

**Contents**

[Hardware](#hardware) ·
[Wiring](#wiring) ·
[Build and flash](#build-and-flash) ·
[Wi-Fi setup](#first-time-wi-fi-setup) ·
[Using it](#using-it) ·
[QR sizing](#how-a-qr-code-is-sized) ·
[Burn-in](#does-e-paper-burn-in) ·
[Firmware over Wi-Fi](#updating-over-wi-fi) ·
[HTTP API](#http-api) ·
[Layout](#project-layout) ·
[Field notes](#field-notes) ·
[Troubleshooting](#troubleshooting)

---

## Hardware

| Part | Notes |
|------|-------|
| ESP32-C3 SuperMini | Any C3 board works; the pin map below is for the SuperMini |
| Waveshare 2.13" e-Paper **V4** | SSD1680 controller, 250 × 122. [Other revisions need a one-line change](#1-wrong-gxepd2-panel-class) |
| Waveshare e-Paper Driver HAT *(optional)* | Or wire the FPC connector directly |
| USB-C cable | Data, not charge-only — the same cable flashes and powers it |

No battery, no level shifters, no external supply. Peak draw during a refresh is
10–20 mA, well inside the SuperMini regulator's budget.

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

The panel is mounted cable-to-the-left by default. Flip it with `PANEL_ROTATION`
in `src/main.cpp`: `1` is cable-right, `3` is cable-left. Only the blit is
rotated, so the canvas, the stored image and the browser preview are unchanged.

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

A prebuilt image is committed at [`firmware/firmware.bin`](firmware/firmware.bin)
if you would rather just [upload it](#updating-over-wi-fi) than build.

Then, from the project root:

```bash
pio run                 # build
pio run -t upload       # build + flash over USB-C
pio device monitor      # 115200 baud, native USB CDC
```

Everything is pinned in [`platformio.ini`](platformio.ini) — GxEPD2, Adafruit
GFX and ricmoo/QRCode are fetched automatically. Current footprint:

```
RAM:    15.4%  (50,308 / 327,680 bytes)
Flash:  65.8%  (861,974 / 1,310,720 bytes)
```

The C3's native USB is USB Serial/JTAG, so a plain USB-C cable both flashes and
carries `Serial`. That works because `platformio.ini` sets
`-D ARDUINO_USB_CDC_ON_BOOT=1`.

If upload fails to find the port, force download mode once: hold **BOOT**, tap
**RESET**, release **BOOT**, then `pio run -t upload`. On Linux, a
`Permission denied: /dev/ttyACM0` means the udev rules are missing —
`curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules`
then replug.

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

> **If the AP never appears, or joining fails with `reason 2`,** it is almost
> certainly not your password — see
> [the board won't transmit](#the-board-wont-transmit-invisible-ap-or-reason-2-joining-wi-fi).
> This is the single most likely thing to bite you on a SuperMini.

---

## Using it

**Web UI** — open the label's address:

- text field for the content
- **TEXT / QR / UNICODE / IMAGE / DRAW** tabs, each with exactly one input
- font size small / medium / large
- **Preview** — renders and shows the result in the page *without touching the
  panel*, so you can check the wrap, the truncation or the QR size before
  spending a 2.4 s refresh. The button then becomes **Print to panel**; editing
  any field marks the preview out of date and it reverts to **Preview**.
- **Print now** — skips the preview and draws immediately
- **Clear panel**
- the preview box is outlined while it shows an unprinted preview

Preview runs the real renderer into a separate scratch buffer, so what you see
is the exact bitmap a print would produce — and payloads that are too long are
rejected at preview time rather than after a wasted refresh.

Submitting content identical to what is already displayed is a no-op: the panel
is left alone and the UI says so, rather than flashing for 2.4 s to draw the
same thing.

### Where each mode renders

The four tabs fall into two families, and almost every behavioural difference
between them follows from which family you are in.

|                       | TEXT | QR | UNICODE | IMAGE / DRAW |
|-----------------------|------|----|---------|--------------|
| **Rendered by**       | the device | the device | your browser | your browser |
| **Source**            | 5 × 7 ASCII table in flash | QR modules | your phone's whole font stack | a picture, or your finger |
| **Character range**   | ASCII 32–126, everything else becomes `?` | any bytes | anything Unicode | n/a |
| **Stored in NVS as**  | the string | the string | a 3904-byte frame | a 3904-byte frame |
| **On boot**           | re-rendered | re-rendered | blitted as-is | blitted as-is |
| **Font size applies** | yes | no | yes | no |
| **Preview costs**     | a round trip to the device | a round trip | nothing, the bitmap is local | nothing |
| **Over serial**       | `TEXT:` | `QR:` | — | — |
| **Remembered in [Recent](#recent)** | yes | yes | no | no |
| **Edges**             | crisp | crisp | dithered | dithered / crisp |

Three consequences are worth knowing before you pick a tab:

- **Device-rendered content keeps its meaning; browser-rendered content keeps
  its pixels.** A stored string is laid out again every boot, so changing the
  font or `MAX_LINES` and reflashing re-flows existing TEXT content. A stored
  frame never changes — there is no text left in it to re-flow.
- **Only device-rendered modes work headlessly.** `TEXT:` and `QR:` over USB
  need no browser; UNICODE and IMAGE have no serial equivalent, because the
  serial protocol has no way to carry a frame.
- **Only browser-rendered modes can show emoji or Hangul** — the device simply
  has no glyphs for them.

The preview asymmetry follows from the same split. For TEXT and QR the browser
cannot know what the panel would draw, so it asks the device to render into a
scratch buffer and send the bitmap back; that is what makes the preview exact
rather than an approximation. For UNICODE and IMAGE the browser drew the bitmap
in the first place, so the preview is simply that bitmap, shown instantly.

**Serial** — one command per line, at 115200 baud (CR, LF or CRLF):

```
TEXT:Hello from the desk
QR:https://example.com/thing
QRC:Guest Wi-Fi|https://example.com/thing     code plus a caption
CLEAR
RECENTS                    list what is remembered
RECENTS:CLEAR              forget it
OTAPW:<password>           enable firmware upload over Wi-Fi
OTAPW:                     disable it again
WIFI:<ssid>,<password>     save credentials and reboot
FORGET                     clear credentials, reboot into the setup AP
RECONNECT                  force an immediate retry, re-enabling the STA
STATUS                     radio + content state, incl. stored SSID
SCAN                       list nearby 2.4 GHz networks
APCH:<1-13>                move the SoftAP channel, live
TXPW:<2-20>                set transmit power in dBm, live
```

`TEXT:` uses the font size last chosen in the web UI. `QRC:` splits on the
**first** bar only, so the payload may contain one; the caption may not. The
device answers `OK` or `ERR <reason>`.

### Limits and rejections

Oversized payloads are refused with a message rather than drawn as garbage:

| Mode | Limit |
|------|-------|
| TEXT | 400 characters (`MAX_TEXT_LEN`). Text that wraps past the panel height is truncated with `...` |
| QR   | **271 bytes.** Shorter payloads get larger cells — see [how a QR code is sized](#how-a-qr-code-is-sized) |
| UNICODE / IMAGE / DRAW | exactly 3904 bytes once decoded (250 × 122, 1 bit per pixel). Anything else is rejected |

### How a QR code is sized

Two things decide whether a phone can read the code, and they pull against each
other.

**Cell size.** The grid is drawn a whole number of pixels per cell — there is no
such thing as a 2.97-pixel cell — so the size moves in steps and everything left
over is wasted. This is the dominant factor.

**Error correction.** A QR code stores its message with redundancy woven
through, so a scanner can rebuild it when part is unreadable. Four strengths
exist — Low, Medium, Quartile, High, surviving roughly 7 / 15 / 25 / 30 % of the
code being lost. The redundancy needs cells to live in, so stronger correction
means more cells, and on a fixed 122 px panel, smaller ones.

The firmware evaluates all four strengths, takes whichever yields the **largest
cell**, and breaks ties toward the **stronger correction**. Because of the
whole-pixel rounding, stronger correction is frequently free: the space it needs
was being discarded anyway.

| payload | grid | correction | cell | code |
|---------|------|-----------|------|------|
| ≤ 14 chars | 25 × 25 | High | 4 px (0.78 mm) | 100 px |
| 30 chars | 25 × 25 | Low | 4 px | 100 px |
| 60 chars | 33 × 33 | Medium | 3 px (0.58 mm) | 99 px |
| 80 chars | 49 × 49 | High | 2 px (0.39 mm) | 98 px |
| 230 chars | 53 × 53 | Low | 2 px | 106 px |
| 271 chars | 57 × 57 | Low | 2 px | 114 px |

One pixel per cell is 0.19 mm and no phone can read it, so `QR_MIN_SCALE` refuses
it rather than drawing something useless.

**The white border.** A scanner needs white around the code. ISO 18004 asks for
4 cells, and reserving that up front is what used to make codes small: 33 cells
plus 4 plus 4 is 41, and 122 ÷ 41 = 2.97 → **2 px per cell**, leaving a border
that actually measured **14 cells**. It reserved 4, wasted 14, and halved the
code to do it.

`QR_QUIET_MIN` is therefore **2**. The same code then takes 3 px per cell and
still ends up with a 3.7-cell border. Tested against a simulated phone camera,
4, 3 and 2 cells decode alike and only 1 begins to fail; a dark surround behind
the panel is no worse than its white bezel, since it gives the detector a firmer
edge. So this does not depend on how the label is mounted.

**One exception, in the band where it matters.** Whether the border or the cell
size is the binding constraint depends on which one the scanner is struggling
with, and the two swap places:

| payload | border 2 | border 1 | modelled scan range |
|---------|----------|----------|---------------------|
| 12 chars | 4 px cells, 100 px | 5 px cells, 105 px | 92 → **74 cm** ✗ |
| 82 chars | 2 px cells, 98 px | 3 px cells, 111 px | 41 → **57 cm** ✓ |

At 4 px per cell the scanner resolves cells easily and what limits it is finding
the code's edge, so a narrower border costs range outright. At 2 px it is
struggling to resolve cells at all, and a whole extra pixel per cell dwarfs
anything the border does.

So `QR_QUIET_RESCUE` (1 cell) is used **only when it lifts a code off the 2 px
floor** — payloads of roughly 79–106 characters. Across all 271 lengths that
rescues 28 and makes none worse.

The same 85-character payload through three generations of this firmware:

| | code | cell | border | modelled | on the bench |
|---|---|---|---|---|---|
| ECC fixed at Medium, border 4 | 82 px | 0.39 mm | 10 cells | 32 cm | — |
| all four ECC levels, border 2 | 106 px | 0.39 mm | 4 cells | 40 cm | — |
| with the rescue | **111 px** | **0.58 mm** | 1.7 cells | 52–56 cm | **45 cm** |

Net effect on a 60-character link: **66 px → 99 px**.

### How far these numbers can be trusted

The scan distances above come from a simulated camera — the panel frame
projected at a given distance for a 12 MP sensor at 70°, softened, given sensor
noise, then decoded, bisecting for the range that still reads. It is a model,
and it is optimistic. Checked against a phone on a desk:

| layout | modelled | measured | ratio |
|--------|----------|----------|-------|
| v10, 2 px cells, 2-cell border | 34 cm | **30 cm** | 0.88 |
| v5, 3 px cells, 1-cell border | 52–56 cm | **45 cm** | 0.83 |

**Scale anything modelled here by roughly 0.85.** The ordering has held every
time — every layout the model ranked higher measured higher — so it is sound for
choosing between options, and only the absolute figures need discounting.

Worth stating plainly, since the 1-cell border is two below what ISO 18004 asks
for: it was validated on hardware, not just in the model. A 271-byte code at the
tightest layout the firmware can produce scans at 30 cm, and the photograph of
that panel decodes on its own.

When a code does end up at 2 px per cell — above about 107 characters, where
nothing can be done — the page says so under the preview, because that is the
point where shortening the payload is the only remaining fix.

### Captions

A square code can never be taller than the 122 px panel height, so on a 250 px
wide display **more than half the glass is unusable in QR mode by construction**.
Fill it:

```
QRC:Guest Wi-Fi|WIFI:T:WPA;S:MyNetwork;P:hunter2;;
```

The code takes a 122 px square on the left, the caption gets the strip that was
going to be blank. Caption text is auto-fitted — size 3, then 2, then 1, taking
the first that does not need truncating — and is limited to 60 characters. The
web UI shows a caption field whenever the QR tab is selected.

Because the code is confined to the left square, its sizing is unchanged: height
was always the binding dimension.

### Recent

The last `RECENTS_MAX` (6) things displayed are kept in NVS and listed under the
form. Tapping one loads it into the fields **and previews it** — it never prints
straight off a tap, because a misfire would cost a 2.4 s refresh and the preview
leaves the panel alone. The list de-duplicates, so re-showing something moves it
to the top rather than filling the list with copies.

**TEXT and QR only.** A browser-rendered frame is 3904 bytes and the whole NVS
partition is 20 KB, so six of them could not fit; the UNICODE, IMAGE and DRAW
tabs are not remembered.

### Updating over Wi-Fi

Once the label is mounted somewhere, reaching its USB port stops being
convenient. `http://<label>/update` takes a `firmware.bin` from
`.pio/build/esp32-c3-supermini/` and reboots into it.

**It is off until you set a password**, over USB serial:

```
OTAPW:<password>          enable, user "admin"
OTAPW:                    disable again
```

That is deliberate. Every other endpoint on this server can only change what the
panel shows; this one replaces the running code, which turns "anyone on the
Wi-Fi can change my label" into "anyone on the Wi-Fi can run their own code on a
device inside my network". A default password would be worse than none, because
nobody would change it — so there isn't one, and the endpoint answers 403 until
you choose. Credentials are checked when the upload *starts*, not after it
finishes, so an unauthenticated caller is refused before streaming a megabyte.

Measured on this hardware: 896 KB uploaded and flashed in **4 s**, back online
**3 s** later. A corrupt or truncated image is rejected and nothing is
overwritten — the partition table carries two 1.25 MB app slots, and the running
one is not touched until the new image verifies. Saved Wi-Fi, content and the
recent list all live in NVS, a different partition again, so they survive.

### Knowing what is running

Every build is stamped with a UTC timestamp and the commit it came from:

```
2026-09-13 14:22Z 831d246
```

It appears in the boot banner, in `STATUS`, at the foot of the main page, and in
`/api/status` as `build`. The `/update` page shows the running build above the
file picker, so reloading it after a reboot tells you whether the upload took —
which otherwise looks identical to one that silently failed.

A `+` suffix means the **source** had uncommitted changes when it was built, so
the hash alone does not describe what is running. `firmware/` is excluded from
that check — copying a freshly built image into the tree would otherwise mark
the next build dirty, and every shipped binary would carry a meaningless `+`. The hash is HEAD *at build time*,
so a binary committed afterwards names the commit its source came from, not the
commit containing the binary.

### Flashing from a shell

```bash
curl -u admin:<password> -F "firmware=@.pio/build/esp32-c3-supermini/firmware.bin" \
     http://eink.local/update
```

### Draw

A 250 × 122 canvas with a pen, an eraser, undo and a brush size. The backing
store is the panel's own resolution, so the pixels you touch are literally the
pixels that get sent — there is no resampling step to soften or shift a stroke,
and the preview is the drawing itself.

**Line art is thresholded, not dithered.** Everything else browser-rendered goes
through Floyd–Steinberg, which is right for a photo or a coloured emoji but
wrong here: a pen stroke is solid black with antialiased edges, and diffusing
those edges speckles every line. Measured on a synthetic stroke, thresholding
gives 3.5× fewer black/white transitions along it — the difference between a
line and a dotted line. A uniform grey area shows it most starkly: dithered it
becomes 30,000 transitions of checkerboard, thresholded it is solid.

The sketch is kept in `localStorage` so a reload does not lose it. That is
per-browser and never leaves your phone; the panel only ever receives the
finished frame.

Because DRAW and IMAGE both store as a bare frame, the device cannot tell them
apart on reload — the page remembers which tab you were last on and returns you
to it.

### Does e-paper burn in?

Not the way OLED does. OLED burn-in is emissive material ageing at different
rates per pixel, and it is permanent. E-paper has no emissive layer — nothing is
being driven continuously, and a static image costs no power at all.

It does have a milder relative. A pattern left in one place for a long time can
leave a faint residual image as the pigment settles, and an area switched
repeatedly wears slightly differently from one that never changes. Unlike OLED
burn-in this is usually recoverable: several full black-to-white refreshes clear
it. Panel makers ask for a refresh at least once every 24 hours and warn against
leaving a static image indefinitely, especially when warm.

Two things here address it:

- **Every update is a full refresh** by default (`PANEL_KEEP_POWERED_MS = 0`),
  which is the same black-to-white cycle used to clear retention.
- **QR codes are placed with a small pseudo-random offset**, up to
  `QR_JITTER_PX` (8 px) from centre on each axis, so successive codes do not
  land on exactly the same pixels.

Jitter is bounded by the *comfortable* border (`QR_QUIET_MIN`), never by
whatever the plan settled for. On a rescued code the two collide — there is not
room for 2 cells on both sides — and the axis is centred instead. Letting jitter
spend the rescued border hands back the range the bigger cell just bought: an
85-character code pushed to a 1.0-cell margin modelled at 34 cm against a dark
surround, where centred at 1.7 cells it models at 52 cm and measures 45 cm.

This one was caught on hardware, not in review: the first build of the rescue
let jitter spend the border it had just bought back, landing that code *below*
the version it replaced.

The offset comes from the payload rather than a random number generator, so the
same content always lands in the same spot. If it moved on every draw the
[unchanged-image check](#update-display-finished-instantly-and-nothing-flashed)
could never fire and every repeat print would cost a full 2.4 s refresh. It is
also bounded rather than filling the available slack — a code wandering 70 px
across the panel reads as a bug, not as care.

**The honest limit:** jitter spreads wear across *content changes*. A label that
displays one code untouched for six months gets no benefit from it, because
nothing redraws. If that is your use, a periodic self-refresh on a timer is the
mitigation that would actually help — it is not implemented.

### Emoji, Hangul and pictures

The device has no glyph data beyond a 5 × 7 ASCII table, so **TEXT mode folds
anything outside ASCII 32–126 to `?`** — emoji, Hangul, accented Latin. QR mode
is unaffected, since it encodes raw bytes.

Three tabs sidestep the problem by rasterising in the browser, which already has
every font and emoji on the phone:

- **UNICODE** — type anything, including emoji and Hangul. The page lays it out
  with your phone's own fonts at the chosen size.
- **IMAGE** — pick a picture. Scaled to fit and centred.
- **DRAW** — sketch with your finger. See [below](#draw).

Each tab has exactly one input, so there is nothing to reset when switching. The
result goes onto a 250 × 122 canvas, is converted to 1 bit with Floyd–Steinberg
dithering, and posted as a base64 3904-byte frame. The device decodes, blits and
stores it in NVS, so it survives a reboot. Nothing is re-rendered on the device,
and preview for these tabs is instant because the bitmap already exists in the
browser.

Both tabs store the same device-side mode. The source string tells them apart on
reload — IMAGE never sends one.

Dithering rather than a hard threshold matters here: a bright yellow emoji is
high-luminance and would simply disappear under a threshold, but becomes a
recognisable dot pattern when the error is diffused. Plain black text has no
error to diffuse, so it stays crisp.

Three things to expect:

- **The panel is 1 bit, so emoji arrive as silhouettes.** High-contrast
  pictograms (✓ ★ ♥ ⚠ ↑) read well. Detailed or pale ones often do not.
- **UNICODE and IMAGE are browser-only.** The serial protocol has no way to send
  a frame, so `TEXT:` and `QR:` remain ASCII-only.
- **Plain TEXT is still worth using for ASCII.** The device's 5 × 7 bitmap font
  is crisper on a 1-bit panel than antialiased browser text dithered down to it,
  and its content round-trips as a string rather than a frame.

To render Hangul from the device itself instead, you would need an Adafruit GFX
bitmap font containing those glyphs; note that `renderTextToCanvas()` wraps on a
fixed 6 × 8 cell and would need reworking for a proportional font.

---

## HTTP API

Everything the web UI does is a plain form POST, so `curl` works just as well.

| Method | Path | Body | Returns |
|--------|------|------|---------|
| `GET`  | `/` | | the web UI (redirects to `/wifi` until provisioned) |
| `GET`  | `/wifi` | | the setup page |
| `GET`  | `/api/status` | | mode, text, size, panel size, update count, SSID/IP/RSSI, and the committed frame as base64 |
| `GET`  | `/api/scan` | | up to 20 nearby networks |
| `POST` | `/api/preview` | `mode=text\|qr`, `text`, `caption`, `size=1..3` | the rendered frame — **panel untouched** |
| `POST` | `/api/display` | `mode=text\|qr`, `text`, `caption`, `size=1..3` | `{"ok":true,"changed":bool}` |
| `POST` | `/api/image` | `bits` (base64, 3904 bytes), `text` (optional source string) | as above |
| `POST` | `/api/clear` | | as above |
| `POST` | `/api/recents` | `clear=1` | the recent list |
| `GET` / `POST` | `/update` | firmware upload, password-protected | see below |
| `POST` | `/api/wifi` | `ssid`, `pass` | saves and reboots |

```bash
curl -s -X POST http://eink.local/api/display --data-urlencode 'mode=text' \
     --data-urlencode 'text=back at 15:30' --data 'size=2'
# {"ok":true,"changed":true}
```

Errors come back as HTTP 400 with `{"ok":false,"error":"..."}`. `changed:false`
means the frame was identical to what the glass already holds and no refresh was
performed.

Anything that draws a QR code also reports how it was sized:

```json
"qr": {"version":4,"ecc":"Medium","modules":33,"scale":3,"side":99,"mm":"0.58"}
```

---

## Project layout

```
platformio.ini        board, USB-CDC flag, library pins
scripts/build_id.py   stamps each build with a timestamp and commit
src/main.cpp          pin map, panel class, rendering, Wi-Fi, HTTP, serial
src/web_ui.h          the three HTML pages as PROGMEM strings
firmware/firmware.bin prebuilt image, for uploading over the air
```

Tunables at the top of `main.cpp`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `EPD_PANEL_CLASS` | `GxEPD2_213_BN` | [panel revision](#1-wrong-gxepd2-panel-class) |
| `PANEL_ROTATION` | `3` | which way up — `1` puts the ribbon cable on the right, `3` on the left |
| `WIFI_TX_DBM` | `11` | [transmit power](#the-board-wont-transmit-invisible-ap-or-reason-2-joining-wi-fi) |
| `PANEL_KEEP_POWERED_MS` | `0` | [full vs partial refresh](#ghosting-grey-text-or-update-display-doing-nothing) |
| `FULL_REFRESH_EVERY` | `10` | partial updates between forced full ones |
| `MAX_TEXT_LEN` | `400` | TEXT payload cap |
| `QR_MAX_VERSION` | `10` | largest grid, 57 × 57 cells |
| `QR_QUIET_MIN` | `2` | [white border, in cells](#how-a-qr-code-is-sized) |
| `QR_QUIET_RESCUE` | `1` | [narrower border, only to escape the 2 px floor](#how-a-qr-code-is-sized) |
| `QR_JITTER_PX` | `8` | [anti-retention offset](#does-e-paper-burn-in) |
| `RECENTS_MAX` | `6` | how many past items to remember |
| `AP_SSID` / `AP_CHANNEL` / `HOSTNAME` | `eink-setup` / `11` / `eink` | networking |

---

## Field notes

Three findings cost most of the build time. If you are writing your own
firmware for this hardware, these are the ones to know:

1. **The SuperMini does not radiate cleanly at the default 20 dBm.** Receive is
   fine, so scans work and it looks like a wrong password. Drop to 11 dBm.
   → [details](#the-board-wont-transmit-invisible-ap-or-reason-2-joining-wi-fi)
2. **Partial refresh and powering the panel down after each draw are mutually
   exclusive on the SSD1680.** GxEPD2's `_PowerOn()` reloads the OTP waveform
   over the partial LUT that `_Init_Part()` just wrote. Result: grey ink,
   ghosting, and updates that do nothing. → [details](#ghosting-grey-text-or-update-display-doing-nothing)
3. **`ricmoo/QRCode` never reports payload overflow.** "Try version 1, then 2,
   until it stops failing" silently produces unscannable codes forever.
   → [details](#qr-codes-that-will-not-scan)

---

## Troubleshooting

An e-paper panel never reports errors — a wrong setting just leaves you with a
white rectangle. Work down this list; the first two cover most cases.

### "Update display" finished instantly and nothing flashed

That is correct behaviour, not a fault. E-paper holds its image with no power,
so redrawing pixels that are already on the glass buys nothing and just costs a
2.4 s flash. The firmware hashes the rendered framebuffer and skips the refresh
when it matches what was last drawn; the API returns
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

First check what the code actually is. The web UI prints its sizing under the
preview, and the serial log says the same thing:

```
[qr] v4 Medium 33x33 modules, 3 px/module, 99 px, at 69,6
```

**At 2 px per module (0.39 mm) the code is at its readable limit.** That happens
above roughly 78 characters, and the fix is a shorter payload, not a setting —
a link shortener buys a whole step. Below about 43 characters you get 3 px, and
below 15 you get 4 px.

**A library trap worth knowing if you adapt this code.** `ricmoo/QRCode` does
not check that a payload fits the version it is given. `qrcode_initBytes()`
returns an error only when it cannot choose an encoding mode, never on overflow,
so asking it for version 1 with 30 bytes produces a structurally valid-looking
but unscannable code with no error at all.

Do not write "try version 1, then 2, then 3 until it stops failing" — it never
fails. This firmware carries its own `QR_BYTE_CAPACITY` table and picks the
version from the payload length before calling the library. If you raise
`QR_MAX_VERSION`, extend that table to match.

### The display stays blank

#### 1. Wrong GxEPD2 panel class

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

> Note when adapting this: use `WIDTH_VISIBLE` (122 on this panel), not `WIDTH`
> (128, the controller's RAM width), or your canvas is six rows too tall and
> vertical centring drifts.

#### 2. BUSY stuck high

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

#### 3. Swapped DC and CS

DC on GPIO5, CS on GPIO10 — green and orange. Swapping them is easy on a
crimped Waveshare cable and produces no error at all: every command byte gets
interpreted as data, the panel accepts it, and stays white.

Signs it is this: `pio device monitor` shows the refresh completing normally
and the BUSY line does toggle, but nothing appears. Verify by continuity from
each header pin to the panel connector rather than by eye — the cable's colour
order is not the same as the header order on every HAT revision.

Also check DIN/CLK are not swapped (blue = GPIO7, yellow = GPIO6); that failure
looks identical.

#### 4. Driver HAT jumpers in the wrong position

On the Waveshare e-Paper Driver HAT:

- **Display config** jumper → **A**. Position B rewires the panel's on-board
  boost circuit for a different family; in B, a 2.13" panel gets no drive
  voltage and stays blank.
- **Interface config** jumper → **0** (4-wire SPI). Position 1 selects 3-wire
  SPI, where DC is folded into the data stream — the panel then ignores
  everything this firmware sends.

Both are silent failures with no serial output difference.

#### 5. Power

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
band and power stepped down (numbers are `nmcli` signal strength; *invisible*
means the beacon was not seen at all):

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

Those reason codes are not trustworthy on their own. **2 and 15 are not
exclusively "wrong password"** — a marginal signal produces them too. This board
taught us that directly: reason 2 on a perfectly correct password, because it
[was not radiating cleanly](#the-board-wont-transmit-invisible-ap-or-reason-2-joining-wi-fi)
at 20 dBm.

So the firmware does not decide from the reason code alone. It records in NVS
whether the stored credentials have **ever completed a connection**:

| | reason 2 / 15 / 202 / 204 / 205 | reason 201, plain link drops |
|---|---|---|
| **never connected** | almost certainly a typo — STA off, AP-only, so the portal is reachable | retry forever with backoff |
| **has connected before** | must be interference — retry forever, AP raised alongside | retry forever with backoff |

A credential that has worked is never "wrong". Without that distinction a
distant, congested or rebooting router can park a working label in AP-only mode
until someone power-cycles it — fine on a desk, useless on a wall.

The flag is cleared on every credential change, so a genuinely mistyped password
still lands you in the setup portal on the first attempt rather than inheriting
the previous one's good standing. `STATUS` over serial reports it as
`creds_proven`.

### Provisioning Wi-Fi without the AP

If the setup AP is unreachable for any reason, credentials can be set straight
over USB serial — no captive portal needed:

```
WIFI:<ssid>,<password>
```

Split on the first comma only, so passwords may contain commas (SSIDs may not).
The device saves to NVS and reboots.

### Nothing at all on serial

If `pio device monitor` shows no boot banner, the firmware is not running:
check `-D ARDUINO_USB_CDC_ON_BOOT=1` is present in `platformio.ini`, and that
no wire is pulling **GPIO9** low at power-up — that puts the C3 into download
mode, where it sits forever.

If the banner appears as garbage, the monitor is at the wrong baud — run
`pio device monitor` **from the project directory** so it reads `monitor_speed`
from `platformio.ini`.

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
BUSY is not reaching GPIO4. See [BUSY stuck high](#2-busy-stuck-high).

---

## Built with

[GxEPD2](https://github.com/ZinggJM/GxEPD2) ·
[Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) ·
[ricmoo/QRCode](https://github.com/ricmoo/QRCode) ·
[PlatformIO](https://platformio.org/) ·
[arduino-esp32](https://github.com/espressif/arduino-esp32)
