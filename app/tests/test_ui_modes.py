#!/usr/bin/env python3
"""Regression test for the RX controls (offscreen, throwaway settings dir).

Network audio + the TX gate is the only mode (Mumble was removed; see git tag last-with-mumble):
no Mumble RX volume row, the network listen slider and Listen toggle are shown and Listen is on by
default; an old Mumble-era settings file (mode=0, txGate=false, [mumble], startMumbleMinimized,
rig/bridge*) still loads, as network audio, and its old rig/bridgeConfigDir is used as the rig config dir.
Also: the network RX gain slider (default +9.5 dB = 3x, saved value shown, the stored 3.0 not
rewritten just by starting) and the band scope's click-to-tune options ("Snap to" step, SSB offset)
(default on, saved setting honoured, greyed out when click-to-tune is off).
Starts no stream and keys nothing (the app only polls the rig host read-only, if configured).
Run: python3 app/tests/test_ui_modes.py [path/to/ft710-remote]
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "build", "ft710-remote")
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok)); print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail else ""))


def run(conf, args=None):
    d = tempfile.mkdtemp(prefix="ft710ui.")
    os.makedirs(os.path.join(d, "FT-710_remote"))
    path = os.path.join(d, "FT-710_remote", "ft710-remote.conf")
    with open(path, "w") as f: f.write(conf)
    env = dict(os.environ, XDG_CONFIG_HOME=d, QT_QPA_PLATFORM="offscreen", FT710_SMOKE_MS="2500")
    out = subprocess.run([APP] + (args or []), env=env, capture_output=True, text=True, timeout=30).stdout
    with open(path) as f: saved = f.read()
    return out.splitlines(), saved


def line(lines, prefix, contains):
    return next((l for l in lines if l.startswith(prefix) and contains in l), "")


NA = "[netaudio]\nmode=1\nlistenMute=%s\nlistenVolume=80\n"
OLD = ("[General]\nstartMumbleMinimized=true\nrxVolume=40\n[mumble]\nuser=op\nchannel=FT-710\n"
       "[netaudio]\nmode=0\ntxGate=false\nrestoreBridge=true\n"
       "[rig]\nbridgeService=radio-bridge\nmurmurService=murmur\nbridgeConfigDir=~/old-rb-dir\n")


def no_mumble(lines):
    ui = [l for l in lines if l.split(":")[0] in ("label", "button", "toolbutton", "checkbox", "combo", "slider")]
    return [l for l in ui if any(w in l.lower() for w in ("mumble", "murmur", "radio-bridge", "restart bridge"))]


lines, saved = run("")   # defaults
check("defaults: no Mumble RX volume row", line(lines, "slider:", "Mumble") == "")
l = line(lines, "slider:", "listen monitor")
check("defaults: listen slider shown and enabled", l != "" and "[disabled]" not in l and "[hidden]" not in l, l)
check("defaults: Listen is on", line(lines, "toolbutton:", "Listen: on") != "", line(lines, "toolbutton:", "Listen:"))
check("defaults: no Mumble / murmur / radio-bridge anywhere in the UI", not no_mumble(lines), "; ".join(no_mumble(lines)))
btns = [l for l in lines if l.startswith("button:")]
check("defaults: CAT reset button next to Settings", any(l.startswith("button: CAT reset") for l in btns), "; ".join(btns))
check("defaults: no separate Connect / Disconnect buttons (Start/Stop does both)",
      not any(l.split(" [")[0] in ("button: Connect", "button: Disconnect") for l in btns) and any(l.startswith("button: Start") for l in btns),
      "; ".join(btns))
check("defaults: RX stream combo is ROC", "current=ROC" in line(lines, "combo:", "rig host sends"), line(lines, "combo:", "rig host sends"))

lines, saved = run(NA % "true")
check("saved Listen off is honoured", line(lines, "toolbutton:", "Listen: off") != "", line(lines, "toolbutton:", "Listen:"))

lines, saved = run(OLD)
l = line(lines, "slider:", "listen monitor")
check("old Mumble config: loads as network audio (listen row shown)", l != "" and "[hidden]" not in l, l)
check("old Mumble config: no Start Mumble minimized / Restart bridge / Mumble UI", not no_mumble(lines), "; ".join(no_mumble(lines)))
check("old Mumble config: RX stream = ROC", "current=ROC" in line(lines, "combo:", "rig host sends"), line(lines, "combo:", "rig host sends"))
check("old Mumble config: starting doesn't rewrite the old settings (only Settings > OK drops them)",
      "mode=0" in saved and "bridgeConfigDir=~/old-rb-dir" in saved and "rxGain" not in saved)
pc, _ = run(OLD, ["--print-config"])
check("old rig/bridgeConfigDir is used as the rig config dir", "configDir=~/old-rb-dir" in pc, " ".join(x for x in pc if "configDir" in x))
check("--print-config: ROC, TX gate, nothing about Mumble",
      any(x.startswith("netaudio=ROC") for x in pc) and any(x.startswith("tx=tx-gate") for x in pc) and not any("mumble" in x.lower() for x in pc))
pc, _ = run("[rig]\nconfigDir=~/new-dir\nbridgeConfigDir=~/old-rb-dir\n", ["--print-config"])
check("rig/configDir wins over the old key", "configDir=~/new-dir" in pc)

# RX gain slider (value is in 0.1 dB)
lines, saved = run(NA % "true")
l = line(lines, "slider:", "RX gain")
check("RX gain slider shown, enabled, default +9.5 dB", "value=95" in l and "[hidden]" not in l and "[disabled]" not in l, l)
check("RX gain label shows +9.5 dB", line(lines, "label:", "+9.5 dB") != "")
check("starting doesn't rewrite the RX gain", "rxGain" not in saved)
lines, _ = run(NA % "true" + "rxGain=10\n")
check("saved RX gain 10x shows +20.0 dB (slider max)", "value=200" in line(lines, "slider:", "RX gain") and line(lines, "label:", "+20.0 dB") != "",
      line(lines, "slider:", "RX gain"))
lines, _ = run(NA % "true" + "rxGain=0.5\n")
check("saved RX gain 0.5x shows -6.0 dB (slider min)", "value=-60" in line(lines, "slider:", "RX gain"), line(lines, "slider:", "RX gain"))
lines, _ = run(NA % "true" + "rxGain=99\n")
check("out-of-range RX gain is clamped to 10x", "value=200" in line(lines, "slider:", "RX gain"), line(lines, "slider:", "RX gain"))

# Click-to-tune options: "Snap to [0.5 kHz|1 kHz]" and SSB offset (scope shown by default; offscreen it can't connect, but the controls exist)
lines, saved = run("")
l = line(lines, "checkbox:", "Snap to")
check("Snap to: present and on by default", "[checked]" in l and "[disabled]" not in l, l)
l = line(lines, "combo:", "Snap step")
check("snap step: defaults to 0.5 kHz, enabled", "current=0.5 kHz" in l and "[disabled]" not in l, l)
l = line(lines, "spinbox:", "SSB signal")
check("SSB offset: defaults to 1500 Hz, enabled", "value=1500" in l and "[disabled]" not in l, l)
check("starting doesn't rewrite snap step / offset", "scopeSnapStep" not in saved and "scopeSsbOffset" not in saved)
lines, _ = run("[General]\nscopeSnapKHz=false\n")
check("Snap to: saved off is honoured", (l := line(lines, "checkbox:", "Snap to")) != "" and "[checked]" not in l, l)
check("snap step: disabled when Snap is off", "[disabled]" in line(lines, "combo:", "Snap step"), line(lines, "combo:", "Snap step"))
lines, _ = run("[General]\nscopeSnapStep=1000\nscopeSsbOffset=1200\n")
check("snap step: saved 1 kHz is honoured", "current=1 kHz" in line(lines, "combo:", "Snap step"), line(lines, "combo:", "Snap step"))
check("SSB offset: saved 1200 Hz is honoured", "value=1200" in line(lines, "spinbox:", "SSB signal"), line(lines, "spinbox:", "SSB signal"))
lines, _ = run("[General]\nscopeSnapStep=123\nscopeSsbOffset=9999\n")
check("snap step: junk falls back to 0.5 kHz", "current=0.5 kHz" in line(lines, "combo:", "Snap step"), line(lines, "combo:", "Snap step"))
check("SSB offset: out of range clamped to 3000", "value=3000" in line(lines, "spinbox:", "SSB signal"), line(lines, "spinbox:", "SSB signal"))
lines, _ = run("[General]\nscopeClickTune=false\n")
check("click-to-tune off: Snap/step/offset disabled",
      all("[disabled]" in line(lines, k, c) for k, c in (("checkbox:", "Snap to"), ("combo:", "Snap step"), ("spinbox:", "SSB signal"))))

bad = [n for n, ok in RESULTS if not ok]
print("\n%d/%d passed" % (len(RESULTS) - len(bad), len(RESULTS)) + ("; FAILED: " + ", ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
