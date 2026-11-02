#!/usr/bin/env python3
"""Offline tests for the TX gate's CAT reset (ft710_txgate.cat_reset). Never touches a real radio:
the FT-710 is a fake on a pseudo-terminal, flrig is a stub process / stub XML-RPC object, and the
restart command is replaced. Run: python3 test_catreset.py"""
import os, signal, subprocess, sys, tempfile, threading, time, select

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
TMP = tempfile.mkdtemp(prefix="catreset-test-")
os.environ["TG_MARK"] = os.path.join(TMP, "keyed")
os.environ["TG_CAT_LOCK"] = os.path.join(TMP, "catreset.lock")
import ft710_txgate as G

FAILS = []
def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond or not extra else "  [%s]" % extra))
    if not cond: FAILS.append(name)


class FakeRadio(threading.Thread):
    """FT-710 CAT on a pty. on: PS;->PS1; TX;->TX0;, PS0; powers off. off: silent; ';' then PS1;
    within 1 s powers on after boot_s. ignore_ps0 / dead simulate faults."""
    def __init__(self, boot_s=0.3, ignore_ps0=False, dead=False, on=True):
        super().__init__(daemon=True)
        self.m, self.s = os.openpty()
        self.path = os.ttyname(self.s)
        self.on, self.boot_s, self.ignore_ps0, self.dead = on, boot_s, ignore_ps0, dead
        self.rx, self.wake_t, self.boot_at, self.stop = [], 0.0, None, False
        self.start()

    def run(self):
        buf = b""
        while not self.stop:
            if self.boot_at and time.monotonic() >= self.boot_at:
                self.on, self.boot_at = True, None
            r, _, _ = select.select([self.m], [], [], 0.05)
            if not r: continue
            try: buf += os.read(self.m, 64)
            except OSError: return
            while b";" in buf:
                cmd, buf = buf[: buf.index(b";") + 1], buf[buf.index(b";") + 1:]
                self.rx.append(cmd)
                if self.dead: continue
                now = time.monotonic()
                if not self.on:
                    if cmd == b";": self.wake_t = now
                    elif cmd == b"PS1;" and now - self.wake_t < 1.0 and not self.boot_at:
                        self.boot_at = now + self.boot_s
                    continue
                if cmd == b"PS;": os.write(self.m, b"PS1;")
                elif cmd == b"TX;": os.write(self.m, b"TX0;")
                elif cmd == b"PS0;" and not self.ignore_ps0: self.on = False

    def close(self):
        self.stop = True; time.sleep(0.1)
        for fd in (self.m, self.s):
            try: os.close(fd)
            except OSError: pass


FAST = dict(off_wait=0.2, boot_wait=0.5, boot_max=4)
steps = []
say = steps.append

# ---- prefs / settings parsing ----
pf = os.path.join(TMP, "FT-710.prefs")
open(pf, "w").write("xcvr_name:FT-710\nxcvr_serial_port:/dev/serial/by-id/usb-Silicon_Labs_CP2105_Dual_USB_to_UART_B\n"
                    "+ridge_Controller_01AE870C-if00-port0\nserial_baudrate:7\nserial_stopbits:1\n")
pr = G.read_flrig_prefs(pf)
check("prefs: continued port line joined", pr["xcvr_serial_port"].endswith("UART_Bridge_Controller_01AE870C-if00-port0"), pr.get("xcvr_serial_port"))
port, baud, stop, src = G.cat_settings(pr, port_override="")
check("prefs: baud index 7 = 38400, 1 stop bit, from prefs", (baud, stop, src) == (38400, 1, "flrig prefs"), (baud, stop, src))
check("override wins", G.cat_settings(pr, port_override="/dev/x")[0] == "/dev/x")
byid = os.path.join(TMP, "usb-Silicon_Labs_CP2105_X-if00-port0"); open(byid, "w").close()
check("by-id fallback when prefs have none", G.cat_settings({}, "", os.path.join(TMP, "*CP2105*if00-port0"))[0] == byid)
try:
    G.cat_settings({"xcvr_serial_port": "NONE"}, "", os.path.join(TMP, "nothing*")); check("no port -> error", False)
except G.CatError: check("no port -> error", True)
check("missing prefs file -> {}", G.read_flrig_prefs(os.path.join(TMP, "nope")) == {})

# ---- serial safety ----
fr = FakeRadio()
cp = G.CatPort(fr.path, 38400, 1)
for bad in (b"TX1;", b"TX2;", b"MX1;", b"FA14074000;", b"MD0C;"):
    try: cp.write(bad); check("refuses to send %r" % bad, False)
    except G.CatError: check("refuses to send %r" % bad, True)
check("PS; query on fake radio", cp.query(b"PS;") == "PS1;")

# ---- the power-cycle sequence ----
steps.clear()
r = G.cat_power_cycle(cp, say, **FAST)
check("power cycle: ok summary", r == "radio power-cycled, PS1 TX0", r)
check("power cycle: radio went off then on", fr.on and b"PS0;" in fr.rx and fr.rx.index(b"PS0;") < fr.rx.index(b"PS1;"), fr.rx)
check("power cycle: wake ; then PS1;", fr.rx[fr.rx.index(b"PS1;") - 1] == b";", fr.rx)
check("power cycle: never TX1 / only allowed strings", all(c in G.CAT_ALLOWED for c in fr.rx), fr.rx)
check("power cycle: verified TX0", any("TX; -> TX0;" in s for s in steps), steps)
cp.close(); fr.close()

fr = FakeRadio(ignore_ps0=True); cp = G.CatPort(fr.path)
try: G.cat_power_cycle(cp, say, **FAST); check("radio ignoring PS0 -> error", False)
except G.CatError as e: check("radio ignoring PS0 -> error (no power-on sent)", "still answers" in str(e) and b"PS1;" not in fr.rx, str(e))
cp.close(); fr.close()

fr = FakeRadio(dead=True); cp = G.CatPort(fr.path)
t0 = time.monotonic()
try: G.cat_power_cycle(cp, say, off_wait=0.1, boot_wait=0.2, boot_max=2); check("dead CAT -> error", False)
except G.CatError as e: check("dead CAT -> error, bounded time", "no CAT reply" in str(e) and time.monotonic() - t0 < 10, str(e))
cp.close(); fr.close()

fr = FakeRadio(on=False); cp = G.CatPort(fr.path)
r = G.cat_power_cycle(cp, say, **FAST)
check("radio off at start -> powered on", fr.on and b"PS0;" not in fr.rx, fr.rx)
cp.close(); fr.close()

# ---- flrig restart command ----
fi_app = {"pid": 1, "argv": ["/usr/bin/flrig"], "cwd": "/home/u", "unit": "app-gnome-flrig-1.scope", "kind": "app",
          "env": {"DISPLAY": ":0", "WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": "/run/user/1000", "PATH": "/x"}}
c = G.flrig_restart_cmd(fi_app, now=42)
check("desktop flrig -> systemd-run --user in the graphical session",
      c[:2] == ["systemd-run", "--user"] and "--setenv=DISPLAY=:0" in c and "--setenv=WAYLAND_DISPLAY=wayland-0" in c
      and "--unit=app-flrig-catreset-42" in c and "--working-directory=/home/u" in c and c[-1] == "/usr/bin/flrig"
      and not any(x.startswith("--setenv=PATH") for x in c), c)
check("unit flrig -> systemctl --user start", G.flrig_restart_cmd(dict(fi_app, kind="unit", unit="flrig.service")) ==
      ["systemctl", "--user", "start", "flrig.service"])

# ---- full cat_reset with a stub flrig process ----
class StubRig:
    def __init__(self): self.calls = 0
    def get_ptt(self): self.calls += 1; return 0
    def set_ptt(self, on): raise AssertionError("cat_reset must never call set_ptt")

def run_reset(dry, radio, with_flrig=True, keyed=lambda: False):
    flr = subprocess.Popen(["sleep", "60"]) if with_flrig else None
    started = []
    G.flrig_find = lambda: (dict(fi_app, pid=flr.pid) if flr and flr.poll() is None else None)
    G.cat_settings = lambda prefs: (radio.path, 38400, 1, "test")
    G.flrig_restart_cmd = lambda fi, now=None: started.append(fi) or ["true"]
    G.CAT_OFF_WAIT_S, G.CAT_BOOT_WAIT_S, G.CAT_BOOT_MAX_S = 0.2, 0.5, 4
    orig = G.cat_power_cycle
    steps.clear()
    ok, summ = G.cat_reset(dry, say, rig=StubRig(), keyed=keyed)
    if flr and flr.poll() is None: flr.kill()
    if flr: flr.wait()
    return ok, summ, started, flr

fr = FakeRadio()
ok, summ, started, flr = run_reset(False, fr)
check("cat_reset: ok, flrig stopped (SIGTERM) and restarted", ok and flr.returncode == -signal.SIGTERM and len(started) == 1, (ok, summ, steps))
check("cat_reset: radio power-cycled", fr.on and b"PS0;" in fr.rx, fr.rx)
fr.close()

fr = FakeRadio()
ok, summ, started, flr = run_reset(False, fr, keyed=lambda: True)
check("cat_reset refused while keyed: nothing stopped or sent", not ok and "keyed" in summ and flr.returncode == -signal.SIGKILL and not fr.rx and not started, (summ, fr.rx))
fr.close()

fr = FakeRadio(dead=True)
ok, summ, started, flr = run_reset(False, fr)
check("cat_reset failure still restarts flrig", not ok and len(started) == 1 and "no CAT reply" in summ, (summ, steps))
fr.close()

fr = FakeRadio()
ok, summ, started, flr = run_reset(True, fr, with_flrig=False)
check("dry run, port free: reads PS1, sends only PS;/TX;", ok and "PS1" in summ and set(fr.rx) <= {b"PS;", b"TX;"} and fr.on, (summ, fr.rx))
fr.close()

# dry run while flrig holds the port: nothing stopped, nothing sent
fr = FakeRadio()
holder = subprocess.Popen(["sleep", "60"], stdin=open(fr.path, "rb"))
G.flrig_find = lambda: dict(fi_app, pid=holder.pid)
G.cat_settings = lambda prefs: (fr.path, 38400, 1, "test")
steps.clear()
ok, summ = G.cat_reset(True, say, rig=StubRig(), keyed=lambda: False)
check("dry run, flrig owns port: ok, flrig left running, nothing sent", ok and holder.poll() is None and not fr.rx, (summ, fr.rx))
holder.kill(); holder.wait()
# another program on the port
other = subprocess.Popen(["sleep", "60"], stdin=open(fr.path, "rb"))
G.flrig_find = lambda: None
ok, summ = G.cat_reset(False, say, rig=StubRig(), keyed=lambda: False)
check("port open by something else -> busy, nothing sent", not ok and "busy" in summ and not fr.rx, summ)
other.kill(); other.wait(); fr.close()

# ---- gate: CATRESET refused while PTT held; runs in a thread otherwise and blocks keying ----
class NullPlayer:
    def start(self): pass
    def write(self, pcm): return True
    def stop(self, drain=False): pass
class NullGain:
    def refresh(self): pass
    def apply(self, p): return p
g = G.Gate(StubRig(), NullPlayer(), NullGain(), bridge_check=lambda: False)
g.ptt_req = True
check("gate: CATRESET refused while PTT held", g.start_cat_reset(False, runner=lambda *a, **k: (True, "x")).startswith("refused"))
g.ptt_req = False
ev = threading.Event()
def slow(dry, say_, keyed): say_("step1"); ev.wait(2); return True, "done %s" % dry
check("gate: CATRESET starts", g.start_cat_reset(True, runner=slow).startswith("started"))
time.sleep(0.1)
g.flags = {"tx_enabled": True, "bridge": False, "bridge_keyed": False}
check("gate: keying refused while a CAT reset runs", g.cat_busy and g.refuse_reason() == "CAT reset in progress", g.refuse_reason())
check("gate: second CATRESET refused while running", g.start_cat_reset(False, runner=slow).startswith("refused"))
ev.set(); time.sleep(0.2)
evs = list(g.events)
check("gate: progress + done events", "catreset: step1" in evs and "catreset-done ok done True" in evs and not g.cat_busy, evs)

# ---- the gate runs the reset in its own transient unit ----
c = G.cat_reset_unit_cmd(True, now=7)
check("unit cmd: systemd-run --wait --pipe, env files, --cat-reset --dry-run",
      c[:2] == ["systemd-run", "--user"] and "--wait" in c and "--pipe" in c and "--unit=ft710-catreset-7" in c
      and any(x.endswith("radio-bridge/bridge.env") for x in c) and c[-2:] == ["--cat-reset", "--dry-run"], c)
check("unit cmd: no --dry-run for a real reset", "--dry-run" not in G.cat_reset_unit_cmd(False))
fake = os.path.join(TMP, "fakeunit.sh")
open(fake, "w").write("#!/bin/sh\necho 'step: flrig: not running'\necho 'step: PS; -> PS1;'\necho 'result=ok dry run: radio on (PS1)'\n")
os.chmod(fake, 0o755)
G.cat_reset_unit_cmd = lambda dry, now=None: [fake]
steps.clear()
ok, summ = G.cat_reset_via_unit(True, say)
check("relay: steps and result parsed", ok and summ == "dry run: radio on (PS1)" and steps == ["flrig: not running", "PS; -> PS1;"], (ok, summ, steps))
open(fake, "w").write("#!/bin/sh\necho boom\nexit 3\n")
ok, summ = G.cat_reset_via_unit(True, say)
check("relay: no result line -> failure", not ok and "without a result" in summ, summ)
check("relay: refused while keyed", G.cat_reset_via_unit(True, say, keyed=lambda: True)[0] is False)

print("\n%d failure(s)" % len(FAILS) if FAILS else "\nall CAT reset tests passed")
sys.exit(1 if FAILS else 0)
