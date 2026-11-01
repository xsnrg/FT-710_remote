#!/usr/bin/env python3
"""End-to-end test: the real FT-710 Remote app (hidden --txgate-test hook, 700 Hz -20 dBFS tone)
against a local ft710_txgate.py with a fake flrig and a fake pacat (see test_txgate.py).
Never touches a real radio or sound device.
Run: python3 test_app_txgate.py /path/to/ft710-remote
"""
import os, signal, subprocess, sys, time
import test_txgate as T

APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(T.HERE, "..", "..", "build", "ft710-remote")


def app(g, hold_ms, *extra):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    return subprocess.Popen([APP, "--txgate-test", "127.0.0.1", str(T.GATE_PORT), g.env["TG_TOKEN_FILE"], str(hold_ms), *extra],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def wait_key(t0, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if any(v == 1 for _, v in T.calls_since(t0)): return True
        time.sleep(0.01)
    return False


def peak(g):
    return max((d[2] for d in T.pacat_audio(g)[1]), default=0)


def main():
    os.makedirs(os.path.join(T.TMP, "bin"))
    with open(os.path.join(T.TMP, "bin", "pacat"), "w") as f: f.write(T.FAKE_PACAT)
    os.chmod(os.path.join(T.TMP, "bin", "pacat"), 0o755)
    T.flrig_stub()
    check = T.check

    for name, extra, lo, hi in (("L16", (), 2800, 3800), ("Opus 32", ("--opus", "32"), 2300, 4300)):
        g = T.Gate("app_" + name.replace(" ", "")).start(); time.sleep(0.2)
        t = time.monotonic(); p = app(g, 1500, *extra); out = p.communicate(timeout=30)[0]
        cs = T.calls_since(t)
        ok = [v for _, v in cs] == [1, 0]
        held = cs[1][0] - cs[0][0] if ok else -1
        check("app %s: one key + one unkey" % name, ok and p.returncode == 0, str(cs))
        check("app %s: key time ~ hold 1.5 s + hang 0.7 s" % name, 2.05 <= held <= 2.5, "%.3f s" % held)
        check("app %s: tone reached the sink at the expected level" % name, lo <= peak(g) <= hi, "peak %d" % peak(g))
        check("app %s: gate reported KEYED to the app" % name, "KEYED" in out, out[-400:] if "KEYED" not in out else "")
        g.stop()

    # app frozen (SIGSTOP) while holding PTT -> gate unkeys by itself (audio gap / heartbeat)
    g = T.Gate("app_stop").start(); time.sleep(0.2)
    t = time.monotonic(); p = app(g, 20000)
    keyed = wait_key(t); time.sleep(0.3); ts = time.monotonic(); p.send_signal(signal.SIGSTOP); time.sleep(1.0)
    cs = T.calls_since(t)
    d = cs[-1][0] - (ts - t) if len(cs) == 2 else -1
    check("app frozen while keyed: gate unkeys within 0.6 s", keyed and [v for _, v in cs] == [1, 0] and d <= 0.6, "%s %.3f s" % (cs, d))
    p.send_signal(signal.SIGCONT); time.sleep(0.6)
    check("app frozen: no re-key after it resumes (latched until release)", [v for _, v in T.calls_since(t)] == [1, 0])
    p.kill(); p.wait(); g.stop()

    # app killed while holding PTT -> immediate unkey
    g = T.Gate("app_kill").start(); time.sleep(0.2)
    t = time.monotonic(); p = app(g, 20000)
    keyed = wait_key(t); time.sleep(0.3); tk = time.monotonic(); p.kill(); p.wait(); time.sleep(0.4)
    cs = T.calls_since(t)
    d = cs[-1][0] - (tk - t) if len(cs) == 2 else -1
    check("app killed while keyed: gate unkeys within 0.2 s", keyed and [v for _, v in cs] == [1, 0] and d <= 0.2, "%s %.3f s" % (cs, d))
    check("app killed: no marker left", not os.path.exists(g.mark))
    g.stop()

    # tx_enabled off: app asks, gate refuses, nothing keyed
    g = T.Gate("app_txoff"); os.remove(os.path.join(g.cfg, "tx_enabled")); g.start(); time.sleep(0.2)
    t = time.monotonic(); p = app(g, 800); out = p.communicate(timeout=30)[0]
    check("app with tx_enabled off: never keyed, refusal shown", T.calls_since(t) == [] and "tx_enabled" in out, out[-300:])
    g.stop()

    # gate stopped while app holds PTT -> unkey, app drops PTT; gate back -> app reconnects, does not re-key
    g = T.Gate("app_gatestop").start(); time.sleep(0.2)
    t = time.monotonic(); p = app(g, 20000, "--wait-ms", "6000")
    keyed = wait_key(t); time.sleep(0.3); g.stop(); time.sleep(0.3)
    cs = T.calls_since(t)
    check("gate stopped while keyed: unkeyed", keyed and [v for _, v in cs] == [1, 0], str(cs))
    g.start(); time.sleep(4.0)
    p.terminate(); out = p.communicate(timeout=10)[0]
    check("gate stopped: app dropped PTT", "pttLost" in out or "PTT dropped" in out, out[-500:])
    check("gate back: app reconnected without re-keying", out.count("status ready") >= 2 and [v for _, v in T.calls_since(t)] == [1, 0], out[-600:])
    g.stop()

    # the real GUI PTT path (pressPtt/beginTalk/stopTalk, focus loss) with a throwaway settings dir
    g = T.Gate("app_gui").start(); time.sleep(0.2)
    xdg = os.path.join(T.TMP, "xdg"); os.makedirs(xdg, exist_ok=True)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=xdg)
    t = time.monotonic()
    p = subprocess.Popen([APP, "--txgate-gui-test", "127.0.0.1", str(T.GATE_PORT), g.env["TG_TOKEN_FILE"], "1500"],
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = p.communicate(timeout=40)[0]
    cs = T.calls_since(t); vs = [v for _, v in cs]
    lines = {l.split("|")[0].replace("gui-test:", "").strip(): l for l in out.splitlines() if l.startswith("gui-test:")}
    check("GUI: two overs -> key, unkey, key, unkey", vs == [1, 0, 1, 0] and p.returncode == 0, "%s rc=%s" % (cs, p.returncode))
    check("GUI: first over held ~1.5 s + 0.7 s hang", len(cs) == 4 and 2.0 <= cs[1][0] - cs[0][0] <= 2.6, str(cs))
    check("GUI: button shows TALKING while keyed", "TALKING" in lines.get("holding", "") and "TALKING?" not in lines.get("holding", ""), lines.get("holding", out[-600:]))
    check("GUI: button released after release", "PTT" in lines.get("released", "") and "talking=0" in lines.get("released", ""), lines.get("released", ""))
    check("GUI: focus loss releases PTT (unkey after hang)", len(cs) == 4 and cs[3][0] - cs[2][0] <= 1.5 and "talking=0" in lines.get("focus lost", ""), "%s %s" % (cs, lines.get("focus lost", "")))
    check("GUI: no marker left", not os.path.exists(g.mark))
    print(out[-1500:])
    g.stop()

    bad = [n for n, ok in T.RESULTS if not ok]
    print("\n%d/%d passed" % (len(T.RESULTS) - len(bad), len(T.RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
