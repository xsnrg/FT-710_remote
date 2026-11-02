#!/usr/bin/env python3
"""Tests the app's rig-host scripts (TXG_PREPARE / TXG_STOP from app/txgate.h) locally with a throw-away
systemd --user unit: a gate on test ports with a fake flrig + fake pacat. The unit is removed
afterwards. Never touches a radio or the real flrig.
Run: python3 test_app_scripts.py"""
import os, re, subprocess, sys, time
import test_txgate as T

HDR = open(os.path.join(T.HERE, "..", "..", "app", "txgate.h")).read()
PREP = re.search(r'TXG_PREPARE = R"SH\((.*?)\)SH"', HDR, re.S).group(1)
STOP = re.search(r'TXG_STOP = R"SH\((.*?)\)SH"', HDR, re.S).group(1)
UD = os.path.expanduser("~/.config/systemd/user")
GU = "ft710-txgate-scripttest.service"
FLRIG = "http://127.0.0.1:%d/RPC2" % T.FLRIG_PORT


def sc(*a): return subprocess.run(["systemctl", "--user", *a], capture_output=True, text=True).stdout.strip()


def run(script, **v):
    # HOME is redirected so the scripts' marker/env/token paths point into the temp dir
    env = dict(os.environ, HOME=HOMED)
    pre = "".join("%s='%s'\n" % kv for kv in v.items())
    r = subprocess.run(["bash", "-s"], input=pre + script, capture_output=True, text=True, env=env, timeout=30)
    return dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l), r.stdout


def main():
    global HOMED
    os.makedirs(os.path.join(T.TMP, "bin"))
    with open(os.path.join(T.TMP, "bin", "pacat"), "w") as f: f.write(T.FAKE_PACAT)
    os.chmod(os.path.join(T.TMP, "bin", "pacat"), 0o755)
    T.flrig_stub()
    HOMED = os.path.join(T.TMP, "home"); os.makedirs(os.path.join(HOMED, ".config", "ft710-txgate"))
    os.makedirs(os.path.join(HOMED, ".cache", "radio-bridge")); os.makedirs(os.path.join(HOMED, ".config", "radio-bridge"))
    open(os.path.join(HOMED, ".config", "radio-bridge", "tx_enabled"), "w").close()
    tok = os.path.join(HOMED, ".config", "ft710-txgate", "token")
    open(os.path.join(HOMED, ".config", "ft710-txgate", "txgate.env"), "w").write(
        "TG_TOKEN_FILE=%s\nTG_PORT=%d\n" % (tok, T.GATE_PORT))
    env = {"TG_CONFIG_DIR": os.path.join(HOMED, ".config", "radio-bridge"), "TG_MARK": os.path.join(HOMED, ".cache", "ft710-txgate", "keyed"),
           "TG_BRIDGE_MARK": os.path.join(HOMED, ".cache", "radio-bridge", "keyed"), "TG_TOKEN_FILE": tok, "TG_FLRIG_URL": FLRIG,
           "TG_SINK": "ft710_txgate_test", "TG_BIND": "127.0.0.1", "TG_PORT": str(T.GATE_PORT), "TG_BRIDGE_UNIT": "",
           "TG_ALLOW": "127.0.0.0/8", "FAKE_PACAT_LOG": os.path.join(T.TMP, "pacat.log"),
           "PATH": os.path.join(T.TMP, "bin") + ":/usr/bin:/bin"}
    open(os.path.join(UD, GU), "w").write("[Service]\n%s\nExecStart=%s -u %s\nExecStopPost=%s %s --unkey-if-marked\n" % (
        "\n".join("Environment=%s=%s" % kv for kv in env.items()), sys.executable, T.GATE, sys.executable, T.GATE))
    try:
        sc("daemon-reload")
        V = dict(UNIT=GU, FLRIG=FLRIG)
        T.RIG["ptt"] = 1
        kv, out = run(PREP, **V)
        T.check("prepare refuses while flrig PTT=1 (gate not started)", kv.get("result", "").startswith("busy") and sc("is-active", GU) != "active", out)
        T.RIG["ptt"] = 0
        mk = os.path.join(HOMED, ".cache", "ft710-txgate", "keyed"); os.makedirs(os.path.dirname(mk), exist_ok=True); open(mk, "w").close()
        kv, out = run(PREP, **V)
        T.check("prepare refuses with the gate's keyed marker", kv.get("result", "").startswith("busy") and sc("is-active", GU) != "active", out)
        os.remove(mk)
        t = time.monotonic()
        kv, out = run(PREP, **V)
        T.check("prepare: starts gate, returns token", kv.get("result") == "ok" and sc("is-active", GU) == "active"
                and len(kv.get("token", "")) >= 32 and kv.get("gport") == str(T.GATE_PORT), re.sub(r"token=.*", "token=<hidden>", out))
        T.check("prepare: no set_ptt issued", T.calls_since(t) == [])
        kv2, out = run(PREP, **V)
        T.check("prepare again (gate active): ok, same token", kv2.get("result") == "ok" and kv2.get("token") == kv.get("token"))
        t = time.monotonic()
        kv, out = run(STOP, **V)
        T.check("stop: gate stopped, nothing else touched", sc("is-active", GU) != "active" and kv.get("gate") != "active"
                and "bridge" not in kv, out)
        T.check("stop: no set_ptt issued (gate was unkeyed)", T.calls_since(t) == [])
        kv, out = run(PREP, UNIT="ft710-txgate-not-installed.service", FLRIG=FLRIG)
        T.check("prepare: missing unit reported", kv.get("result") == "notinstalled", out)
    finally:
        sc("stop", GU)
        try: os.remove(os.path.join(UD, GU))
        except OSError: pass
        sc("daemon-reload"); sc("reset-failed", GU)
    bad = [n for n, ok in T.RESULTS if not ok]
    print("\n%d/%d passed" % (len(T.RESULTS) - len(bad), len(T.RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
