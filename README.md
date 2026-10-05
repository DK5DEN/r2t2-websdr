# r2t2-websdr

Multi-user WebSDR for the DARC **R2T2** receiver (Zynq Z7020, dual 14-bit ADC, 122.88 MHz),
running directly on the device. No additional computer is required.

## How it works

The R2T2 FPGA contains eight digital down-converters. Each one delivers two streams over the
internal `rad0` interface (Ethertype `0x7232`):

| Stream | Rate per receiver | Used for |
|---|---|---|
| narrow (tag 1) | 16 kS/s | not used (see below) |
| wide (tag 2) | 192 kS/s | waterfall and all audio |

`r2t2sdr` treats the eight receivers as a pool:

- a waterfall (view) occupies one receiver, shared by all its viewers. A configured band is
  widened over up to four receivers side by side to its band edges while receivers are free
  (20 m: three receivers, 14.000 to 14.350 MHz); the browser composes the segments into one
  waterfall. Extra segments are the first thing given back when someone needs a receiver.
- a listener inside any active waterfall is cut out of that wide stream by the CPU and costs
  no receiver. Only a listener outside every waterfall (or on another antenna) gets a
  receiver of its own; its wide stream is used like a private segment, with the NCO 20 kHz
  off the listener (away from the DC spur) and moved only when the listener leaves +-60 kHz.

The narrow stream is ignored: in this FPGA build it does not match its receiver (correlated
against the wide stream of every receiver, decimated to 16 kS/s, in both I/Q orders: no
match; listening on it gave wrong pitches). `tools/streamcap.c` shows frame rates, tags and
these correlations.

Listeners are cut out by fast convolution (overlap-save), in two stages:

1. per segment one 6144-point FFT over the wide stream (4608 new samples per window), shared
   by all listeners on it; each listener takes the 512 bins around its frequency, weighted by
   an anti-alias low-pass (-6 dB at 7.5 kHz), and a 512-point inverse FFT gives 384 samples
   at 16 kS/s; a phase correction per window and a residual shift below one bin (31.25 Hz)
   keep the mixing continuous;
2. the channel filter of the demodulator (up to 641 taps) as a 1024-point FFT convolution,
   384 new samples per block.

Neither stage depends on the filter width.

Receivers go by priority: station > logged-in user > guest. A client may take a receiver from
a holder of lower priority: guests first, and among the holders of the lowest priority the one
inactive for the longest time (last command other than the keep-alive ping; a shared waterfall
counts as active as its most active viewer or listener). The holder keeps the connection, loses
that waterfall (or its own receiver) and gets a notice. The listener limit works the same way. Every status message lists the active waterfalls, so the page
marks the bands where listening costs no receiver and, when all receivers are busy, offers
them as buttons.

CPU (two Cortex-A9 cores at 667 MHz), measured with all 8 receivers on waterfalls: 40 % of
one core without listeners, about 7 % per segment that has listeners (its FFT), and about
3.6 % per listener (0.8 % its part of the segment FFT, 2.7 % channel filter and
demodulator, 0.1 % ADPCM), the same for every filter width. Listeners run on two threads,
one per core; a segment belongs to thread `rx % 2`, so its FFT is computed once. 30
listeners keep full audio at 157 % of one core (of 200 %); `max_listeners` defaults to 30,
`max_clients` to 48 (64 at most). A higher-priority listener bumps the lowest one when the
limit is reached. The waterfall averages two FFTs per line; a receiver that only feeds a
waterfall does not unpack the samples of the rest of the line period.

Before the fast convolution a listener cost 7.5 % (15 % with a 250 Hz filter) and 16
listeners dropped packets.

Filters: SSB 2.8, 2.4, 1.8, 1.2 kHz and 500 or 250 Hz around 1.5 kHz audio (digital modes);
CW 1 kHz, 500, 250, 100 Hz; AM 12, 9, 6 kHz; FM 15, 12, 8 kHz. The channel filter grows with
narrower passbands (161 taps from 1.7 kHz, 241, 401, 641 below 400 Hz; skirts ~550 down to
~140 Hz); with the FFT convolution the length costs nothing extra.

Antenna: a listener may choose an input instead of the one the antenna ranges give. If the
waterfall's receiver is on that input, the audio still comes from it; otherwise the listener
gets a receiver of its own on the chosen input (by priority). The shared waterfall is not
switched.

Audio is demodulated on the device (USB, LSB, CW, AM, FM with AGC and squelch), resampled to
8 kHz, IMA-ADPCM compressed (~32 kbit/s) and sent over a WebSocket. The browser plays it with
the Web Audio API (no AudioWorklet, so plain HTTP works).

## FPGA interface

Register map recovered from the 2017 `r2t2srv` binary. Registers are **write-only**; reading
invalid addresses raises a bus error.

| Address | Function |
|---|---|
| `0x53000000 + 4*rx` | phase increment = f * 2^30 / 122.88 MHz |
| `0x50000040 + rx*0x10000` | input select: 0 = ANT1, 1 = ANT2, 2 = third input |
| `0x50000000 + rx*0x10000` | control, written as 2 |

Stream frames: 16 byte header, then 32-bit little-endian words. Bits 0..23 hold the signed
sample, bits 24..31 a tag (`tag & 7` = stream, `0x30` marks the first word). Samples are I/Q pairs
of receivers 0..7, interleaved.

Gain (-9..32 dB) and attenuator (0..31 dB) per ADC are set through the vendor tool
`/usr/bin/r2t2` (bit-banged GPIO).

## Build and install

Build on an x86_64 host with Docker and qemu-user (binfmt), not on the R2T2: the armv7
Debian 9 container (`build/Dockerfile.armv7`) has the device's glibc 2.24 and gcc 6.3.

```sh
tools/build-ai1.sh                      # BUILD_HOST=user@host, result in out/r2t2sdr
R2T2_SUDO=... tools/deploy-r2t2.sh      # R2T2_HOST, R2T2_KEY; installs without compiling
```

`build-ai1.sh` refuses binaries that need symbols newer than glibc 2.24; `deploy-r2t2.sh`
checks the checksum and the shared libraries on the device, runs `make install-files` and
restarts the service. First installation on the device:

```sh
sudo make install-files BIN=out/r2t2sdr
sudo systemctl disable --now radiowatch
sudo systemctl enable --now r2t2sdr
```

`make` and `make install` still work on the device itself (Arch Linux ARM, gcc 6.3, fftw).

`make install-files` copies the binary and web files to `/opt/r2t2sdr`, installs
`/etc/r2t2sdr.conf` if it does not exist yet and adds `r2t2sdr.service`. The service conflicts with
the original `radiowatch.service` (`r2t2srv`/`r2t2client`), which must not run at the same time.

The web interface is served on port 8073.

Create the first administrator on the device (asks for the password, the running service
picks the account up without a restart):

```sh
sudo /opt/r2t2sdr/r2t2sdr --user DL0XYZ --role admin
sudo /opt/r2t2sdr/r2t2sdr --users      # list accounts
```

## Configuration

`/etc/r2t2sdr.conf`, see [dist/r2t2sdr.conf](dist/r2t2sdr.conf):

```
title = R2T2 WebSDR
callsign = DL0XYZ
location = Somewhere
gain1 = 0
band = 40m, 7100000, 1, lsb     # name, centre Hz, antenna input, default mode[, lo, hi]
max_listeners = 30              # listeners fed from the wide stream (CPU guard)
```

A band starts with 192 kHz around its centre and is widened to its edges (IARU region 1 by
default, or `lo`, `hi` in Hz) over up to four receivers while more than one receiver is free.
Each segment contributes about 170 kHz (the FPGA decimation filter rolls off at the edges).

`r2t2sdr --no-ddc` gives every listener a receiver of its own, for comparisons.

Settings changed in the web interface live in `/var/lib/r2t2sdr` (`state_dir`) and override
the config file:

| File | Content |
|---|---|
| `users` | accounts: name, role, PBKDF2 iterations, salt, hash (mode 0600) |
| `sessions` | login sessions: SHA-256 of the token, name, expiry (mode 0600) |
| `bookmarks` | id, frequency, mode, name |
| `antennas` | name and frequency ranges per antenna input, default input |
| `station.conf` | title, callsign, location, locator, access, chat, gain, attenuator |

## Web interface

Zoom: the mouse wheel over waterfall or spectrum zooms around the pointer (Shift + wheel tunes),
dragging pans, a double click shows the whole view, two fingers zoom on touch screens; the
panel has −, ganz and ＋. Each viewer zooms on their own; the server then sends only the shown
part at up to the full FFT resolution (~47 Hz), no extra receiver is used.

Opened directly, the page looks like the remote station of afu.tools: `afu-style.css` and
`remote.css` are unchanged copies of the afu.tools style sheets, the markup uses the same
classes (`rm-…`). Own additions are in `sdr.css`. Tables follow the afu.tools design guide with
`afu-tabelle.js` (copy of `js/tabelle.js`): sortable heads, filter fields that move into the head
on wide screens, count line, footer with paging (10 per page unless the reader chose more). Parts of the remote station that make no sense
for a receiver (transmitting, VFO B, RIT, memories, log book, TX meters) are left out.

- header with frequency (mouse wheel over a digit tunes that digit), mode, step, station and
  UTC clock, connection state, audio and administration
- band buttons and band scale with passband, the part the waterfall shows and bookmark flags
- level meter (dBFS in the filter), antenna in use, free receivers
- waterfall and spectrum with a quick panel (volume, mute, squelch, filter, waterfall levels)
- boxes for bookmarks and chat
- top bar as in the afu-remote direct access, light/dark key, "Melden" (report a problem to
  afu.tools) when the browser reaches afu.tools

Inside an iframe (or with `?embed`, `?embed=0` forces the full page) it shows only the
frequency scale, spectrum and waterfall, without controls and without audio.

All URLs are relative (`afu-style.css`, `app.js`, `ws`, `api/...`), so the page also works below
a path prefix, e.g. behind the WebSDR tunnel of afu-remote. Style sheets, scripts and images are
cached for a week; the `?v=` stamps in `index.html` change with every release.

## Accounts

Listening and the waterfall need no account. Roles, each including the ones below:

| Role | May |
|---|---|
| `admin` | everything: accounts, station, antennas, bookmarks |
| `lesezeichen` | maintain bookmarks |
| `station` | as `nutzer`, but first in line for receivers (external waterfall of a remote station) |
| `nutzer` | listen and chat when the station restricts this to logged-in users |

Logged-in accounts take receivers from guests, `station` takes them from everyone else.

Verwaltung > Online (admins) lists every connection: account or guest (with the chat name),
address, waterfall, what they listen to (and whether that uses a receiver of its own), time
online and time since the last action; refreshed every 3 s. Below it an activity log that an
admin switches on there (`log = on` in `station.conf`, off by default): connect and disconnect
with duration, login and logout, listening with frequency and mode (tuning logged at most every
15 s for moves of 3 kHz or more), waterfall changes and displacements, with name and address, in
`activity.log` in the state directory (at 1 MB the previous generation becomes
`activity.log.1`). The page shows the newest 500 lines, searchable.

Everybody sees a line above the chat: "Online: dk5den, dl1nux, Peter + 2 Gäste" (accounts and
guests who gave a chat name, other guests counted).

Login works without the password on the wire, also over plain http:

1. the browser asks for a challenge and gets salt, iterations and a one-time nonce
   (unknown names get a stable fake salt, so the answer does not reveal accounts),
2. it computes `PBKDF2-HMAC-SHA256(password, salt, 10000)` itself (`kdf.js`, browsers offer
   no WebCrypto over http) and sends `HMAC-SHA256(result, nonce)`,
3. the server compares with the stored hash; the nonce is valid once and for 60 s.

New passwords are hashed in the browser as well, only salt and hash are sent. Five failed
logins lock the address for 60 s. Sessions last 90 days and survive a restart.

Without TLS a listener on the network can still see the session token and try to guess weak
passwords offline from a recorded login. Use passwords that are not used elsewhere.

## Bookmark import and export

Under Verwaltung > Lesezeichen, in the format of OpenWebRX (`bookmarks.json`: a list of
`{name, frequency, modulation}`). The export writes FM as `nfm`. The import also reads this
program's own format, shows a preview and either adds (same frequency and name are skipped) or
replaces the list. OpenWebRX modulations are mapped onto the five modes here: digital modes
carried by SSB (FT8, WSPR, JS8, RTTY, HFDL, NAVTEX, fax …) become USB, DMR/YSF/D-Star/NXDN/M17,
packet and POCSAG become FM, DRM and SAM become AM, unknown ones USB. Up to 1000 bookmarks.

OpenWebRX+ hands out its bookmarks only per profile over its WebSocket; a script that selects
the profiles one after another and collects the `bookmarks` messages produces such a file.

## Antennas

Under Verwaltung > Antennen each input (ANT1, ANT2) gets a name, the frequency ranges it serves
(kHz, e.g. `3500-3800, 7000-7200`), gain and attenuator. A frequency goes to the first input
whose ranges cover it, otherwise to the default input. Waterfall views and listeners switch
input automatically; the page shows the antenna in use.

## Chat

Messages go to everybody connected. The last 30 stay in memory for newcomers, nothing is
written to disk. Guests choose a name and are marked as guests. The station decides who may
write: everybody, logged-in users only, or nobody. Ten messages per minute per connection.

## Embedding and remote control

The embedded page behaves like OpenWebRX towards the page around it, so tools written for
OpenWebRX (afu.tools/remote, "external waterfall") drive it without changes:

- `#freq=<Hz>,mod=<usb|lsb|cw|am|nfm>` in the address, read on load and on every change
- `window.UI.setFrequency(hz)`, `UI.getFrequency()`, `UI.setModulation(mode)`,
  `UI.getModulation()` for same-origin parents (e.g. through a tunnel); in addition
  `UI.setZoom(factor, centreHz)` (1 = whole view) and `UI.getZoom()` (`factor`, `lo`, `hi`)
- `postMessage({type: "r2t2sdr:set", freq, mode, zoom, zoomCenter}, "*")` for cross-origin parents

The waterfall follows the frequency: if a configured band covers it, that band is shown,
otherwise a free view centred on a 25 kHz grid. The view moves once the frequency leaves the
middle 85 % of the span. Viewers of the same view share one receiver.

A click or wheel step in the embedded waterfall is reported to the parent:

```js
window.addEventListener("message", (e) => {
  if (e.data?.source === "r2t2sdr" && e.data.type === "tune") {
    // e.data.freq (Hz), e.data.mode (usb, lsb, cw, am, nfm)
  }
});
```

## Engine mode for afu-remote

r2t2sdr can serve as the signal engine behind [afu-remote](https://afu.tools/remote): afu-remote
handles the page, accounts, bookmarks and chat, the browser fetches waterfall and audio straight
from r2t2sdr. Python stays out of the data path.

- **Tickets.** afu-remote signs a short-lived ticket for each session with a secret shared with
  r2t2sdr (`ticket_secret`, a file with at least 32 characters; its text is the HMAC key). The
  browser opens `ws://<host>:8073/ws?ticket=<ticket>`. A ticket is
  `base64url(JSON) "." base64url(HMAC-SHA256(secret, first part))` with `d` (receiver id, must
  equal `ticket_device`), `c` (call), `r` (role), `e` (expiry, Unix time) and `n` (nonce, each
  accepted once). Roles map as `besitzer` → admin, `verwalter` → bookmarks, `station` →
  station, `nutzer`/`hoerer` → user, `gast` → guest. Invalid, expired or replayed tickets get an
  error and the connection closes. Login, logout and password change do not apply to ticket
  sessions.
- **`ticket_only = 1`** refuses connections without a ticket; with 0 the direct access stays as
  it is.
- **Control socket.** `control_socket` is a Unix socket (mode 0660, group `control_group`):
  write one JSON line, read one back. `{"cmd":"status"}` answers like the `status` message,
  `{"cmd":"online"}` like `online`, where each entry carries `via` (`afu-remote` or `direct`).

`tools/engine-setup-r2t2.sh` creates the secret and adds these keys on the device.

## HTTP API

| Path | Answer |
|---|---|
| `GET api/config` | version, title, callsign, location, locator, span, bins, audio rate, receivers, max. frequency, bands |
| `GET api/status` | users, listeners, free and total receivers, `active` waterfalls (see `status`) |

Both return JSON with `Access-Control-Allow-Origin: *`.

## WebSocket protocol

Endpoint `ws` relative to the page (`ws://<host>:8073/ws`).

Client to server (JSON text frames):

| Command | Fields |
|---|---|
| `band` | `id`: show a configured band |
| `view` | `center` (Hz): show a free view around this frequency |
| `tune` | `freq` (Hz), `mode` (`usb`, `lsb`, `cw`, `am`, `fm`/`nfm`), `lo`, `hi` (passband in Hz relative to `freq`) |
| `start` / `stop` | audio on/off (a receiver only outside every active waterfall) |
| `squelch` | `level` in dBFS, `-999` = off |
| `antenna` | `input`: 0 = by frequency (antenna ranges), 1 or 2 = this input for the own audio |
| `zoom` | `lo`, `hi` (Hz): only this part of the view is sent, at up to the full FFT resolution; without fields the whole view |
| `chat` | `text`, `name` (guests only) |
| `challenge` | `user`, `purpose` (`login` or `passwd`) |
| `login` | `user`, `proof` |
| `auth` / `logout` | `token` |
| `passwd` | `proof` (old password), `salt`, `hash`, `iter` |
| `bm_set` / `bm_del` | `id` (0 = new), `name`, `freq`, `mode` / `id` |
| `users`, `user_set`, `user_del` | `name`, `role`, optional `salt`, `hash`, `iter` |
| `online` | admin: list of connections |
| `log` / `log_set` | admin: activity log; `on` (`on`/`off`), `clear` (`yes`) |
| `station_set` | `title`, `callsign`, `location`, `locator`, `access` (`open`, `login`), `chat` (`all`, `login`, `off`) |
| `antennas`, `ant_set` | `input`, `name`, `ranges`, `gain`, `att`, `default` |
| `ping` | keep-alive, sent every 10 s; clients silent for 30 s are dropped |

Server to client, JSON text frames:

| `type` | Fields |
|---|---|
| `config` | as `api/config`, plus `access`, `chat`, antenna names |
| `view` | `id`, `band` (configured band or -1), `center`, `span` (one receiver), `lo`, `hi` (range shown), `input`, `antenna`, `segments` (`center`, `lo`, `hi` per receiver); `id` -1 with `reason`: no waterfall (all receivers busy or taken by a client with priority) |
| `log` | `on`, `lines`: newest first, each `[time UTC, who, address, event]` |
| `status` | `users`, `listeners`, `free`, `total`, `names` (`name`, `guest`), `guests` (unnamed guests), `active`: per waterfall `view`, `band`, `center`, `lo`, `hi`, `segments`, `viewers`, `listeners` |
| `audio` | `on`, `input`, `antenna`, `own` (true: own receiver, false: cut out of a waterfall) |
| `bookmarks` | `list` of `id`, `freq`, `mode`, `name` |
| `chat` | `who`, `guest`, `ts`, `text` |
| `challenge` | `purpose`, `user`, `salt`, `iter`, `nonce` |
| `login` / `logout` | `user`, `role`, `token` (only right after login) |
| `users`, `antennas`, `ok` | administration answers |
| `online` | `list`: per connection `user`, `role`, `guest` (chat name), `ip`, `since`, `idle` (s since the last command), `view`, `segments`, `zoom`, `listening`, `freq`, `mode`, `own`, `self` |
| `error` | `msg` (German, shown to the user) |

Binary frames:

- `0x01 view:uint8 segment:uint8 bins[1024]` waterfall line of one segment of view `view`
  (192 kHz around the segment centre), one byte per bin = dBFS + 170, lowest frequency first;
  segments are not synchronised, the client composes them by `view.segments`
- `0x03 view:uint8 segment:uint8 x0:uint16 n:uint16 bins[n]` zoomed waterfall: points
  `x0 .. x0+n-1` of a 2048-point line over the `zoom` range, taken from this segment's
  4096-point FFT (max of the bins a point covers, ~47 Hz resolution); the line is complete when
  the lowest segment inside the range has arrived
- `0x02 level:int16 pred:int16 index:uint8 adpcm[128]` 256 audio samples at 8 kHz,
  level in dBFS*10; the ADPCM state travels with every packet

## Status

Receive only; the R2T2 transmitter is locked in hardware.

Tested on one R2T2 (October 2026):

- frequency accuracy against broadcast carriers (< 1 ppm), waterfall and spectrum 0.1 to 61 MHz
- WebSocket protocol, audio packet rate and ADPCM decoding
- listeners from the wide stream: a broadcast carrier gives a 1 kHz tone in USB 1 kHz below
  and in LSB 1 kHz above, same as with an own receiver; 8 waterfalls plus 12 listeners at full
  audio rate
- band widening (20 m over three receivers, 31 m over three), shrinking when receivers are
  needed, priority guest < user < station with notices, full-receiver notice in the page
- service start after reboot, dead connections dropped after 30 s
- full page at 1440 px (dark and light) and 390 px, embedded page in an iframe including
  `window.UI`, `#freq=` and `postMessage` in both directions
- login by challenge-response (JavaScript KDF checked against Node crypto), wrong password,
  accounts created and deleted with browser-side hashes, bookmarks, antenna ranges switching
  the input, access restricted to logged-in users, chat with a logged-in user and a guest

Not tested yet:

- listening with a real HF antenna (only UHF stubs were connected, carriers visible, audio not
  intelligible)
- reception on 6 m (front end passes up to 61 MHz, no signal seen)
- operation behind the afu-remote WebSDR tunnel
- sending a report through "Melden" (button appears, the request itself was not sent)
