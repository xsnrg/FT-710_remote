#!/usr/bin/env python3
"""FT-710 <-> Mumble bridge (part of FT-710_remote).

RX: FT-710 USB audio (PipeWire source) -> Mumble, continuously (muted while we key).
TX: audio from other Mumble users -> FT-710 USB audio (PipeWire sink), with PTT via
    flrig XML-RPC. Unkeys after HANG_S of silence; hard TX timeout TX_TIMEOUT_S.
Keying only happens while <RB_CONFIG_DIR>/tx_enabled exists (default ~/.config/radio-bridge).
Settings come from RB_* environment variables (see bridge.env.example).

TX sequencing (all in the TxEngine thread; the Mumble audio callback only queues packets):
  first packet -> key via flrig -> prime the playback stream with silence and hold the
  audio for TX_DELAY_S while the rig switches -> play from a >= PREBUF_S jitter buffer
  with a FADE_S fade-in.  End of over: fade out, keep the stream fed with silence until
  HANG_S after the last packet, let the playback buffer drain, then unkey.
"""
import os, sys, ssl, math, time, signal, logging, threading, subprocess, xmlrpc.client, collections
from array import array

# --- Python 3.12+ removed ssl.wrap_socket, which pymumble 1.6.1 still calls ---
if not hasattr(ssl, "wrap_socket"):
    def _wrap_socket(sock, keyfile=None, certfile=None, server_side=False,
                     cert_reqs=None, ssl_version=None, ca_certs=None,
                     do_handshake_on_connect=True, suppress_ragged_eofs=True,
                     ciphers=None, server_hostname=None):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        if certfile:
            ctx.load_cert_chain(certfile, keyfile)
        return ctx.wrap_socket(sock, server_side=server_side,
                               do_handshake_on_connect=do_handshake_on_connect,
                               suppress_ragged_eofs=suppress_ragged_eofs)
    ssl.wrap_socket = _wrap_socket

import pymumble_py3 as pymumble
from pymumble_py3.constants import PYMUMBLE_CLBK_SOUNDRECEIVED, PYMUMBLE_CONN_STATE_CONNECTED

HOME = os.path.expanduser("~")
CFG = os.path.expanduser(os.environ.get("RB_CONFIG_DIR", os.path.join(HOME, ".config", "radio-bridge")))
TX_FLAG = os.path.join(CFG, "tx_enabled")
PW_FILE = os.path.expanduser(os.environ.get("RB_PASSWORD_FILE", os.path.join(CFG, "server.pw")))  # optional
KEYED_MARK = os.path.join(HOME, ".cache", "radio-bridge", "keyed")

MUMBLE_HOST = os.environ.get("RB_MUMBLE_HOST", "127.0.0.1")
MUMBLE_PORT = int(os.environ.get("RB_MUMBLE_PORT", "64738"))
MUMBLE_USER = os.environ.get("RB_MUMBLE_USER", "FT710")
FLRIG_URL = os.environ.get("RB_FLRIG_URL", "http://127.0.0.1:12345/RPC2")
# PipeWire/PulseAudio names of the radio's audio interface (`pactl list short sources|sinks`). Required.
SRC = os.environ.get("RB_SOURCE", "")
RX_BANDWIDTH = int(os.environ.get("RB_RX_BANDWIDTH", "72000"))  # total bit/s incl. overhead
SINK = os.environ.get("RB_SINK", "")
HANG_S = float(os.environ.get("RB_HANG_S", "0.7"))          # silence after last packet before unkey
TX_TIMEOUT_S = float(os.environ.get("RB_TX_TIMEOUT_S", "120"))
TX_DELAY_S = float(os.environ.get("RB_TX_DELAY_S", "0.07"))   # rig T/R switch time before audio
PREBUF_S = float(os.environ.get("RB_PREBUF_S", "0.05"))       # jitter buffer before audio starts
FADE_S = float(os.environ.get("RB_FADE_S", "0.015"))          # fade-in / fade-out length
LEAD_S = float(os.environ.get("RB_LEAD_S", "0.06"))           # audio kept queued ahead in pacat
MAX_BUF_S = float(os.environ.get("RB_MAX_BUF_S", "0.4"))      # cap on jitter buffer (latency)
GAP_LOG_S = 0.1                # log inter-packet gaps longer than this while keyed
LOCKOUT_QUIET_S = 1.5          # after a timeout, need this much silence before re-keying
KEY_RETRY_S = 1.0              # after a failed key attempt, ignore audio this long
RX_GAIN_FILE = os.path.join(CFG, "rx_gain")      # optional; overrides RB_RX_GAIN, re-read when changed
RX_GAIN_DEFAULT = float(os.environ.get("RB_RX_GAIN", "3.0"))
# TX (Mumble -> FT-710) level, linear amplitude factor written by FT-710 Remote. Applied here, so it
# works whether or not the pacat stream exists, and survives bridge restarts. 1.0 = unchanged.
TX_GAIN_FILE = os.path.join(CFG, "tx_gain")
TX_GAIN_DEFAULT = float(os.environ.get("RB_TX_GAIN", "1.0"))
RATE, FRAME = 48000, 960        # 48 kHz mono s16le, 20 ms frames (RX)
FRAME_BYTES = FRAME * 2
TX_FRAME = 480                  # 10 ms TX write unit
TX_FRAME_BYTES = TX_FRAME * 2
TICK_S = 0.005

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("radio-bridge")


def _secs(nbytes):
    return nbytes / 2.0 / RATE


class Gain:
    """Gain for s16 PCM with a soft limiter (no int16 wraparound).

    Linear up to KNEE, then tanh-compressed so the output approaches but never exceeds full scale.
    Uses a 65536-entry lookup table indexed by the s16 sample (negative indexes wrap to the end).
    The gain file is checked every `period` s. The first table is built at startup; later tables are
    built in a background thread and swapped in atomically, so audio threads never wait for a build.
    """
    KNEE, LIMIT = 24000.0, 32767.0

    def __init__(self, path=RX_GAIN_FILE, default=RX_GAIN_DEFAULT, max_gain=20.0, name="RX", period=5.0):
        self.path, self.default, self.max_gain, self.name, self.period = path, default, max_gain, name, period
        self.state = (None, None)          # (gain, lut), replaced as one tuple
        self._mtime, self._checked, self._building = None, 0.0, False
        self.refresh(force=True)

    @property
    def gain(self):
        return self.state[0]

    def _make_lut(self, g):
        k, span = self.KNEE, self.LIMIT - self.KNEE
        def f(x):
            y = abs(x) * g
            if y > k:
                y = k + span * math.tanh((y - k) / span)
            return int(round(math.copysign(min(y, self.LIMIT), x)))
        lut = [0] * 65536
        for x in range(-32768, 32768):
            lut[x] = f(x)
        return lut

    def _install(self, g, src):
        self.state = (g, None if g == 1.0 else self._make_lut(g))
        log.info("%s gain %.3f (%s)", self.name, g, src)

    def _bg_build(self, g, src):
        try:
            self._install(g, src)
        except Exception as e:
            log.warning("%s gain table build failed: %s", self.name, e)
        finally:
            self._building = False

    def refresh(self, force=False):
        now = time.monotonic()
        if not force and (self._building or now - self._checked < self.period):
            return
        self._checked = now
        try:
            mt = os.stat(self.path).st_mtime
        except OSError:
            mt = None
        if not force and mt == self._mtime:
            return
        self._mtime = mt
        cur = self.state[0]
        g = self.default
        if mt is not None:
            try:
                g = float(open(self.path).read().strip())
                if not math.isfinite(g):
                    raise ValueError
            except (OSError, ValueError):
                keep = cur if cur is not None else self.default
                log.warning("bad %s; keeping %.3f", self.path, keep)
                g = keep
        g = max(0.0, min(g, self.max_gain))
        if g == cur:
            return
        src = self.path if mt is not None else "default"
        if force or cur is None:
            self._install(g, src)
        else:
            self._building = True
            threading.Thread(target=self._bg_build, args=(g, src), daemon=True).start()

    def apply(self, pcm):
        g, lut = self.state
        if g == 1.0 or lut is None:
            return pcm
        a = array("h")
        a.frombytes(pcm[: len(pcm) & ~1])
        return array("h", [lut[x] for x in a]).tobytes()


def fade(pcm, start, total, rising):
    """Linear fade over samples [start, start+n) of a `total`-sample ramp."""
    a = array("h")
    a.frombytes(pcm[: len(pcm) & ~1])
    for i in range(len(a)):
        p = min(1.0, (start + i + 1) / float(total))
        a[i] = int(a[i] * (p if rising else 1.0 - p))
    return a.tobytes()


class _TimeoutTransport(xmlrpc.client.Transport):
    def make_connection(self, host):
        c = super().make_connection(host)
        c.timeout = 2.0
        return c


class Rig:
    """flrig PTT. Only ever called from the TxEngine thread."""
    def __init__(self, url=FLRIG_URL):
        self.url = url

    def set_ptt(self, on):
        # fresh proxy per call: a call that timed out can't leave a half-read connection behind
        rig = xmlrpc.client.ServerProxy(self.url, transport=_TimeoutTransport())
        rig.rig.set_ptt(1 if on else 0)


class Player:
    def __init__(self):
        self.p = None
        self.started = 0   # incremented each time a new pacat is spawned

    def write(self, pcm):
        """Returns False if pacat had died (a new one is started on the next write)."""
        if self.p is None or self.p.poll() is not None:
            self.p = subprocess.Popen(
                ["pacat", "--playback", "--raw", "--device=" + SINK, "--format=s16le",
                 "--rate=%d" % RATE, "--channels=1", "--latency-msec=60",
                 "--client-name=radio-bridge-tx", "--stream-name=Mumble to FT-710",
                 # level is set by tx_gain above; keep the stream at 100% and stop WirePlumber
                 # restoring an old per-app volume onto it
                 "--volume=65536", "--property=state.restore-props=false"],
                stdin=subprocess.PIPE)
            self.started += 1
        try:
            self.p.stdin.write(pcm)
            self.p.stdin.flush()
            return True
        except (BrokenPipeError, OSError):
            self.p = None
            return False

    def close(self):
        if self.p and self.p.poll() is None:
            self.p.terminate()


class TxEngine:
    """Owns PTT and TX playback. The Mumble callback only calls feed()."""
    IDLE, KEYED = 0, 1
    # playback sub-states while KEYED
    HOLD, PLAY, GAP, DRAIN = "hold", "play", "gap", "drain"

    def __init__(self, rig, player, txgain, clock=time.monotonic, flag_exists=None):
        self.rig, self.player, self.txgain, self.clock = rig, player, txgain, clock
        self.flag_exists = flag_exists or (lambda: os.path.exists(TX_FLAG))
        self.inq = collections.deque()       # (arrival_time, pcm) from the Mumble thread
        self.wake = threading.Event()
        self.last_arrival = 0.0
        self.state = self.IDLE
        self.keyed = False                   # read by the RX loop / status line
        self.locked_out = False
        self.retry_after = 0.0
        self._flag_checked = 0.0
        self._flag_ok = True

    # ---- Mumble thread: cheap, never blocks ----
    def feed(self, pcm):
        now = self.clock()
        self.inq.append((now, pcm, now - self.last_arrival))
        self.last_arrival = now
        self.wake.set()

    # ---- helpers ----
    def _mark(self, on):
        try:
            if on:
                os.makedirs(os.path.dirname(KEYED_MARK), exist_ok=True)
                open(KEYED_MARK, "w").close()
            else:
                os.remove(KEYED_MARK)
        except FileNotFoundError:
            pass

    def _ptt(self, on):
        t0 = self.clock()
        try:
            self.rig.set_ptt(on)
            ok, err = True, None
        except Exception as e:
            ok, err = False, e
        ms = (self.clock() - t0) * 1000
        if ok:
            (log.warning if ms > 250 else log.info)("flrig set_ptt(%d) took %.0f ms", 1 if on else 0, ms)
        else:
            log.error("flrig set_ptt(%d) failed after %.0f ms: %s", 1 if on else 0, ms, err)
        return ok

    def _pull(self):
        """Move queued packets into the jitter buffer (gain applied here, not in the Mumble thread)."""
        got = False
        while self.inq:
            t, pcm, _gap = self.inq.popleft()
            if self.state == self.KEYED:
                if self.prev_arrival and t - self.prev_arrival > GAP_LOG_S:
                    self.gaps += 1
                    log.info("packet gap %.0f ms while keyed", (t - self.prev_arrival) * 1000)
                self.prev_arrival = t
                self.buf += self.txgain.apply(pcm)
                self.pkts += 1
                got = True
        if len(self.buf) > int(MAX_BUF_S * RATE) * 2:
            drop = len(self.buf) - int(PREBUF_S * RATE) * 2
            drop -= drop % 2
            del self.buf[:drop]
            self.trimmed += _secs(drop)
            log.info("jitter buffer over %.0f ms; dropped %.0f ms to cut latency", MAX_BUF_S * 1000, _secs(drop) * 1000)
        return got

    def _write(self, pcm):
        if not self.player.write(pcm):
            # pacat died: restart the pacing clock with the new stream
            self.t0, self.written = self.clock(), 0
            self.player.write(pcm)
        self.written += len(pcm) // 2

    def _lead(self, now):
        return self.written / float(RATE) - (now - self.t0)

    def _write_frame(self, now):
        """Write one 10 ms frame (audio or silence) according to the playback sub-state."""
        n = TX_FRAME_BYTES
        fade_n = max(1, int(FADE_S * RATE))
        if self.sub == self.PLAY:
            if len(self.buf) >= 2 * n:
                pcm = bytes(self.buf[:n]); del self.buf[:n]
                if self.fade_pos < fade_n:
                    pcm = fade(pcm, self.fade_pos, fade_n, True)
                    self.fade_pos += len(pcm) // 2
                self.audio_s += _secs(len(pcm))
                self._write(pcm)
                return
            # about to run dry: fade out what is left (10-20 ms) and pad with silence
            rest = bytes(self.buf); self.buf.clear()
            if rest:
                k = len(rest) // 2
                rest = fade(rest, 0, k, False)
                self.audio_s += _secs(len(rest))
            self._write(rest + b"\0" * max(0, n - len(rest)) if len(rest) < n else rest)
            self.sub = self.GAP
            self.underruns_mid += 1
            return
        self._write(b"\0" * n)   # HOLD / GAP / DRAIN: keep the stream primed

    def _begin_over(self, now):
        self._mark(True)
        if not self._ptt(True):
            self._mark(False)
            self.inq.clear()
            self.retry_after = now + KEY_RETRY_S
            return False
        now = self.clock()
        self.state, self.keyed = self.KEYED, True
        self.t_first = self.first_arrival
        self.t_key = now
        self.t0, self.written = now, 0
        self.buf = bytearray()
        self.sub = self.HOLD
        self.fade_pos = 0
        self.prev_arrival = 0.0
        self.pkts = self.gaps = self.underruns_mid = 0
        self.audio_s = self.trimmed = 0.0
        self.t_audio = None
        self.drain_until = None
        log.info("PTT ON")
        return True

    def _end_over(self, why):
        if not self._ptt(False):
            time.sleep(0.1)
            if not self._ptt(False):
                log.error("PTT off failed twice (%s); check the rig", why)
        self.state, self.keyed = self.IDLE, False
        self._mark(False)
        dur = self.clock() - self.t_key
        drops = self.underruns_mid - (1 if why == "silence" else 0)
        lat = "%.0f ms" % ((self.t_audio - self.t_first) * 1000) if self.t_audio else "n/a"
        log.info("PTT OFF (%s) keyed=%.2fs audio=%.2fs pkts=%d gaps>100ms=%d rebuffers=%d trimmed=%.0fms first-pkt-to-audio=%s",
                 why, dur, self.audio_s, self.pkts, self.gaps, max(0, drops),
                 self.trimmed * 1000, lat)

    def _flag(self, now):
        if now - self._flag_checked >= 0.1:
            self._flag_checked, self._flag_ok = now, self.flag_exists()
        return self._flag_ok

    # ---- one scheduling step; run() calls this every TICK_S ----
    def step(self):
        now = self.clock()
        if self.state == self.IDLE:
            if not self.inq:
                if self.locked_out and now - self.last_arrival >= LOCKOUT_QUIET_S:
                    self.locked_out = False
                return
            if self.locked_out:
                if self.inq[0][2] < LOCKOUT_QUIET_S:     # audio never paused long enough
                    self.inq.clear()
                    return
                self.locked_out = False
            if now < self.retry_after or not self.flag_exists():
                self.inq.clear()
                return
            self.first_arrival = self.inq[0][0]
            if not self._begin_over(now):
                return
            now = self.clock()

        # ---- KEYED ----
        got = self._pull()
        if now - self.t_key > TX_TIMEOUT_S:
            self.locked_out = True
            self.inq.clear()
            self._end_over("TX timeout %.0fs" % TX_TIMEOUT_S)
            return
        if not self._flag(now):
            self._end_over("tx_enabled removed")
            return

        since_pkt = now - self.last_arrival
        if got and self.sub == self.DRAIN:
            self.sub, self.drain_until = self.GAP, None        # more audio before unkey: carry on
        if self.sub == self.HOLD:
            if now - self.t_key >= TX_DELAY_S and (len(self.buf) >= int(PREBUF_S * RATE) * 2
                                                   or (self.buf and since_pkt > 0.06)):
                self.sub, self.fade_pos = self.PLAY, 0
                if self.t_audio is None:
                    self.t_audio = now
                    log.info("audio starts %.0f ms after first packet (%.0f ms after key)",
                             (now - self.t_first) * 1000, (now - self.t_key) * 1000)
        elif self.sub == self.GAP:
            if len(self.buf) >= int(PREBUF_S * 0.8 * RATE) * 2 or (self.buf and since_pkt > 0.06):
                self.sub, self.fade_pos = self.PLAY, 0          # resume with a fresh fade-in
            elif not self.buf and since_pkt > HANG_S:
                self.sub = self.DRAIN
                self.drain_until = now + max(0.0, self._lead(now)) + 0.03
        if self.sub == self.DRAIN:
            if now >= self.drain_until:
                self._end_over("silence")
            return                                             # stop feeding; let pacat drain

        # pace writes against the wall clock, keeping LEAD_S queued in pacat
        lead = self._lead(now)
        if lead < -0.05:                                       # we were stalled; don't burst-write
            self.t0 = now - self.written / float(RATE)
        while self._lead(now) < LEAD_S:
            self._write_frame(now)

    def run(self, stop):
        while not stop.is_set():
            try:
                self.txgain.refresh()
                self.step()
            except Exception:
                log.exception("TX engine error")
                if self.state == self.KEYED:
                    try:
                        self._end_over("engine error")
                    except Exception:
                        log.exception("unkey after engine error failed")
            self.wake.wait(TICK_S)
            self.wake.clear()

    def shutdown(self):
        if self.state == self.KEYED:
            self._end_over("shutdown")


def main():
    if not SRC or not SINK:
        log.error("RB_SOURCE and RB_SINK must name the radio's audio source and sink (see bridge.env.example)")
        return 2
    password = open(PW_FILE).read().strip() if os.path.exists(PW_FILE) else ""
    stop = threading.Event()
    player = Player()
    rxgain = Gain()
    txgain = Gain(TX_GAIN_FILE, TX_GAIN_DEFAULT, max_gain=1.5 ** 3, name="TX", period=1.0)
    tx = TxEngine(Rig(), player, txgain)
    stats = {"rx_frames": 0, "tx_chunks": 0}
    log.info("TX sequencing: hang %.2fs, tx delay %.0f ms, prebuffer %.0f ms, fade %.0f ms, timeout %.0fs",
             HANG_S, TX_DELAY_S * 1000, PREBUF_S * 1000, FADE_S * 1000, TX_TIMEOUT_S)

    def on_sound(user, chunk):
        # Mumble thread: queue only. No flrig, no file I/O, no gain-table work here.
        stats["tx_chunks"] += 1
        tx.feed(chunk.pcm)

    def shutdown(*_):
        stop.set()
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    m = pymumble.Mumble(MUMBLE_HOST, MUMBLE_USER, port=MUMBLE_PORT, password=password, reconnect=True)
    m.set_application_string("FT-710_remote bridge")
    m.set_receive_sound(True)
    m.callbacks.set_callback(PYMUMBLE_CLBK_SOUNDRECEIVED, on_sound)
    m.start()
    log.info("connecting to Mumble %s:%d as %s", MUMBLE_HOST, MUMBLE_PORT, MUMBLE_USER)

    # TX engine thread (also refreshes the TX gain, whose table builds in the background)
    txt = threading.Thread(target=tx.run, args=(stop,), name="tx-engine", daemon=True)
    txt.start()

    rec = subprocess.Popen(
        ["parec", "--raw", "--device=" + SRC, "--format=s16le", "--rate=%d" % RATE,
         "--channels=1", "--latency-msec=20", "--client-name=radio-bridge-rx",
         "--stream-name=FT-710 to Mumble"], stdout=subprocess.PIPE)
    last_report = time.monotonic()
    was_connected = False
    try:
        while not stop.is_set():
            pcm = rec.stdout.read(FRAME_BYTES)
            if not pcm:
                log.error("parec ended; exiting for systemd restart")
                break
            connected = m.connected == PYMUMBLE_CONN_STATE_CONNECTED and m.sound_output is not None
            if connected != was_connected:
                log.info("Mumble %s", "connected" if connected else "disconnected")
                was_connected = connected
            if connected and m.bandwidth != RX_BANDWIDTH:
                # murmur.ini bandwidth=558000 makes pymumble encode ~1.3 KB Opus packets,
                # over the Mumble ~1 KB audio packet limit, so the server drops them silently.
                m.set_bandwidth(RX_BANDWIDTH)
                log.info("outgoing bandwidth capped at %d bit/s", RX_BANDWIDTH)
            if connected and not tx.keyed:
                try:
                    rxgain.refresh()
                    m.sound_output.add_sound(rxgain.apply(pcm))
                    stats["rx_frames"] += 1
                except Exception as e:
                    log.debug("add_sound: %s", e)
            if time.monotonic() - last_report > 60:
                log.info("status connected=%s rx_frames_sent=%d tx_chunks_rcvd=%d keyed=%s tx_enabled=%s",
                         connected, stats["rx_frames"], stats["tx_chunks"], tx.keyed, os.path.exists(TX_FLAG))
                last_report = time.monotonic()
    finally:
        stop.set()
        txt.join(timeout=3)
        tx.shutdown()
        rec.terminate()
        player.close()
        try:
            m.stop()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
