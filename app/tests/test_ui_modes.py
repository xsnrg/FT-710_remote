#!/usr/bin/env python3
"""Regression test for the RX controls per audio mode (offscreen, throwaway settings dir).

Also: the network RX gain slider (shown only in network mode, default +9.5 dB = 3x, saved value
shown, the stored 3.0 not rewritten just by starting) and the band scope's "Snap to kHz" checkbox
(default on, saved setting honoured, greyed out when click-to-tune is off).

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

# RX gain slider (value is in 0.1 dB)
lines, _ = run("")
check("Mumble mode: RX gain slider hidden", "[hidden]" in line(lines, "slider:", "RX gain"), line(lines, "slider:", "RX gain"))
lines, saved = run(NA % ("false", "true"))
l = line(lines, "slider:", "RX gain")
check("network mode: RX gain slider shown, enabled, default +9.5 dB", "value=95" in l and "[hidden]" not in l and "[disabled]" not in l, l)
check("network mode: RX gain label shows +9.5 dB", line(lines, "label:", "+9.5 dB") != "")
check("starting doesn't rewrite the RX gain", "rxGain" not in saved)
lines, _ = run(NA % ("false", "true") + "rxGain=10\n")
check("saved RX gain 10x shows +20.0 dB (slider max)", "value=200" in line(lines, "slider:", "RX gain") and line(lines, "label:", "+20.0 dB") != "",
      line(lines, "slider:", "RX gain"))
lines, _ = run(NA % ("false", "true") + "rxGain=0.5\n")
check("saved RX gain 0.5x shows -6.0 dB (slider min)", "value=-60" in line(lines, "slider:", "RX gain"), line(lines, "slider:", "RX gain"))
lines, _ = run(NA % ("false", "true") + "rxGain=99\n")
check("out-of-range RX gain is clamped to 10x", "value=200" in line(lines, "slider:", "RX gain"), line(lines, "slider:", "RX gain"))

# Snap to kHz (scope shown by default; offscreen it can't connect, but the controls exist)
lines, _ = run("")
l = line(lines, "checkbox:", "Snap to kHz")
check("Snap to kHz: present and on by default", "[checked]" in l and "[disabled]" not in l, l)
lines, _ = run("[General]\nscopeSnapKHz=false\n")
check("Snap to kHz: saved off is honoured", (l := line(lines, "checkbox:", "Snap to kHz")) != "" and "[checked]" not in l, l)
lines, _ = run("[General]\nscopeClickTune=false\n")
check("Snap to kHz: disabled when click-to-tune is off", "[disabled]" in line(lines, "checkbox:", "Snap to kHz"), line(lines, "checkbox:", "Snap to kHz"))

bad = [n for n, ok in RESULTS if not ok]
print("\n%d/%d passed" % (len(RESULTS) - len(bad), len(RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
