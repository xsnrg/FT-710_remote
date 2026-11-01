#!/usr/bin/env python3
"""Offline tests for ft710_txgate.py. Never touches a real radio:
  * flrig is a local XML-RPC stub that records every set_ptt call
  * pacat is replaced by a stub on PATH that records what would be played
    (or, with --null-sink, a temporary PipeWire null sink is used for one real-playback test)
  * all state files live in a temporary directory
Run: python3 test_txgate.py [--null-sink]
"""
import array, ctypes, ctypes.util, hashlib, hmac, json, math, os, signal, socket, struct, subprocess, sys
import tempfile, threading, time, xmlrpc.server

HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, "ft710_txgate.py")
TMP = tempfile.mkdtemp(prefix="txgate-test-")
FLRIG_PORT, GATE_PORT = 18345, 18010
CALLS = []                      # (time, value) of set_ptt
FAIL = {"set": False}
RIG = {"ptt": 0}


def flrig_stub():
    s = xmlrpc.server.SimpleXMLRPCServer(("127.0.0.1", FLRIG_PORT), logRequests=False, allow_none=True)
    def set_ptt(v):
        CALLS.append((time.monotonic(), int(v)))
        if FAIL["set"]:
            raise Exception("stub failure")
        RIG["ptt"] = int(v)
        return 0
    s.register_function(set_ptt, "rig.set_ptt")
    s.register_function(lambda: RIG["ptt"], "rig.get_ptt")
    threading.Thread(target=s.serve_forever, daemon=True).start()


FAKE_PACAT = r'''#!/usr/bin/env python3
import sys, time, os
log = open(os.environ["FAKE_PACAT_LOG"], "a")
log.write("start %f %s\n" % (time.monotonic(), " ".join(sys.argv[1:]))); log.flush()
while True:
    d = sys.stdin.buffer.read1(65536)
    if not d: break
    import array; a = array.array("h"); a.frombytes(d[:len(d)&~1])
    log.write("data %f %d %d\n" % (time.monotonic(), len(a), max((abs(x) for x in a), default=0))); log.flush()
log.write("end %f\n" % time.monotonic())
'''


class Gate:
    def __init__(self, name, **env):
        self.dir = os.path.join(TMP, name); os.makedirs(self.dir)
        self.cfg = os.path.join(self.dir, "cfg"); os.makedirs(self.cfg)
        open(os.path.join(self.cfg, "tx_enabled"), "w").close()
        self.mark = os.path.join(self.dir, "keyed")
        self.bmark = os.path.join(self.dir, "bridge_keyed")
        self.pacat_log = os.path.join(self.dir, "pacat.log")
        self.env = dict(os.environ, TG_CONFIG_DIR=self.cfg, TG_MARK=self.mark, TG_BRIDGE_MARK=self.bmark,
                        TG_TOKEN_FILE=os.path.join(self.dir, "token"), TG_FLRIG_URL="http://127.0.0.1:%d/RPC2" % FLRIG_PORT,
                        TG_SINK="ft710_txgate_test", TG_BIND="127.0.0.1", TG_PORT=str(GATE_PORT),
                        TG_BRIDGE_UNIT="ft710-txgate-test-nonexistent.service", TG_ALLOW="127.0.0.0/8",
                        FAKE_PACAT_LOG=self.pacat_log, PYTHONUNBUFFERED="1")
        self.env.pop("RB_SINK", None); self.env.pop("RB_FLRIG_URL", None)
        if env.pop("real_pacat", False) is False:
            self.env["PATH"] = os.path.join(TMP, "bin") + ":" + self.env["PATH"]
        self.env.update({k: str(v) for k, v in env.items()})
        self.p = None

    def start(self, extra=()):
        self.log = open(os.path.join(self.dir, "gate.log"), "w")
        self.p = subprocess.Popen([sys.executable, GATE, *extra], env=self.env, stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", GATE_PORT), timeout=0.2).close(); return self
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("gate did not start: " + open(self.log.name).read())

    def stop(self, sig=signal.SIGTERM):
        if self.p and self.p.poll() is None:
            self.p.send_signal(sig); self.p.wait(5)

    def token(self):
        return open(self.env["TG_TOKEN_FILE"]).read().strip().encode()


class Client:
    def __init__(self, gate, token=None, opus=False):
        self.s = socket.create_connection(("127.0.0.1", GATE_PORT), timeout=2)
        self.f = self.s.makefile("r")
        hello = self.f.readline().split()
        tok = token if token is not None else gate.token()
        self.send("AUTH " + hmac.new(tok, hello[2].encode(), hashlib.sha256).hexdigest())
        self.reply = self.f.readline().strip()
        self.lines = []
        self.u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq, self.ts = 0, 0
        self.hb_on = self.audio_on = False
        self.alive = self.reply == "OK"
        self.opus = Opus() if opus else None
        if self.alive:
            threading.Thread(target=self._reader, daemon=True).start()
            threading.Thread(target=self._pump, daemon=True).start()

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def _reader(self):
        try:
            for line in self.f:
                self.lines.append((time.monotonic(), line.strip()))
        except Exception:
            pass

    def _pump(self):
        nxt = time.monotonic()
        k = 0
        while self.alive:
            try:
                if self.hb_on and k % 10 == 0:
                    self.send("HB")
                if self.audio_on:
                    pcm = array.array("h", [int(8000 * math.sin(2 * math.pi * 1000 * (self.ts + i) / 48000)) for i in range(480)])
                    if self.opus:
                        payload, pt = self.opus.enc(pcm.tobytes()), 97
                    else:
                        pcm.byteswap(); payload, pt = pcm.tobytes(), 96
                    hdr = struct.pack("!BBHII", 0x80, pt, self.seq & 0xFFFF, self.ts & 0xFFFFFFFF, 0x1234)
                    self.u.sendto(hdr + payload, ("127.0.0.1", GATE_PORT))
                    self.seq += 1; self.ts += 480
            except OSError:
                return
            k += 1
            nxt += 0.01
            time.sleep(max(0, nxt - time.monotonic()))

    def events(self):
        return [l for _, l in self.lines if l.startswith("EVENT")]

    def close(self):
        self.alive = False
        try: self.f.close(); self.s.close()
        except OSError: pass


class Opus:
    def __init__(self):
        self.lib = ctypes.CDLL(ctypes.util.find_library("opus") or "libopus.so.0")
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.lib.opus_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int32]
        err = ctypes.c_int()
        self.e = self.lib.opus_encoder_create(48000, 1, 2048, ctypes.byref(err))
        self.out = ctypes.create_string_buffer(4000)

    def enc(self, pcm):
        n = self.lib.opus_encode(self.e, pcm, 480, self.out, 4000)
        return self.out.raw[:n]


def calls_since(t):
    return [(round(c - t, 3), v) for c, v in CALLS if c >= t]


RESULTS = []
def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def keyed_over(g, c, hold=1.0, audio=True):
    c.hb_on = True; c.audio_on = audio
    t = time.monotonic(); c.send("PTT 1")
    time.sleep(hold)
    return t


def pacat_audio(g):
    if not os.path.exists(g.pacat_log): return None, []
    st, data = None, []
    for l in open(g.pacat_log):
        p = l.split()
        if p[0] == "start": st = float(p[1])
        elif p[0] == "data": data.append((float(p[1]), int(p[2]), int(p[3])))
    return st, data


def main():
    os.makedirs(os.path.join(TMP, "bin"))
    with open(os.path.join(TMP, "bin", "pacat"), "w") as f: f.write(FAKE_PACAT)
    os.chmod(os.path.join(TMP, "bin", "pacat"), 0o755)
    flrig_stub()

    # 1 normal over: key, delay, fade-in, release -> drain -> hang -> unkey
    g = Gate("normal").start(); c = Client(g)
    check("auth ok", c.reply == "OK")
    t = keyed_over(g, c, 1.0)
    c.audio_on = False; tr = time.monotonic(); c.send("PTT 0"); time.sleep(1.2)
    cs = calls_since(t)
    check("normal: keyed then unkeyed once each", [v for _, v in cs] == [1, 0], str(cs))
    unkey_after = cs[-1][0] - (tr - t) if len(cs) == 2 else -1
    check("normal: unkey ~hang (0.7 s) after release", 0.65 <= unkey_after <= 0.95, "%.3f s" % unkey_after)
    st, data = pacat_audio(g)
    first = next((d for d in data if d[2] > 0), None)
    lead = first[0] - (t + cs[0][0]) if first else -1
    check("normal: first audio written >= TX delay after key (pacat buffers ahead ~60 ms)", first is not None and lead >= 0.0, "%.3f s" % lead)
    silent = sum(d[1] for d in data if d[0] < (t + cs[0][0]) + 0.07 and d[2] == 0)
    check("normal: >= 70 ms of silence primed before audio", silent >= 0.07 * 48000 * 0.9, "%d samples" % silent)
    check("normal: no marker left", not os.path.exists(g.mark))
    st_lines = [l for _, l in c.lines if l.startswith("STATE")]
    check("normal: STATE reports flrig ptt", any("ptt=1" in l for l in st_lines) and "ptt=0" in st_lines[-1], st_lines[-1] if st_lines else "none")
    c.close(); ts = time.monotonic(); g.stop()
    check("normal: clean stop issues no set_ptt", calls_since(ts) == [])

    # 2 bad token, 3 allowlist
    g = Gate("auth").start()
    c = Client(g, token=b"wrong"); check("bad token rejected", c.reply == "ERR auth", c.reply); c.close()
    g.stop()
    g = Gate("allow", TG_ALLOW="10.250.0.0/16").start()
    try:
        s = socket.create_connection(("127.0.0.1", GATE_PORT), timeout=1); d = s.recv(100); s.close()
        check("non-allowed subnet dropped", d == b"", repr(d))
    except OSError as e:
        check("non-allowed subnet dropped", True, str(e))
    g.stop()

    # 4 heartbeat loss
    g = Gate("hb").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); c.hb_on = False; th = time.monotonic(); time.sleep(1.0)
    cs = calls_since(t)
    d = cs[-1][0] - (th - t) if cs else -1
    check("heartbeat loss: unkeyed within ~0.5-0.7 s", [v for _, v in cs] == [1, 0] and d < 0.75, "%s after last HB %.3f s" % (cs, d))
    c.hb_on = True; time.sleep(0.5)
    check("heartbeat loss: no re-key while PTT still held", [v for _, v in calls_since(t)] == [1, 0])
    check("heartbeat loss: event sent", any("heartbeat timeout" in e for e in c.events()), str(c.events()))
    c.send("PTT 0"); c.close(); g.stop()

    # 5 connection loss
    g = Gate("conn").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); tc = time.monotonic(); c.close(); time.sleep(0.3)
    cs = calls_since(t)
    check("control connection loss: immediate unkey", [v for _, v in cs] == [1, 0] and cs[-1][0] - (tc - t) < 0.1, str(cs))
    g.stop()

    # 6a defaults: first mic audio may take up to TG_FIRST_AUDIO_S (2 s); short mid-over gaps play silence
    g = Gate("firstaudio").start(); c = Client(g)
    t = keyed_over(g, c, 1.5, audio=False)
    check("first audio: still keyed 1.5 s after key with no packets yet", [v for _, v in calls_since(t)] == [1])
    c.audio_on = True; time.sleep(0.8); c.audio_on = False; tg = time.monotonic(); time.sleep(1.2)
    check("mid-over gap of 1.2 s (< TG_GAP_S 2 s): still keyed", [v for _, v in calls_since(t)] == [1])
    st, data = pacat_audio(g)
    zero = sum(d[1] for d in data if d[0] > tg + 0.4 and d[2] == 0); loud = [d for d in data if d[0] > tg + 0.4 and d[2] > 0]
    check("mid-over gap: silence played, no stale audio", zero > 0.4 * 48000 and not loud, "%d zero samples, %d loud chunks" % (zero, len(loud)))
    time.sleep(1.1)
    cs = calls_since(t); d = cs[-1][0] - (tg - t) if len(cs) == 2 else -1
    check("mid-over gap > 2 s: unkey", [v for _, v in cs] == [1, 0] and 1.95 <= d <= 2.3, "%s gap->unkey %.3f s" % (cs, d))
    check("mid-over gap: event", any("mic audio gap over 2.0 s" in e for e in c.events()), str(c.events()))
    c.send("PTT 0"); time.sleep(0.2)
    t = keyed_over(g, c, 2.4, audio=False)
    cs = calls_since(t)
    check("no mic audio at all: unkey ~2 s after key", [v for _, v in cs] == [1, 0] and 1.95 <= cs[-1][0] - cs[0][0] <= 2.3, str(cs))
    check("no mic audio: event", any("no mic audio within 2.0 s" in e for e in c.events()), str(c.events()))
    c.send("PTT 0"); c.close(); g.stop()

    # 6b short configured gap (TG_GAP_S=0.2): latch and re-key behaviour
    g = Gate("gap", TG_GAP_S=0.2).start(); c = Client(g)
    t = keyed_over(g, c, 0.5); c.audio_on = False; tg = time.monotonic(); time.sleep(0.6)
    cs = calls_since(t)
    d = cs[-1][0] - (tg - t) if cs else -1
    check("audio gap > TG_GAP_S 200 ms: unkey", [v for _, v in cs] == [1, 0] and 0.18 <= d <= 0.3, "%s gap->unkey %.3f s" % (cs, d))
    c.audio_on = True; time.sleep(0.4)
    check("audio gap: latched until PTT released", [v for _, v in calls_since(t)] == [1, 0])
    c.send("PTT 0"); time.sleep(0.1); t2 = time.monotonic(); c.send("PTT 1"); time.sleep(0.3)
    check("audio gap: re-key after release+press", [v for _, v in calls_since(t2)] == [1])
    c.send("PTT 0"); c.audio_on = False; time.sleep(1.2)
    check("audio gap: final unkey", [v for _, v in calls_since(t2)] == [1, 0])
    c.close(); g.stop()

    # 7 TX timeout
    g = Gate("timeout", TG_TX_TIMEOUT_S=2).start(); c = Client(g)
    t = keyed_over(g, c, 3.0)
    cs = calls_since(t)
    check("TX timeout: unkeyed at ~2 s and not re-keyed while held", [v for _, v in cs] == [1, 0] and 1.95 <= cs[1][0] <= 2.2, str(cs))
    c.send("PTT 0"); c.audio_on = False; c.close(); g.stop()

    # 8 tx_enabled off, bridge marker, bridge running, safe mode
    g = Gate("txoff"); os.remove(os.path.join(g.cfg, "tx_enabled")); g.start(); c = Client(g)
    t = keyed_over(g, c, 0.5)
    check("tx_enabled absent: refused, no set_ptt", calls_since(t) == [] and any("tx_enabled" in e for e in c.events()), str(c.events()))
    c.send("PTT 0"); c.close(); g.stop()
    g = Gate("bmark"); open(g.bmark, "w").close(); g.start(); c = Client(g)
    t = keyed_over(g, c, 0.5)
    check("radio-bridge keyed marker: refused", calls_since(t) == [], str(c.events()))
    c.send("PTT 0"); c.close(); g.stop()
    g = Gate("bridge", TG_BRIDGE_UNIT="dbus.service").start(); c = Client(g)   # an always-active unit stands in
    t = keyed_over(g, c, 0.5)
    check("radio-bridge active: refused", calls_since(t) == [] and any("radio-bridge is running" in e for e in c.events()), str(c.events()))
    c.send("PTT 0"); c.close(); g.stop()
    g = Gate("safe", TG_SAFE=1).start(); c = Client(g)
    t = keyed_over(g, c, 0.5)
    check("safe mode: refused", calls_since(t) == [] and any("safe mode" in e for e in c.events()))
    c.send("PTT 0"); c.close(); g.stop()

    # 9 tx_enabled removed mid-over
    g = Gate("txoffmid").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); os.remove(os.path.join(g.cfg, "tx_enabled")); time.sleep(0.6)
    check("tx_enabled removed while keyed: unkey", [v for _, v in calls_since(t)] == [1, 0])
    c.send("PTT 0"); c.close(); g.stop()

    # 10 flrig set_ptt failure
    g = Gate("flrigfail").start(); c = Client(g)
    FAIL["set"] = True; t = keyed_over(g, c, 0.6); FAIL["set"] = False; time.sleep(0.6); tf = time.monotonic(); time.sleep(1.0)
    cs = calls_since(t)
    check("flrig failure: refused, unkey retried until flrig answers, then quiet", [v for _, v in cs][0] == 1 and
          set(v for _, v in cs[1:]) == {0} and calls_since(tf) == [] and not os.path.exists(g.mark), str(cs))
    c.send("PTT 0"); c.close(); g.stop()

    # 11 SIGTERM while keyed
    g = Gate("sigterm").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); ts = time.monotonic(); g.stop(signal.SIGTERM); time.sleep(0.2)
    cs = calls_since(t)
    check("SIGTERM while keyed: unkey", [v for _, v in cs] == [1, 0] and not os.path.exists(g.mark), str(cs))
    c.close()

    # 12 SIGKILL while keyed -> marker stays -> --unkey-if-marked unkeys (systemd ExecStopPost path)
    g = Gate("sigkill").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); g.stop(signal.SIGKILL); c.close()
    check("SIGKILL: marker left behind", os.path.exists(g.mark))
    subprocess.run([sys.executable, GATE, "--unkey-if-marked"], env=g.env, timeout=10)
    cs = calls_since(t)
    check("SIGKILL: --unkey-if-marked unkeyed", [v for _, v in cs] == [1, 0] and not os.path.exists(g.mark), str(cs))
    t = time.monotonic()
    subprocess.run([sys.executable, GATE, "--unkey-if-marked"], env=g.env, timeout=10)
    check("--unkey-if-marked without marker: no call (never unkeys fldigi etc.)", calls_since(t) == [])

    # 13 systemd: transient unit with the real ExecStopPost, killed with SIGKILL
    g = Gate("systemd")
    unit = "ft710-txgate-selftest"
    envargs = []
    for k in ("TG_CONFIG_DIR", "TG_MARK", "TG_BRIDGE_MARK", "TG_TOKEN_FILE", "TG_FLRIG_URL", "TG_SINK", "TG_BIND",
              "TG_PORT", "TG_BRIDGE_UNIT", "TG_ALLOW", "FAKE_PACAT_LOG", "PATH"):
        envargs += ["-E", "%s=%s" % (k, g.env[k])]
    r = subprocess.run(["systemd-run", "--user", "--unit=" + unit, "--collect", *envargs,
                        "-p", "ExecStopPost=%s %s --unkey-if-marked" % (sys.executable, GATE), sys.executable, GATE],
                       capture_output=True, text=True)
    if r.returncode != 0:
        check("systemd unit started", False, r.stderr.strip())
    else:
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", GATE_PORT), timeout=0.2).close(); break
            except OSError: time.sleep(0.05)
        c = Client(g); t = keyed_over(g, c, 0.5)
        subprocess.run(["systemctl", "--user", "kill", "-s", "KILL", unit]); time.sleep(1.5)
        cs = calls_since(t)
        check("systemd SIGKILL while keyed: ExecStopPost unkeyed", [v for _, v in cs] == [1, 0] and not os.path.exists(g.mark), str(cs))
        c.close()
        subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True)

    # 14 stale marker at startup
    g = Gate("stale"); open(g.mark, "w").close(); t = time.monotonic(); g.start(); time.sleep(0.3)
    check("stale marker at startup: unkey", [v for _, v in calls_since(t)] == [0] and not os.path.exists(g.mark))
    g.stop()

    # 15 controller replaced while keyed; 16 Opus audio
    g = Gate("replace").start(); c = Client(g)
    t = keyed_over(g, c, 0.5); c2 = Client(g); time.sleep(0.3)
    check("new controller replaces old: unkey", [v for _, v in calls_since(t)] == [1, 0])
    c.close(); c2.close(); g.stop()
    g = Gate("opus").start(); c = Client(g, opus=True)
    t = keyed_over(g, c, 1.0); c.audio_on = False; c.send("PTT 0"); time.sleep(1.1)
    st, data = pacat_audio(g)
    peak = max((d[2] for d in data), default=0)
    check("opus: audio decoded and played, normal unkey", [v for _, v in calls_since(t)] == [1, 0] and 6000 < peak < 9000, "peak %d" % peak)
    c.close(); g.stop()

    # 17 TX gain file applies
    g = Gate("gain"); open(os.path.join(g.cfg, "tx_gain"), "w").write("0.5\n"); g.start(); c = Client(g)
    t = keyed_over(g, c, 1.0); c.audio_on = False; c.send("PTT 0"); time.sleep(1.1)
    peak = max((d[2] for d in pacat_audio(g)[1]), default=0)
    check("tx_gain 0.5 halves the level", 3500 < peak < 4500, "peak %d" % peak)
    c.close(); g.stop()

    if "--null-sink" in sys.argv:
        null_sink_test()

    bad = [n for n, ok in RESULTS if not ok]
    print("\n%d/%d passed" % (len(RESULTS) - len(bad), len(RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
    sys.exit(1 if bad else 0)


def null_sink_test():
    """Real pacat into a temporary PipeWire null sink (unloaded afterwards); default sink is left alone."""
    before = subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
    mod = subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=ft710_txgate_test",
                          "sink_properties=device.description=ft710-txgate-test priority.session=0"],
                         capture_output=True, text=True).stdout.strip()
    try:
        time.sleep(0.5)
        mon = subprocess.Popen(["pw-record", "--target", "ft710_txgate_test", "--rate", "48000", "--channels", "1",
                                "--format", "s16", "-P", "{ stream.capture.sink = true }", "--raw", "-"], stdout=subprocess.PIPE)
        g = Gate("nullsink", real_pacat=True).start(); c = Client(g)
        t = keyed_over(g, c, 1.5); c.audio_on = False; c.send("PTT 0"); time.sleep(1.2)
        c.close(); g.stop()
        mon.terminate(); raw = mon.stdout.read()
        a = array.array("h"); a.frombytes(raw[: len(raw) & ~1])
        peak = max((abs(x) for x in a), default=0)
        check("null sink: real pacat played the tone, normal unkey", [v for _, v in calls_since(t)] == [1, 0] and peak > 6000, "peak %d, %.1f s captured" % (peak, len(a) / 48000))
    finally:
        if mod.isdigit():
            subprocess.run(["pactl", "unload-module", mod])
        after = subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
        check("null sink: removed, default sink unchanged", after == before and "ft710_txgate_test" not in
              subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout, "%s -> %s" % (before, after))


if __name__ == "__main__":
    main()
