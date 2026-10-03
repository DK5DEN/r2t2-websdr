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

## Web interface

Opened directly, the page is a full receiver in the afu.tools look (tokens, buttons and fields
of afu.tools `style.css`, tool-page layout): header with frequency, station and UTC clock,
waterfall with a control panel, light theme from the system setting.

Inside an iframe (or with `?embed`, `?embed=0` forces the full page) it shows only the
frequency scale, spectrum and waterfall, without controls and without audio.

All URLs are relative (`style.css`, `app.js`, `ws`, `api/...`), so the page also works below a
path prefix, e.g. behind the WebSDR tunnel of afu-remote.

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
| `ping` | keep-alive, sent every 10 s; clients silent for 30 s are dropped |

Server to client, JSON text frames:

| `type` | Fields |
|---|---|
| `config` | as `api/config` |
| `view` | `id`, `band` (configured band or -1), `center`, `span` |
| `status` | `users`, `listeners`, `free`, `total` |
| `audio` | `on` |
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

Not tested yet:

- listening with a real HF antenna (only UHF stubs were connected, carriers visible, audio not
  intelligible)
- reception on 6 m (front end passes up to 61 MHz, no signal seen)
- operation behind the afu-remote WebSDR tunnel
