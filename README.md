# r2t2-websdr

Multi-user WebSDR for the DARC **R2T2** receiver (Zynq Z7020, dual 14-bit ADC, 122.88 MHz),
running directly on the device. No additional computer is required.

## How it works

The R2T2 FPGA contains eight digital down-converters. Each one delivers two streams over the
internal `rad0` interface (Ethertype `0x7232`):

| Stream | Rate per receiver | Used for |
|---|---|---|
| narrow (tag 1) | 16 kS/s | audio of one listener |
| wide (tag 2) | 192 kS/s | waterfall of one band |

`r2t2sdr` treats the eight receivers as a pool:

- every band currently viewed occupies one receiver for its waterfall (shared by all viewers),
- every listener occupies one receiver tuned to their own frequency.

Channel selection happens in the FPGA, so the CPU only computes the waterfall FFT and
demodulates 16 kS/s audio. One band plus six listeners uses about 40 % of one Cortex-A9 core.

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

On the R2T2 (Arch Linux ARM, gcc 6.3, fftw):

```sh
make
sudo make install
sudo systemctl disable --now radiowatch
sudo systemctl enable --now r2t2sdr
```

`make install` copies the binary and web files to `/opt/r2t2sdr`, installs
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
band = 40m, 7100000, 1, lsb     # name, centre Hz, antenna input, default mode
```

Each band shows 192 kHz around its centre (about 186 kHz usable, the FPGA decimation filter
rolls off at the edges).

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
| `nutzer` | listen and chat when the station restricts this to logged-in users |

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
  `UI.getModulation()` for same-origin parents (e.g. through a tunnel)
- `postMessage({type: "r2t2sdr:set", freq, mode}, "*")` for cross-origin parents

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

## HTTP API

| Path | Answer |
|---|---|
| `GET api/config` | version, title, callsign, location, locator, span, bins, audio rate, receivers, max. frequency, bands |
| `GET api/status` | users, listeners, free and total receivers, active views with viewers |

Both return JSON with `Access-Control-Allow-Origin: *`.

## WebSocket protocol

Endpoint `ws` relative to the page (`ws://<host>:8073/ws`).

Client to server (JSON text frames):

| Command | Fields |
|---|---|
| `band` | `id`: show a configured band |
| `view` | `center` (Hz): show a free view around this frequency |
| `tune` | `freq` (Hz), `mode` (`usb`, `lsb`, `cw`, `am`, `fm`/`nfm`), `lo`, `hi` (passband in Hz relative to `freq`) |
| `start` / `stop` | audio on/off (allocates a receiver) |
| `squelch` | `level` in dBFS, `-999` = off |
| `chat` | `text`, `name` (guests only) |
| `challenge` | `user`, `purpose` (`login` or `passwd`) |
| `login` | `user`, `proof` |
| `auth` / `logout` | `token` |
| `passwd` | `proof` (old password), `salt`, `hash`, `iter` |
| `bm_set` / `bm_del` | `id` (0 = new), `name`, `freq`, `mode` / `id` |
| `users`, `user_set`, `user_del` | `name`, `role`, optional `salt`, `hash`, `iter` |
| `station_set` | `title`, `callsign`, `location`, `locator`, `access` (`open`, `login`), `chat` (`all`, `login`, `off`) |
| `antennas`, `ant_set` | `input`, `name`, `ranges`, `gain`, `att`, `default` |
| `ping` | keep-alive, sent every 10 s; clients silent for 30 s are dropped |

Server to client, JSON text frames:

| `type` | Fields |
|---|---|
| `config` | as `api/config`, plus `access`, `chat`, antenna names |
| `view` | `id`, `band` (configured band or -1), `center`, `span`, `input`, `antenna` |
| `status` | `users`, `listeners`, `free`, `total` |
| `audio` | `on`, `input`, `antenna` |
| `bookmarks` | `list` of `id`, `freq`, `mode`, `name` |
| `chat` | `who`, `guest`, `ts`, `text` |
| `challenge` | `purpose`, `user`, `salt`, `iter`, `nonce` |
| `login` / `logout` | `user`, `role`, `token` (only right after login) |
| `users`, `antennas`, `ok` | administration answers |
| `error` | `msg` (German, shown to the user) |

Binary frames:

- `0x01 view:uint8 bins[1024]` waterfall line of view `view`, one byte per bin = dBFS + 170,
  lowest frequency first
- `0x02 level:int16 pred:int16 index:uint8 adpcm[128]` 256 audio samples at 8 kHz,
  level in dBFS*10; the ADPCM state travels with every packet

## Status

Receive only; the R2T2 transmitter is locked in hardware.

Tested on one R2T2 (October 2026):

- frequency accuracy against broadcast carriers (< 1 ppm), waterfall and spectrum 0.1 to 61 MHz
- WebSocket protocol, audio packet rate and ADPCM decoding, six listeners plus one waterfall
  at about 40 % of one CPU core
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
