# FT-710_remote

Remote operation of a Yaesu FT-710 over a LAN or VPN: receive and transmit audio
through a Mumble server, or Mumble-free with PipeWire network audio and a direct TX gate,
push-to-talk, RX/TX level control, and a live band scope (spectrum + waterfall) with
click-to-tune.

It has two halves:

* **Client app** (`app/`, Qt 6, C++) – runs on the operator's Linux desktop next to the
  Mumble client.
* **Rig-side services** (`server/`, Python) – run on the Linux machine that is
  USB-connected to the FT-710, next to flrig and a Mumble server (murmur).

![FT-710 Remote client showing the band scope and waterfall on 20 m](docs/screenshot.png)

## Features

* **Mumble bridge** – `radio_bridge.py` joins your Mumble server as a bot user
  (default `FT710`). Radio receive audio is sent to Mumble; Mumble speech from you is
  played into the radio's USB audio input and keys the transmitter through flrig.
* **PTT** – hold-to-talk in the client (it drives Mumble's push-to-talk; the bridge keys
  the rig only while your voice packets arrive, then unkeys after a short hang time).
* **RX / TX levels** – RX is your local Mumble playback volume; TX is a gain the bridge
  applies before the radio (stored on the rig host in `tx_gain`).
* **Band scope** – `ft710_scope.py` reads the FT-710's own scope data from the radio's
  internal FTDI FT4222 USB-to-SPI bridge, which appears on USB only when
  **MENU → SCU-LAN10 = ON**. The client shows spectrum + waterfall, with span and
  reference-level / auto-contrast controls, and **click-to-tune** (sets VFO-A via flrig;
  frequency only).
* **Network audio (optional, receive only)** – instead of hearing the radio only through
  Mumble, the rig host streams the radio's receive audio straight to the desktop with
  PipeWire (ROC with FEC, or RTP; optional Opus compression for VPN use). It appears as the
  input device **"FT-710 RX (network)"** for WSJT-X, fldigi etc. See [Network audio](#network-audio).
* **Direct TX (optional, Mumble-free)** – with network audio, PTT and your microphone can go
  straight to `ft710_txgate.py` on the rig host instead of through Mumble. The gate keys flrig
  only on an explicit PTT from the app and unkeys on its own if the app, the network or the
  audio goes away. See [Direct TX](#direct-tx-mumble-free).
* Status of murmur, the bridge, flrig and `tx_enabled` polled over ssh.
* Window size, volumes and scope settings remembered between runs.
* Optional: start Mumble minimized (hidden in its tray icon). **Disconnect…** offers to close
  Mumble the normal way (WM_DELETE_WINDOW, like its own close button), even while hidden.

## Architecture

```
 operator desktop                                   rig host (USB to FT-710)
 ┌──────────────────────────┐                       ┌──────────────────────────────────┐
 │ ft710-remote (Qt)        │──ssh (status, flrig──▶│ flrig  ──CAT──▶ FT-710           │
 │   │  click-to-tune, levels)   curl, tx_gain)     │ radio-bridge (pymumble) ◀─USB audio─▶ FT-710
 │   │ D-Bus / RPC          │                       │   │ keys PTT via flrig XML-RPC   │
 │   ▼                      │                       │   ▼                              │
 │ Mumble client ◀──────────┼──── Mumble (64738) ──▶│ murmur                           │
 │ band scope view ◀────────┼──── TCP 7355 ────────│ ft710-scope ◀─FT4222 SPI── FT-710 │
 └──────────────────────────┘                       └──────────────────────────────────┘
```

* The client never talks to the radio directly. CAT goes through flrig on the rig host
  (flrig owns the serial port), reached as `ssh <rig> curl http://127.0.0.1:12345/RPC2`.
* ssh uses ControlMaster connection sharing, so polls are cheap. Set up key-based login.
* The scope streamer is receive-only: it reads frames from the FT4222 and never writes
  to the radio.

## Requirements

Client (operator desktop, Linux, X11 or XWayland):
* Qt 6 (Widgets, Network, Xml, DBus), CMake ≥ 3.21, a C++17 compiler
* libxcb and libopus (dev packages), pkg-config
* Mumble client (1.4+) with its RPC/D-Bus interface, PipeWire or PulseAudio (`pactl`)
* OpenSSH client with key login to the rig host

Rig host (Linux):
* flrig, configured for the FT-710 with XML-RPC enabled (default port 12345)
* Mumble server (murmur / mumble-server)
* Python 3 with `pymumble`, `opuslib`, `protobuf` (`server/radio-bridge/requirements.txt`),
  PipeWire or PulseAudio (`pactl`, `pacat`), `curl`
* FTDI **libft4222** (for the band scope; see below)

Debian/Ubuntu example (client): `sudo apt install qt6-base-dev libxcb1-dev libopus-dev cmake g++ pkg-config mumble`

## Rig-side setup

1. **flrig**: run it on the rig host with the FT-710 CAT port; enable XML-RPC.
2. **Mumble server**: install murmur, set a server password if you want one.
3. **Bridge**:
   ```sh
   mkdir -p ~/radio-bridge && cp server/radio-bridge/radio_bridge.py ~/radio-bridge/
   python3 -m venv ~/radio-bridge/venv
   ~/radio-bridge/venv/bin/pip install -r server/radio-bridge/requirements.txt
   mkdir -p ~/.config/radio-bridge
   cp server/radio-bridge/bridge.env.example ~/.config/radio-bridge/bridge.env   # edit RB_SOURCE / RB_SINK
   # optional server password (never commit it):
   install -m 600 /dev/null ~/.config/radio-bridge/server.pw && $EDITOR ~/.config/radio-bridge/server.pw
   cp systemd/radio-bridge.service ~/.config/systemd/user/
   systemctl --user daemon-reload && systemctl --user enable --now radio-bridge
   ```
   Find the radio's USB audio names with `pactl list short sources` / `pactl list short sinks`.
   For user services that must run without a login session: `loginctl enable-linger $USER`.
4. **Band scope**:
   ```sh
   mkdir -p ~/ft710-scope && cp server/ft710-scope/ft710_scope.py ~/ft710-scope/
   # put libft4222.so in ~/ft710-scope/lib/ (see "FTDI library")
   mkdir -p ~/.config/ft710-scope
   cp server/ft710-scope/scope.env.example ~/.config/ft710-scope/scope.env    # set FT710_SCOPE_ALLOW to your LAN
   cp systemd/ft710-scope.service ~/.config/systemd/user/
   systemctl --user daemon-reload && systemctl --user enable --now ft710-scope
   ```
   Turn **MENU → SCU-LAN10 ON** on the radio, then check with
   `python3 ~/ft710-scope/ft710_scope.py analyze`.

### udev rule (FT4222 access)

The FT4222 inside the FT-710 is USB `0403:601c`. Give the service user access:

```sh
sed "s/@USER@/$USER/" udev/99-ft710-ft4222.rules.in | sudo tee /etc/udev/rules.d/99-ft710-ft4222.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403
```

### FTDI library

`libft4222` is proprietary FTDI software and is **not included** in this repository.
Download "LibFT4222" for Linux from FTDI's website (ftdichip.com → Drivers → D2XX /
LibFT4222), extract it, and copy the `libft4222.so.*` for your architecture to
`~/ft710-scope/lib/libft4222.so` (or point `FT710_SCOPE_LIB` at it). Tested with 1.4.4.

## Client setup

```sh
cmake -S app -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j
cmake --install build          # installs bin/ft710-remote and a .desktop file
```

On first start the **Settings…** dialog opens. Tabs:

* **Rig host** – ssh destination (`host`, `user@host` or an `~/.ssh/config` alias),
  flrig host/port as seen from the rig host, bridge service name, bridge config dir,
  the bridge's Mumble user name, the Mumble server service name; a "Test ssh" button.
* **Mumble** – server (blank = rig host), port, your user name, channel, and where the
  server password comes from: read `server.pw` on the rig host over ssh, none (use
  Mumble's saved login), or stored locally in the settings file; "start Mumble minimized".
* **Band scope** – streamer host (blank = rig host) and port.
* **Network audio** – audio mode (Mumble / LAN stream ROC / LAN stream RTP), compression,
  port, buffer, packet size, ROC FEC, source node, send-to address. See below.
* **General** – station label for the window title, optional help/notes file, path of
  the settings file.

Changes are validated and apply immediately, no restart. Settings live in
`~/.config/FT-710_remote/ft710-remote.conf` (a locally stored password is plain text
in that file, which the app keeps owner-only (mode 600); prefer the ssh or saved-login options).
`ft710-remote --print-config` prints the effective settings (without the password)
and exits.

Settings from an older build (`~/.config/<anything>/ft710-remote.conf`) are imported
once on first start; keys already present are not overwritten.

## Network audio

**Settings → Network audio → Audio mode**:

| Mode | What it does |
|---|---|
| **Mumble** (default) | As before: radio audio only through Mumble. |
| **LAN stream: ROC** | PipeWire `roc-sink` on the rig host → `roc-source` on the desktop, Reed-Solomon FEC (`rs8m`) repairs lost packets. Recommended on a LAN. L16 only. |
| **LAN stream: RTP** | PipeWire `rtp-sink` → `rtp-source`. Compression **None** (L16) or **Opus 24/32/48/64 kbit/s** for VPN / slow links. |

The stream itself is **receive only**. With **PTT / TX audio = Mumble** (default) PTT and TX
audio still go through Mumble and radio-bridge exactly as before (keep Mumble connected for PTT;
mute Mumble's playback or leave the app's "Listen" off so you do not hear the radio twice).
With **PTT / TX audio = Direct to the TX gate** Mumble is not used at all; see
[Direct TX](#direct-tx-mumble-free).

How it works:
* **Connect** starts the stream (and Mumble as usual), **Disconnect** stops it, and it is
  stopped when the app quits (unless "keep streaming" is ticked). The **Start/Stop** button in
  the "RX network" row controls it on its own.
* The app writes the configs from built-in templates and runs them as systemd **user** units:
  `ft710-netaudio-recv` locally (config in `~/.config/FT-710_remote/netaudio/`) and
  `ft710-netaudio-send` on the rig host over ssh (config in `~/.config/ft710-netaudio/`).
  Nothing about your setup is hard-coded: the rig host is the ssh host from Settings, the
  source is the **Source node** setting or, if blank, `RB_SOURCE` from the bridge's
  `bridge.env`, and the destination is this computer's address as the rig host sees it
  (or the **Send to** setting).
* The sender captures only that node (`node.dont-fallback`): if the radio's USB audio is
  missing it is not started (no fallback to another microphone), and it is restarted when the
  device comes back.
* **Health**: the app meters the received level (shown as dBFS in the "RX network" row),
  polls both units, and treats digital silence for more than 2 s or a stopped unit as *link
  down*. It then restarts the sender (every third try both ends) with 1 / 2 / 5 / 10 s backoff.
  The status line shows state, uptime and restart count.
* **Listen** plays the stream on this computer's speakers via a `pw-loopback` with its own
  volume (0–150 %); off by default. It never changes what WSJT-X/fldigi record.

**WSJT-X / fldigi**: choose **FT-710 RX (network)** as the audio input (WSJT-X: Settings →
Audio → Input; fldigi: Configure → Sound Card → PulseAudio/PortAudio capture). It is a
normal PipeWire/Pulse source (`ft710_rx_net`), mono 48 kHz. Transmit audio is unchanged.

Requirements:
* PipeWire ≥ 1.0 on both computers (`pipewire -c`, `pw-record`, `pw-loopback`, `pactl`).
* **ROC**: `pipewire-module-roc` (roc-toolkit ≥ 0.4) on **both** computers
  (Fedora: `sudo dnf install pipewire-module-roc`; Debian/Ubuntu: `pipewire-roc` / `libpipewire-0.3-modules`).
* **Opus**: GStreamer on the rig host with the PipeWire and Opus plugins
  (`gst-launch-1.0`, `pipewiresrc`, `opusenc`, `rtpopuspay`; Fedora: `gstreamer1-plugins-base
  gstreamer1-plugins-good pipewire-gstreamer`). roc-toolkit 0.4 has no Opus codec and
  PipeWire's own RTP Opus encoder has no bitrate setting, so Opus is sent with GStreamer
  and received by PipeWire's `rtp-source` (`sess.media = "opus"`).
* Firewall on the desktop: UDP **port, port+1, port+2** for ROC (default 10001–10003),
  UDP **port** for RTP.

Measured on a LAN (FT-710 USB audio, mono 48 kHz, rig host sender / desktop receiver):

| Mode | Wire bitrate | Sender CPU | Receiver CPU | Notes |
|---|---|---|---|---|
| RTP L16, 10 ms packets | ~800 kbit/s | 1.7 % (pipewire, 11 MB) | 0.2 % | lossless; 95–150 ms with 100 ms buffer |
| ROC L16, rs8m FEC, 120 ms | ~1160 kbit/s (743 source + 411 repair + 4 control) | 1.6–1.8 % (10 MB) | 1.3–1.7 % | ~160–170 ms; survives packet loss |
| RTP Opus 24 kbit/s, 20 ms | 24 kbit/s payload, ~41 kbit/s on the wire | 2.7–4 % (gst-launch, 18 MB) | 0.4 % | 50 packets/s |
| RTP Opus 32 kbit/s | 25 kbit/s payload, ~42 kbit/s wire | 3–4.6 % | 0.4 % | VBR: SSB audio needs less than the target |
| RTP Opus 48 kbit/s | 53 kbit/s payload, ~70 kbit/s wire | 3.2 % | 0.3 % | |
| RTP Opus 64 kbit/s | 69 kbit/s payload, ~86 kbit/s wire | 3.4 % | 0.3 % | |
| Mumble (for comparison) | ~72 kbit/s | bridge 6.6 %, murmur 1.3 % | client 1.2 % (143 MB) | |

Opus is lossy; for digital modes on a LAN prefer L16. 32–48 kbit/s is plenty for SSB/FT8
over a VPN.

Receive latency, measured from the rig host's capture to arrival on the desktop (LAN, GCC-PHAT
cross-correlation over 2 s windows, ~5 min per mode; milliseconds):

| Mode | Median | p10 | p90 |
|---|---|---|---|
| RTP L16, 40 ms buffer | 59 | 53 | 65 |
| ROC L16, 60 ms | 82 | 77 | 94 |
| RTP Opus 32, 60 ms | 98 | 87 | 107 |
| RTP L16, 120 ms | 150 | 144 | 154 |
| ROC L16, 120 ms | 158 | 143 | 158 |
| RTP Opus 48, 120 ms | 171 | 161 | 177 |
| RTP Opus 32, 120 ms | 192 | 188 | 198 |
| Mumble (packet arrival at the decoder) | 24 | 24 | 35 |

The Mumble figure excludes the Mumble client's jitter buffer and audio output (typically another
30–70 ms), so in practice it is comparable to RTP L16 / 40 ms or ROC / 60 ms. Lower buffers are
fine on a quiet wired LAN; use 120 ms or more over Wi-Fi or a VPN.

Optional unit files (`server/netaudio/ft710-netaudio-send.service`,
`app/netaudio/ft710-netaudio-recv.service`): copy them to `~/.config/systemd/user/` if you
want the units to be regular installed services; the app then uses `systemctl --user
restart` instead of a transient `systemd-run` unit. The `*.in` files in the same folders are
the templates compiled into the app.

Command line (handy for testing; `--na-*` overrides are not saved):

```sh
ft710-remote --netaudio start [--na-mode roc|rtp] [--na-compression none|opus] [--na-bitrate 32] [--na-port N] [--na-latency MS]
ft710-remote --netaudio watch 30    # monitor a running stream: level, unit states, recoveries
ft710-remote --netaudio stop
```

## Direct TX (Mumble-free)

`server/ft710-txgate/ft710_txgate.py` replaces Mumble + radio-bridge for transmit. The app holds
one authenticated TCP control connection to it (HMAC challenge with a shared token, heartbeat
every 100 ms) and, while PTT is held, sends the selected microphone as RTP over UDP on the same
port (L16, or Opus at the stream's bitrate when compression is on). The gate is then the only
thing that plays into the radio's USB audio input (`RB_SINK`) and the only thing that keys flrig.

```
 desktop: ft710-remote ── TCP 10010 (auth, HB, PTT) ──▶ rig host: ft710-txgate ── flrig set_ptt
          mic (pactl source) ── UDP 10010 RTP ───────▶            └─ pacat ──▶ FT-710 USB audio
```

Rig-host install (do **not** enable it; the app starts it when you Connect):

```sh
mkdir -p ~/ft710-txgate ~/.config/ft710-txgate
cp server/ft710-txgate/ft710_txgate.py ~/ft710-txgate/
cp server/ft710-txgate/txgate.env.example ~/.config/ft710-txgate/txgate.env   # set TG_ALLOW to your LAN/VPN
cp systemd/ft710-txgate.service ~/.config/systemd/user/
systemctl --user daemon-reload
```

It reads `bridge.env` first (so `RB_SINK`, `RB_FLRIG_URL`, `RB_HANG_S` are shared) and uses the
same `tx_enabled` and `tx_gain` files in `~/.config/radio-bridge/`. Needs Python 3, `pacat`
and libopus (for Opus). Open TCP+UDP `TG_PORT` (default 10010) in the rig host's firewall for
the allowed subnets. The token is created (mode 600) on first start; the app reads it over ssh
and never stores it.

Client: **Settings → Network audio** → a LAN stream mode, **PTT / TX audio = Direct to the TX
gate**, pick the **Microphone**, TX gate port and service. Then:

* **Connect** checks flrig PTT is 0 and no keyed marker exists, stops `radio-bridge` (the units
  conflict, so only one of them can ever drive the radio), starts `ft710-txgate`, reads the token
  and connects. The "TX gate (direct TX)" row shows the link state; reconnects use 1/2/5/10 s
  backoff and never re-key by themselves.
* **PTT / Space** keys only when the link is ready and TX keying is enabled. The button shows
  TALKING when the gate reports the rig keyed; the RX/TX label comes from flrig. The "TX mic"
  meter shows your level; the TX slider is applied in the gate.
* **Disconnect** (and quitting) unkeys, stops the gate and starts `radio-bridge` again — only if
  flrig reports PTT 0 ("restore radio-bridge" in Settings). Switching back to Mumble mode always
  restores the bridge.

Gate behaviour: key → 70 ms TX delay → fade in; on release drain, fade out, 0.7 s hang, unkey.
It unkeys at once on control-connection loss, a heartbeat older than 500 ms, an audio gap over
200 ms while PTT is held, the 120 s TX timeout (then PTT must be released first), `tx_enabled`
removed, `radio-bridge` starting, or service stop; `ExecStopPost` unkeys after a crash (only if
its own keyed marker exists). Settings: `server/ft710-txgate/txgate.env.example`.

Tests (no radio, no real flrig, no sound card: a fake flrig XML-RPC server and a fake `pacat`
or a PipeWire null sink):

```sh
cd server/ft710-txgate
python3 test_txgate.py [--null-sink]            # the gate: auth, allowlist, timeouts, gaps, failsafes
python3 test_app_txgate.py ../../build/ft710-remote   # the real app (CLI + GUI PTT path) against the gate
python3 test_app_scripts.py                     # the app's ssh start/stop scripts with temporary user units
```

## Safety

* The bridge keys the radio **only while `~/.config/radio-bridge/tx_enabled` exists**
  on the rig host. Remove the file (or use the client's TX-enable control) to make the
  system receive-only.
* Hard **transmit timeout of 120 s** (`RB_TX_TIMEOUT_S`); the bridge unkeys and ends the
  over. It also unkeys after `RB_HANG_S` of silence and if `tx_enabled` disappears.
* If the bridge dies while keyed, the systemd unit's `ExecStopPost` unkeys via flrig
  (only when the bridge's own `keyed` marker exists).
* The TX gate (direct TX) keys only on an explicit PTT from the authenticated app, only while
  `tx_enabled` exists and radio-bridge is stopped, with the same 120 s timeout and hang, and
  unkeys by itself when heartbeats or audio stop (see [Direct TX](#direct-tx-mumble-free)).
* The scope streamer and click-to-tune never key the transmitter; click-to-tune changes
  only VFO-A frequency.
* You are responsible for operating within your license and local regulations,
  including control-operator requirements for remote operation.

## Credits

* [wfview](https://wfview.org/) – FT-710 scope frame layout and FT4222 handling
  (`yaesu_scope_data`, `ft4222handler`).
* [ratmandu/YaesuWFTesting](https://github.com/ratmandu/YaesuWFTesting) – Yaesu
  SCU-LAN10 / FT4222 waterfall experiments.
* VLSC – prior work on the Yaesu scope stream.
* Mumble / pymumble, flrig.

Copyright 2026 Jim Howard. Licensed under the Apache License, Version 2.0; see [LICENSE](LICENSE).
