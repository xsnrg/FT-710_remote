#!/usr/bin/env python3
"""FT-710 TX gate (part of FT-710_remote): Mumble-free transmit path.

The FT-710 Remote app (network audio mode) holds one authenticated TCP control connection and
streams the desktop mic as RTP over UDP while PTT is held. This service is the only thing that
plays into the radio's USB audio sink (TG_SINK, default RB_SINK) and the only thing that keys
flrig while it runs. It never keys on audio alone: only an explicit "PTT 1" from the app keys.

Safety:
  * keys only while <TG_CONFIG_DIR>/tx_enabled exists, radio-bridge is not active and its keyed
    marker is absent, and TG_SAFE is not set
  * key -> TG_TX_DELAY_S (rig T/R switch) -> fade in; on release: drain, fade out, TG_HANG_S, unkey
  * unkeys immediately on: control connection loss, heartbeat older than TG_HB_TIMEOUT_S, no mic
    audio within TG_FIRST_AUDIO_S of keying or a mid-stream audio gap over TG_GAP_S while PTT is
    held (silence is played during a shorter gap, never stale audio), TG_TX_TIMEOUT_S, tx_enabled removed, radio-bridge
    starting, SIGTERM/SIGINT/SIGHUP. After a TX timeout PTT must be released before re-keying.
  * while keyed a marker file exists (TG_MARK); `--unkey-if-marked` (systemd ExecStopPost) unkeys
    after a crash or SIGKILL and only when that marker exists, so it never unkeys fldigi etc.

Control protocol (TCP, one line per message, UTF-8):
  server: "FT710-TXGATE 1 <nonce>"          client: "AUTH <hex HMAC-SHA256(token, nonce)>"
  server: "OK" | "ERR <why>" (then closes)
  client: "HB" (every ~100 ms; any line counts), "PTT 1", "PTT 0", "BYE"
  server: "STATE k=v ..." (every 250 ms and on change), "EVENT <text>"
Audio (UDP, same port): RTP, PT 96 = L16/48000/1 (network byte order), PT 97 = Opus/48000/1,
accepted only from the authenticated controller's address. A new authenticated controller
replaces the old one (the rig is unkeyed first).
"""
import argparse, array, collections, ctypes, ctypes.util, hashlib, hmac, ipaddress, logging, math, os
import secrets, selectors, signal, socket, struct, subprocess, sys, threading, time, xmlrpc.client

HOME = os.path.expanduser("~")
E = os.environ.get
def _f(name, rb, default): return float(E(name, E(rb, default) if rb else default))

CFG = os.path.expanduser(E("TG_CONFIG_DIR", E("RB_CONFIG_DIR", os.path.join(HOME, ".config", "radio-bridge"))))
TX_FLAG = os.path.join(CFG, "tx_enabled")
TX_GAIN_FILE = os.path.join(CFG, "tx_gain")            # same file the app's TX level slider writes
BRIDGE_MARK = os.path.expanduser(E("TG_BRIDGE_MARK", os.path.join(HOME, ".cache", "radio-bridge", "keyed")))
MARK = os.path.expanduser(E("TG_MARK", os.path.join(HOME, ".cache", "ft710-txgate", "keyed")))
TOKEN_FILE = os.path.expanduser(E("TG_TOKEN_FILE", os.path.join(HOME, ".config", "ft710-txgate", "token")))
FLRIG_URL = E("TG_FLRIG_URL", E("RB_FLRIG_URL", "http://127.0.0.1:12345/RPC2"))
SINK = E("TG_SINK", E("RB_SINK", ""))
BIND = E("TG_BIND", "0.0.0.0")
PORT = int(E("TG_PORT", "10010"))
ALLOW = [ipaddress.ip_network(n.strip(), strict=False)
         for n in E("TG_ALLOW", "127.0.0.0/8,::1/128").split(",") if n.strip()]
BRIDGE_UNIT = E("TG_BRIDGE_UNIT", "radio-bridge.service")   # empty = don't check
SAFE = E("TG_SAFE", "0") not in ("", "0", "no", "false")
HANG_S = _f("TG_HANG_S", "RB_HANG_S", "0.7")
TX_TIMEOUT_S = _f("TG_TX_TIMEOUT_S", "RB_TX_TIMEOUT_S", "120")
TX_DELAY_S = _f("TG_TX_DELAY_S", "RB_TX_DELAY_S", "0.07")
FADE_S = _f("TG_FADE_S", "RB_FADE_S", "0.015")
PREBUF_S = _f("TG_PREBUF_S", None, "0.04")
LEAD_S = _f("TG_LEAD_S", "RB_LEAD_S", "0.06")
MAX_BUF_S = _f("TG_MAX_BUF_S", None, "0.3")
HB_TIMEOUT_S = _f("TG_HB_TIMEOUT_S", None, "0.5")
GAP_S = _f("TG_GAP_S", None, "2.0")              # mid-stream audio gap while PTT is held
FIRST_AUDIO_S = _f("TG_FIRST_AUDIO_S", None, "2.0")   # first mic packet after keying
PA_LATENCY_MS = int(E("TG_PA_LATENCY_MS", "60"))
TX_GAIN_DEFAULT = float(E("RB_TX_GAIN", "1.0"))
RATE, TX_FRAME = 48000, 480          # 48 kHz mono s16le, 10 ms write unit
TX_FRAME_BYTES = TX_FRAME * 2
TICK_S = 0.005
LATE_PKT_S = 0.1                     # audio still accepted this long after "PTT 0" (in flight)
PT_L16, PT_OPUS = 96, 97

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("ft710-txgate")


def _secs(nbytes): return nbytes / 2.0 / RATE


class Gain:
    """Linear gain with a soft limiter (same curve as radio-bridge). Re-reads tx_gain when it changes."""
    KNEE, LIMIT = 24000.0, 32767.0

    def __init__(self, path=TX_GAIN_FILE, default=TX_GAIN_DEFAULT, max_gain=20.0, period=1.0):
        self.path, self.default, self.max_gain, self.period = path, default, max_gain, period
        self.state, self._mtime, self._checked, self._building = (None, None), "unset", 0.0, False
        self.refresh(force=True)

    def _lut(self, g):
        k, span = self.KNEE, self.LIMIT - self.KNEE
        lut = [0] * 65536
        for x in range(-32768, 32768):
            y = abs(x) * g
            if y > k:
                y = k + span * math.tanh((y - k) / span)
            lut[x] = int(round(math.copysign(min(y, self.LIMIT), x)))
        return lut

    def refresh(self, force=False):
        now = time.monotonic()
        if not force and now - self._checked < self.period:
            return
        self._checked = now
        try:
            mt = os.stat(self.path).st_mtime
        except OSError:
            mt = None
        if mt == self._mtime and not force:
            return
        self._mtime = mt
        g = self.default
        if mt is not None:
            try:
                g = float(open(self.path).read().strip())
                if not math.isfinite(g):
                    raise ValueError
            except (OSError, ValueError):
                g = self.state[0] if self.state[0] is not None else self.default
        g = max(0.0, min(g, self.max_gain))
        if g != self.state[0] and not self._building:
            if self.state[0] is None:
                self._install(g)
            else:   # build the table off the audio thread; swap in when ready
                self._building = True
                threading.Thread(target=self._install, args=(g,), daemon=True).start()

    def _install(self, g):
        try:
            self.state = (g, None if g == 1.0 else self._lut(g))
            log.info("TX gain %.3f", g)
        finally:
            self._building = False

    def apply(self, pcm):
        g, lut = self.state
        if lut is None:
            return pcm
        a = array.array("h"); a.frombytes(pcm)
        return array.array("h", [lut[x] for x in a]).tobytes()


def fade(pcm, start, total, rising):
    a = array.array("h"); a.frombytes(pcm[: len(pcm) & ~1])
    for i in range(len(a)):
        p = min(1.0, (start + i + 1) / float(total))
        a[i] = int(a[i] * (p if rising else 1.0 - p))
    return a.tobytes()


class OpusDecoder:
    def __init__(self):
        name = ctypes.util.find_library("opus") or "libopus.so.0"
        self.lib = ctypes.CDLL(name)
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
                                         ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
        err = ctypes.c_int()
        self.dec = self.lib.opus_decoder_create(RATE, 1, ctypes.byref(err))
        if err.value != 0 or not self.dec:
            raise RuntimeError("opus_decoder_create failed (%d)" % err.value)
        self.out = (ctypes.c_int16 * 5760)()

    def decode(self, data):
        n = self.lib.opus_decode(self.dec, data, len(data), self.out, 5760, 0)
        if n < 0:
            raise ValueError("opus_decode error %d" % n)
        return ctypes.string_at(self.out, n * 2)


class _TimeoutTransport(xmlrpc.client.Transport):
    def make_connection(self, host):
        c = super().make_connection(host)
        c.timeout = 2.0
        return c


class Rig:
    def __init__(self, url=FLRIG_URL):
        self.url = url

    def _proxy(self):   # fresh proxy per call: a timed-out call can't leave a half-read connection
        return xmlrpc.client.ServerProxy(self.url, transport=_TimeoutTransport())

    def set_ptt(self, on):
        self._proxy().rig.set_ptt(1 if on else 0)

    def get_ptt(self):
        return int(self._proxy().rig.get_ptt())


class Player:
    """pacat into the radio sink; started at key-down, stopped after unkey."""
    def __init__(self, sink=SINK):
        self.sink, self.p = sink, None

    def start(self):
        self.p = subprocess.Popen(
            ["pacat", "--playback", "--raw", "--device=" + self.sink, "--format=s16le", "--rate=%d" % RATE,
             "--channels=1", "--latency-msec=%d" % PA_LATENCY_MS, "--client-name=ft710-txgate",
             "--stream-name=FT-710 Remote TX", "--volume=65536", "--property=state.restore-props=false"],
            stdin=subprocess.PIPE)

    def write(self, pcm):
        if self.p is None or self.p.poll() is not None:
            return False
        try:
            self.p.stdin.write(pcm); self.p.stdin.flush()
            return True
        except (BrokenPipeError, OSError):
            return False

    def stop(self, drain=False):
        p, self.p = self.p, None
        if p is None:
            return
        try:
            if drain:
                p.stdin.close()
                p.wait(timeout=0.5)
        except Exception:
            pass
        if p.poll() is None:
            p.kill()
        try:
            p.wait(timeout=1)
        except Exception:
            pass


def mark(path, on):
    try:
        if on:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w").close()
        else:
            os.remove(path)
    except FileNotFoundError:
        pass


def bridge_active():
    if not BRIDGE_UNIT:
        return False
    try:
        return subprocess.run(["systemctl", "--user", "is-active", "--quiet", BRIDGE_UNIT],
                              timeout=2).returncode == 0
    except Exception:
        return True          # can't tell: assume it is, so we don't fight it


class Gate:
    IDLE, KEYED = "idle", "keyed"
    HOLD, PLAY, GAP, DRAIN = "hold", "play", "gap", "drain"

    def __init__(self, rig, player, gain, clock=time.monotonic, bridge_check=bridge_active):
        self.rig, self.player, self.gain, self.clock, self.bridge_check = rig, player, gain, clock, bridge_check
        self.lock = threading.Lock()
        self.events = collections.deque()      # outgoing EVENT lines
        self.inq = collections.deque()         # (arrival, pcm) from the network thread
        self.ctrl_id = 0                       # current controller (0 = none)
        self.ctrl_hb = 0.0
        self.ptt_req = False
        self.ptt_off_t = 0.0
        self.last_pkt = 0.0
        self.state, self.sub = self.IDLE, None
        self.latched = None                    # reason keying is refused until PTT is released
        self.unkey_pending = False
        self.flags = {"tx_enabled": False, "bridge": False, "bridge_keyed": False}
        self.flags_t = -1e9
        self.rig_ptt = "?"
        self.last_reason = ""
        self.buf = bytearray()
        self.stats = {"overs": 0, "pkts": 0, "underruns": 0}

    # ---- called from the network thread ----
    def event(self, text):
        log.info("event: %s", text)
        self.last_reason = text
        self.events.append(text)

    def controller(self, cid):
        with self.lock:
            self.ctrl_id, self.ctrl_hb, self.ptt_req = cid, self.clock(), False

    def heartbeat(self, cid):
        if cid == self.ctrl_id:
            self.ctrl_hb = self.clock()

    def ptt(self, cid, on):
        if cid != self.ctrl_id:
            return
        with self.lock:
            self.ctrl_hb = self.clock()
            if on and not self.ptt_req:
                self.inq.clear()
            if not on and self.ptt_req:
                self.ptt_off_t = self.clock()
            self.ptt_req = on

    def audio(self, cid, pcm):
        now = self.clock()
        if cid != self.ctrl_id:
            return
        if not self.ptt_req and now - self.ptt_off_t > LATE_PKT_S:
            return                              # PTT not held: mic audio is dropped here
        self.inq.append((now, pcm))

    # ---- engine thread ----
    def refresh_flags(self, now, force=False):
        if not force and now - self.flags_t < (0.25 if self.state == self.KEYED or self.ptt_req else 1.0):
            return
        self.flags_t = now
        self.flags = {"tx_enabled": os.path.exists(TX_FLAG), "bridge": self.bridge_check(),
                      "bridge_keyed": os.path.exists(BRIDGE_MARK)}

    def _set_ptt(self, on):
        t0 = self.clock()
        try:
            self.rig.set_ptt(on)
        except Exception as e:
            log.error("flrig set_ptt(%d) failed: %s", on, e)
            return False
        ms = (self.clock() - t0) * 1000
        (log.warning if ms > 250 else log.info)("flrig set_ptt(%d) %.0f ms", on, ms)
        return True

    def refuse_reason(self):
        f = self.flags
        if SAFE: return "safe mode (TG_SAFE): keying disabled"
        if not f["tx_enabled"]: return "TX keying is off (tx_enabled)"
        if f["bridge"]: return "radio-bridge is running; it owns PTT"
        if f["bridge_keyed"]: return "radio-bridge has the rig keyed"
        if not SINK: return "no TX sink configured (TG_SINK / RB_SINK)"
        return None

    def _key(self, now):
        self.refresh_flags(now, force=True)
        why = self.refuse_reason()
        if why:
            self.latched = why
            self.event("refused: " + why)
            return
        mark(MARK, True)
        if not self._set_ptt(True):
            self.latched = "flrig set_ptt failed"
            self.event("refused: flrig set_ptt(1) failed")
            self._unkey("flrig set_ptt(1) failed")
            return
        now = self.clock()
        self.state, self.sub = self.KEYED, self.HOLD
        self.t_key = self.t0 = now
        self.key_ctrl = self.ctrl_id
        self.written, self.fade_pos = 0, 0
        self.buf = bytearray()
        self.hang_until = None
        self.delay_done = False
        self.stats["overs"] += 1
        try:
            self.player.start()
        except Exception as e:
            self._unkey("can't start playback: %s" % e)
            self.latched = "playback failed"
            return
        self.event("keyed")

    def _unkey(self, why):
        was = self.state == self.KEYED
        self.state, self.sub = self.IDLE, None
        if was or os.path.exists(MARK):      # never unkey a rig we did not key (fldigi etc.)
            ok = self._set_ptt(False) or self._set_ptt(False)
            if ok:
                mark(MARK, False); self.unkey_pending = False
            else:
                self.unkey_pending = True
                self.event("UNKEY FAILED (flrig) — retrying; check the radio")
        self.player.stop(drain=False)
        self.buf = bytearray()
        if was:
            self.event("unkeyed: " + why)
            if why != "released" and self.latched is None:
                self.latched = why + ": release PTT to key again"

    def emergency_all(self, why):
        """Signal handler / shutdown: unkey now, whatever the state."""
        self._unkey(why)

    def _pull(self):
        while self.inq:
            t, pcm = self.inq.popleft()
            self.last_pkt = t
            if self.state == self.KEYED:
                self.buf += self.gain.apply(pcm)
                self.stats["pkts"] += 1
        cap = int(MAX_BUF_S * RATE) * 2
        if len(self.buf) > cap:
            drop = len(self.buf) - int(PREBUF_S * RATE) * 2
            del self.buf[: drop - drop % 2]

    def _write(self, pcm):
        if not self.player.write(pcm):
            raise OSError("playback stream died")
        self.written += len(pcm) // 2

    def _frame(self):
        n, fade_n = TX_FRAME_BYTES, max(1, int(FADE_S * RATE))
        if self.sub == self.PLAY or (self.sub == self.DRAIN and self.buf and self.delay_done):
            if len(self.buf) >= 2 * n or (self.sub == self.DRAIN and len(self.buf) >= n + 2 * fade_n):
                pcm = bytes(self.buf[:n]); del self.buf[:n]
                if self.fade_pos < fade_n:
                    pcm = fade(pcm, self.fade_pos, fade_n, True); self.fade_pos += len(pcm) // 2
                return self._write(pcm)
            rest = bytes(self.buf); self.buf.clear()   # running dry: fade out what is left
            if rest:
                rest = fade(rest, 0, len(rest) // 2, False)
            if self.sub == self.PLAY:
                self.stats["underruns"] += 1; self.sub = self.GAP
            return self._write(rest + b"\0" * max(0, n - len(rest)))
        self._write(b"\0" * n)

    def tick(self):
        now = self.clock()
        self.refresh_flags(now)
        self.gain.refresh()
        if self.unkey_pending and now - getattr(self, "_retry_t", 0) > 0.5:
            self._retry_t = now
            self._unkey("retry")
        with self.lock:
            ctrl, hb, ptt, ptt_off = self.ctrl_id, self.ctrl_hb, self.ptt_req, self.ptt_off_t
        if self.state == self.IDLE:
            if not ptt:
                self.latched = None
            elif ctrl and self.latched is None and now - hb <= HB_TIMEOUT_S:
                self._key(now)
            self.inq.clear() if self.state == self.IDLE else None
            return
        # ---- keyed ----
        self._pull()
        self.delay_done = now - self.t_key >= TX_DELAY_S
        f, why = self.flags, None
        if not ctrl: why = "control connection lost"
        elif ctrl != self.key_ctrl: why = "controller replaced"
        elif now - hb > HB_TIMEOUT_S: why = "heartbeat timeout (%.0f ms)" % ((now - hb) * 1000)
        elif now - self.t_key > TX_TIMEOUT_S: why = "TX timeout (%.0f s)" % TX_TIMEOUT_S; self.latched = "TX timeout: release PTT"
        elif not f["tx_enabled"]: why = "TX keying turned off"
        elif f["bridge"]: why = "radio-bridge started"
        elif ptt and self.last_pkt < self.t_key and now - self.t_key > FIRST_AUDIO_S:
            why = "no mic audio within %.1f s of keying" % FIRST_AUDIO_S
        elif ptt and self.last_pkt >= self.t_key and now - self.last_pkt > GAP_S:
            why = "mic audio gap over %.1f s" % GAP_S
        if why:
            return self._unkey(why)
        if ptt:
            if self.sub in (self.DRAIN,) or self.hang_until is not None:
                self.sub, self.hang_until, self.fade_pos = self.GAP, None, 0
            if self.sub in (self.HOLD, self.GAP) and now - self.t_key >= TX_DELAY_S and \
                    len(self.buf) >= int(PREBUF_S * RATE) * 2:
                self.sub, self.fade_pos = self.PLAY, 0
        else:
            if self.sub != self.DRAIN:
                self.sub = self.DRAIN
            if self.hang_until is None and not self.buf and now - ptt_off > LATE_PKT_S:
                # all audio handed to pacat; it still holds ~lead + its own latency
                self.hang_until = now + max(HANG_S, LEAD_S + PA_LATENCY_MS / 1000.0 + 0.03)
            if self.hang_until is not None and now >= self.hang_until:
                return self._unkey("released")
        try:
            while self.written / float(RATE) - (now - self.t0) < LEAD_S:
                self._frame()
        except OSError as e:
            self._unkey(str(e))

    def status(self):
        f = self.flags
        return ("STATE keyed=%d ptt=%s tx_enabled=%d bridge=%d safe=%d sub=%s pkts=%d underruns=%d latched=%s" % (
            self.state == self.KEYED, self.rig_ptt, f["tx_enabled"], f["bridge"], SAFE, self.sub or "-",
            self.stats["pkts"], self.stats["underruns"], (self.latched or "-").replace(" ", "_")))


class Net(threading.Thread):
    def __init__(self, gate, token, bind=BIND, port=PORT):
        super().__init__(daemon=True)
        self.gate, self.token = gate, token
        self.sel = selectors.DefaultSelector()
        fam = socket.AF_INET6 if ":" in bind else socket.AF_INET
        self.lsock = socket.socket(fam, socket.SOCK_STREAM)
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind((bind, port)); self.lsock.listen(4); self.lsock.setblocking(False)
        self.usock = socket.socket(fam, socket.SOCK_DGRAM)
        self.usock.bind((bind, port)); self.usock.setblocking(False)
        self.sel.register(self.lsock, selectors.EVENT_READ, "listen")
        self.sel.register(self.usock, selectors.EVENT_READ, "udp")
        self.clients = {}          # sock -> dict(nonce, buf, authed, id, addr, t)
        self.ctrl = None           # authenticated controller socket
        self.next_id = 1
        self.opus = None
        self.last_seq = None
        self.last_state, self.state_t = "", 0.0

    def _allowed(self, ip):
        try:
            a = ipaddress.ip_address(ip)
            if a.version == 6 and a.ipv4_mapped:
                a = a.ipv4_mapped
            return any(a in n for n in ALLOW)
        except ValueError:
            return False

    def _send(self, s, line):
        try:
            s.sendall((line + "\n").encode())
        except OSError:
            self._drop(s, "send failed")

    def _drop(self, s, why):
        c = self.clients.pop(s, None)
        try: self.sel.unregister(s)
        except Exception: pass
        try: s.close()
        except Exception: pass
        if c and s is self.ctrl:
            self.ctrl = None
            self.gate.controller(0)
            log.info("controller %s disconnected (%s)", c["addr"][0], why)

    def _accept(self):
        s, addr = self.lsock.accept()
        if not self._allowed(addr[0]):
            log.warning("rejected %s (not in TG_ALLOW)", addr[0]); s.close(); return
        s.setblocking(False)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        nonce = secrets.token_hex(16)
        self.clients[s] = {"nonce": nonce, "buf": b"", "authed": False, "addr": addr, "t": time.monotonic(), "id": 0}
        self.sel.register(s, selectors.EVENT_READ, "client")
        self._send(s, "FT710-TXGATE 1 " + nonce)

    def _line(self, s, c, line):
        parts = line.split()
        if not parts:
            return
        if not c["authed"]:
            want = hmac.new(self.token, c["nonce"].encode(), hashlib.sha256).hexdigest()
            if len(parts) == 2 and parts[0] == "AUTH" and hmac.compare_digest(parts[1], want):
                c["authed"], c["id"] = True, self.next_id
                self.next_id += 1
                if self.ctrl is not None:
                    old = self.ctrl
                    self._send(old, "EVENT replaced by a new controller")
                    self._drop(old, "replaced")
                self.ctrl = s
                self.gate.controller(c["id"])
                self.last_seq = None
                log.info("controller %s authenticated", c["addr"][0])
                self._send(s, "OK")
                self.last_state = ""
            else:
                log.warning("auth failed from %s", c["addr"][0])
                self._send(s, "ERR auth"); self._drop(s, "auth failed")
            return
        cmd = parts[0]
        if cmd == "HB":
            self.gate.heartbeat(c["id"])
        elif cmd == "PTT" and len(parts) == 2:
            self.gate.ptt(c["id"], parts[1] == "1")
        elif cmd == "BYE":
            self._drop(s, "bye")
        else:
            self.gate.heartbeat(c["id"])

    def _client(self, s):
        c = self.clients.get(s)
        if c is None:
            return
        try:
            d = s.recv(4096)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            d = b""
        if not d:
            return self._drop(s, "closed")
        c["buf"] += d
        if len(c["buf"]) > 65536:
            return self._drop(s, "line too long")
        while b"\n" in c["buf"] and s in self.clients:
            line, c["buf"] = c["buf"].split(b"\n", 1)
            self._line(s, c, line.decode("utf-8", "replace").strip())

    def _udp(self):
        for _ in range(64):
            try:
                pkt, addr = self.usock.recvfrom(4096)
            except (BlockingIOError, InterruptedError):
                return
            c = self.clients.get(self.ctrl) if self.ctrl else None
            if c is None or addr[0] != c["addr"][0] or len(pkt) < 12 or pkt[0] >> 6 != 2:
                continue
            cc, pt = pkt[0] & 0x0F, pkt[1] & 0x7F
            seq = struct.unpack("!H", pkt[2:4])[0]
            off = 12 + 4 * cc
            if pkt[0] & 0x10 and len(pkt) >= off + 4:
                off += 4 + 4 * struct.unpack("!H", pkt[off + 2:off + 4])[0]
            end = len(pkt) - (pkt[-1] if pkt[0] & 0x20 else 0)
            payload = pkt[off:end]
            if self.last_seq is not None and 0 < (self.last_seq - seq) & 0xFFFF < 1000:
                continue                       # late / duplicate
            self.last_seq = seq
            try:
                if pt == PT_L16:
                    a = array.array("h"); a.frombytes(payload[: len(payload) & ~1])
                    if sys.byteorder == "little":
                        a.byteswap()
                    pcm = a.tobytes()
                elif pt == PT_OPUS:
                    if self.opus is None:
                        self.opus = OpusDecoder()
                    pcm = self.opus.decode(payload)
                else:
                    continue
            except Exception as e:
                log.warning("bad audio packet: %s", e); continue
            self.gate.audio(c["id"], pcm)

    def run(self):
        while True:
            for key, _ in self.sel.select(timeout=0.05):
                if key.data == "listen": self._accept()
                elif key.data == "udp": self._udp()
                else: self._client(key.fileobj)
            now = time.monotonic()
            for s, c in list(self.clients.items()):
                if not c["authed"] and now - c["t"] > 5:
                    self._drop(s, "auth timeout")
            if self.ctrl is not None:
                while self.gate.events:
                    self._send(self.ctrl, "EVENT " + self.gate.events.popleft())
                st = self.gate.status()
                if self.ctrl is not None and (st != self.last_state or now - self.state_t >= 0.25):
                    self.last_state, self.state_t = st, now
                    self._send(self.ctrl, st)
            else:
                self.gate.events.clear()


def poll_rig(gate, rig):
    """flrig PTT readout for the app (0.25 s while a controller is connected, else every 2 s)."""
    last = 0.0
    while True:
        if gate.ctrl_id or time.monotonic() - last >= 2.0:
            last = time.monotonic()
            try:
                gate.rig_ptt = str(rig.get_ptt())
            except Exception:
                gate.rig_ptt = "?"
        time.sleep(0.25)


def load_token():
    try:
        t = open(TOKEN_FILE).read().strip()
        if len(t) >= 32:
            return t.encode()
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(TOKEN_FILE), exist_ok=True)
    old = os.umask(0o077)
    try:
        t = secrets.token_hex(32)
        with open(TOKEN_FILE, "w") as f:
            f.write(t + "\n")
    finally:
        os.umask(old)
    log.info("created a new control token in %s", TOKEN_FILE)
    return t.encode()


def unkey_if_marked():
    """systemd ExecStopPost safety net: only acts when our keyed marker exists."""
    if not os.path.exists(MARK):
        return 0
    for _ in range(3):
        try:
            Rig().set_ptt(False)
            mark(MARK, False)
            print("ft710-txgate: unkeyed after an unclean stop")
            return 0
        except Exception as e:
            print("ft710-txgate: unkey failed: %s" % e); time.sleep(0.3)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--unkey-if-marked", action="store_true", help="unkey via flrig if our keyed marker exists, then exit")
    a = ap.parse_args()
    if a.unkey_if_marked:
        sys.exit(unkey_if_marked())
    rig = Rig()
    if os.path.exists(MARK):
        log.warning("keyed marker left from a previous run: unkeying")
        unkey_if_marked()
    gate = Gate(rig, Player(), Gain())
    stop = threading.Event()
    def on_sig(signum, _f):
        stop.set()
    for sg in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sg, on_sig)
    net = Net(gate, load_token())
    net.start()
    threading.Thread(target=poll_rig, args=(gate, rig), daemon=True).start()
    log.info("listening on %s:%d tcp+udp (allow %s) sink=%s flrig=%s safe=%s hang=%.2fs timeout=%.0fs first_audio=%.1fs gap=%.1fs",
             BIND, PORT, ",".join(map(str, ALLOW)), SINK or "-", FLRIG_URL, SAFE, HANG_S, TX_TIMEOUT_S, FIRST_AUDIO_S, GAP_S)
    last = time.monotonic()
    try:
        while not stop.is_set():
            gate.tick()
            if time.monotonic() - last > 60:
                last = time.monotonic()
                log.info("status %s", gate.status())
            time.sleep(TICK_S)
    finally:
        gate.emergency_all("service stopping")
        log.info("stopped")


if __name__ == "__main__":
    main()
