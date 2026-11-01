#!/usr/bin/env python3
"""Regression test for the RX controls per audio mode (offscreen, throwaway settings dir).

Direct-TX mode must not show Mumble's (always disabled there) RX volume slider, must keep the
network listen slider and Listen toggle usable, and switching to direct TX must turn Listen on
(nothing else plays the radio without Mumble); switching back to Mumble TX turns it off.
Starts no stream and keys nothing (the app only polls the rig host read-only, if configured).
Run: python3 app/tests/test_ui_modes.py [path/to/ft710-remote]
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "build", "ft710-remote")
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok)); print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail else ""))


def run(conf, switch=None):
    d = tempfile.mkdtemp(prefix="ft710ui.")
    os.makedirs(os.path.join(d, "FT-710_remote"))
    path = os.path.join(d, "FT-710_remote", "ft710-remote.conf")
    with open(path, "w") as f: f.write(conf)
    env = dict(os.environ, XDG_CONFIG_HOME=d, QT_QPA_PLATFORM="offscreen", FT710_SMOKE_MS="2500")
    if switch is not None: env["FT710_SMOKE_TXGATE"] = switch
    out = subprocess.run([APP], env=env, capture_output=True, text=True, timeout=30).stdout
    with open(path) as f: saved = f.read()
    return out.splitlines(), saved


def line(lines, prefix, contains):
    return next((l for l in lines if l.startswith(prefix) and contains in l), "")


NA = "[netaudio]\nmode=1\ntxGate=%s\nlistenMute=%s\nlistenVolume=80\n"

lines, _ = run("")   # defaults: Mumble mode
check("Mumble mode: Mumble RX volume row shown", "[hidden]" not in line(lines, "slider:", "Mumble's audio") and line(lines, "slider:", "Mumble's audio") != "")
check("Mumble mode: network row hidden", "[hidden]" in line(lines, "slider:", "listen monitor"))

lines, _ = run(NA % ("false", "true"))
check("network + Mumble TX: Mumble RX volume row shown", "[hidden]" not in line(lines, "slider:", "Mumble's audio"))
check("network + Mumble TX: listen slider usable", (l := line(lines, "slider:", "listen monitor")) and "[disabled]" not in l and "[hidden]" not in l, l)

lines, _ = run(NA % ("true", "false"))
l = line(lines, "slider:", "Mumble's audio")
check("direct TX: Mumble's disabled RX volume slider not shown", "[hidden]" in l, l)
l = line(lines, "slider:", "listen monitor")
check("direct TX: listen slider shown and enabled", l != "" and "[disabled]" not in l and "[hidden]" not in l, l)
l = line(lines, "toolbutton:", "Listen:")
check("direct TX: Listen toggle shown and enabled", l != "" and "[disabled]" not in l and "[hidden]" not in l, l)

lines, saved = run(NA % ("false", "true"), switch="1")
check("switch to direct TX turns Listen on", line(lines, "toolbutton:", "Listen: on") != "" and "listenMute=false" in saved,
      line(lines, "toolbutton:", "Listen:"))
check("switch to direct TX keeps the listen volume", "listenVolume=80" in saved)
lines, saved = run(NA % ("true", "false"), switch="0")
check("switch back to Mumble TX turns Listen off", line(lines, "toolbutton:", "Listen: off") != "" and "listenMute=true" in saved,
      line(lines, "toolbutton:", "Listen:"))

bad = [n for n, ok in RESULTS if not ok]
print("\n%d/%d passed" % (len(RESULTS) - len(bad), len(RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
