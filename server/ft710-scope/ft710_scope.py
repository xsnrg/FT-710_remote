#!/usr/bin/env python3
"""FT-710 scope streamer (part of FT-710_remote).

Reads the FT-710's band-scope frames from its internal FT4222 USB-to-SPI bridge
(present when MENU "SCU-LAN10" is ON) and serves them to LAN clients over TCP.

READ ONLY: it opens only the FT4222 ("FT4222 A"), never writes to it, and never
touches the CAT serial ports, the audio device, flrig or radio-bridge.

Frame format (FT-710, 4096 bytes, per wfview's yaesu_scope_data / ft4222handler):
    0..849     main scope bins (stored inverted; we invert back: higher = stronger)
    850..1699  sub-receiver scope (unused on the FT-710)
    1700..2899 AF FFT / AF scope (ignored here)
    2900..3049 150-byte parameter block ("meta"):
        meta[17] scope mode, meta[32] span code (0=1k,1=2k,2=5k,3=10k,4=20k,5=50k,
        6=100k,7=200k,8=500k,9=1M), meta[52] 0=center 1=cursor 2=fixed,
        meta[60] mode, meta[64:69] VFO-A Hz as packed BCD, meta[110] S-meter,
        meta[144:148] fixed-mode scope start (BE)
    4092..4095 sync FF 01 EE 01
These offsets come from reverse engineering and are checked by `analyze`.

Wire protocol (TCP, server -> client only, little-endian), one message per frame:
    HDR = struct "<4sBBHIdQQIBBBBBxxxH"  (see HDR_FMT; HDR.size bytes)
      magic b"F7SC", version 1, hdr_len, nbins, seq, unix_time,
      vfoa_hz, start_hz, span_hz, flags, mode, span_code, scope_mode(meta[52]), smeter,
      meta_len
    then meta_len bytes of the raw (un-inverted) parameter block, then nbins bytes.
    flags: bit0 data valid, bit1 fixed-edge scope mode, bit7 = status only
    (device not available; nbins = 0), sent once a second while there is no data.
"""
import argparse, ctypes, ipaddress, os, queue, socket, struct, sys, threading, time, glob

HERE = os.path.dirname(os.path.abspath(__file__))
# Settings come from the environment (the systemd unit loads ~/.config/ft710-scope/scope.env):
#   FT710_SCOPE_LIB    path to FTDI's libft4222.so (default: lib/libft4222.so next to this script)
#   FT710_SCOPE_ALLOW  comma-separated client networks allowed to connect (default: localhost only)
#   FT710_SCOPE_BIND / FT710_SCOPE_PORT   listen address for `serve` (default 0.0.0.0:7355)
#   FT710_FLRIG_HOST / FT710_FLRIG_PORT   flrig XML-RPC, used only by `analyze` (default 127.0.0.1:12345)
LIB = os.path.expanduser(os.environ.get("FT710_SCOPE_LIB", os.path.join(HERE, "lib", "libft4222.so")))
FRAME = 4096
SYNC = b"\xff\x01\xee\x01"
TRAILER = SYNC * 4   # frames end FF01EE01 x4 (bytes 4080..4095); checking all 16 rejects 4/8/12-byte slips
WF1 = (0, 850)
META = (2900, 3050)
SPANS = [1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000]
HDR_FMT = "<4sBBHIdQQIBBBBBxxxH"
HDR = struct.Struct(HDR_FMT)
MAGIC = b"F7SC"
DEFAULT_PORT = 7355
ALLOW = [ipaddress.ip_network(n.strip(), strict=False)
         for n in os.environ.get("FT710_SCOPE_ALLOW", "127.0.0.0/8,::1/128").split(",") if n.strip()]
FLRIG_HOST = os.environ.get("FT710_FLRIG_HOST", "127.0.0.1")
FLRIG_PORT = int(os.environ.get("FT710_FLRIG_PORT", "12345"))

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

def bcd(b):
    s = b.hex()
    return int(s) if s.isdigit() else -1

def decode(frame):
    """Return dict with decoded fields from one raw 4096-byte frame."""
    meta = frame[META[0]:META[1]]
    bins = bytes(255 - x for x in frame[WF1[0]:WF1[1]])
    code = meta[32]
    span = SPANS[code] if code < len(SPANS) else 0
    vfo = bcd(meta[64:69])
    fixed = meta[52] == 2
    if fixed:
        start = int.from_bytes(meta[144:148], "big")
    else:
        start = vfo - span // 2 if vfo > 0 else 0
    return dict(meta=bytes(meta), bins=bins, span=span, span_code=code, vfo=vfo,
                start=max(start, 0), fixed=fixed, mode=meta[60], scope_mode=meta[52], smeter=meta[110])

def pack(seq, d=None, ts=None):
    ts = time.time() if ts is None else ts
    if d is None:
        return HDR.pack(MAGIC, 1, HDR.size, 0, seq, ts, 0, 0, 0, 0x80, 0, 0, 0, 0, 0)
    flags = 1 | (2 if d["fixed"] else 0)
    return HDR.pack(MAGIC, 1, HDR.size, len(d["bins"]), seq, ts, max(d["vfo"], 0), d["start"], d["span"],
                    flags, d["mode"], d["span_code"], d["scope_mode"], d["smeter"], len(d["meta"])) + d["meta"] + d["bins"]

# ---------------------------------------------------------------- FT4222
class FT4222:
    SPI_IO_SINGLE, CLK_DIV_64, CLK_IDLE_HIGH, CLK_LEADING, SYS_CLK_24 = 1, 6, 1, 0, 1

    def __init__(self, path=LIB):
        L = ctypes.CDLL(path)
        vp, ul = ctypes.c_void_p, ctypes.c_ulong
        L.FT_OpenEx.argtypes = [vp, ul, ctypes.POINTER(vp)]
        L.FT_SetTimeouts.argtypes = [vp, ul, ul]
        L.FT_SetLatencyTimer.argtypes = [vp, ctypes.c_ubyte]
        L.FT4222_SPIMaster_Init.argtypes = [vp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ubyte]
        L.FT4222_SPIMaster_SingleRead.argtypes = [vp, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_ushort,
                                                 ctypes.POINTER(ctypes.c_ushort), ctypes.c_bool]
        L.FT4222_SetClock.argtypes = [vp, ctypes.c_int]
        L.FT4222_UnInitialize.argtypes = [vp]
        L.FT_Close.argtypes = [vp]
        for f in ("FT_OpenEx", "FT_SetTimeouts", "FT_SetLatencyTimer", "FT4222_SPIMaster_Init",
                  "FT4222_SPIMaster_SingleRead", "FT4222_SetClock", "FT4222_UnInitialize", "FT_Close"):
            getattr(L, f).restype = ul
        self.L, self.h = L, None
        self.buf = (ctypes.c_ubyte * FRAME)()
        self.one = (ctypes.c_ubyte * 1)()

    def open(self):
        self.close()
        h = ctypes.c_void_p()
        st = self.L.FT_OpenEx(b"FT4222 A", 2, ctypes.byref(h))   # FT_OPEN_BY_DESCRIPTION
        if st != 0:
            raise OSError(f"FT_OpenEx failed (status {st})")
        self.h = h
        for name, st in (("FT_SetTimeouts", self.L.FT_SetTimeouts(h, 100, 100)),
                         ("FT_SetLatencyTimer", self.L.FT_SetLatencyTimer(h, 2)),
                         ("SPIMaster_Init", self.L.FT4222_SPIMaster_Init(h, self.SPI_IO_SINGLE, self.CLK_DIV_64,
                                                                         self.CLK_IDLE_HIGH, self.CLK_LEADING, 1)),
                         ("SetClock", self.L.FT4222_SetClock(h, self.SYS_CLK_24))):
            if st != 0:
                self.close()
                raise OSError(f"{name} failed (status {st})")

    def close(self):
        if self.h:
            try:
                self.L.FT4222_UnInitialize(self.h); self.L.FT_Close(self.h)
            except Exception:
                pass
        self.h = None

    def read_frame(self):
        n = ctypes.c_ushort(0)
        st = self.L.FT4222_SPIMaster_SingleRead(self.h, self.buf, FRAME, ctypes.byref(n), False)
        if st != 0:
            raise OSError(f"SPI read failed (status {st})")
        return bytes(self.buf[:n.value])

    def read_n(self, k):
        n = ctypes.c_ushort(0); b = (ctypes.c_ubyte * k)()
        st = self.L.FT4222_SPIMaster_SingleRead(self.h, b, k, ctypes.byref(n), False)
        if st != 0:
            raise OSError(f"SPI read failed (status {st})")
        return bytes(b[:n.value])

    def resync(self, frame=b"", tries=4):
        """Re-align to frame boundaries. The SPI stream is continuous across reads, so find the
        FF01EE01 x4 trailer in a full frame and discard up to its end in ONE read (single-byte
        reads do not work on the FT-710: verified on the rig 2026-11-01)."""
        want = SYNC * 4
        for _ in range(tries):
            if len(frame) != FRAME:
                frame = self.read_frame()
            if frame.endswith(TRAILER):
                return True
            buf = frame + self.read_frame()          # two frames so a trailer split at the edge is still found
            p = buf.rfind(want)
            if p >= 0:
                skip = (p + len(want)) % FRAME
                if skip:
                    self.read_n(skip)
                frame = self.read_frame()
                if frame.endswith(TRAILER):
                    return True
            frame = b""
        return False

def usb_state():
    """'absent', 'noperm' or 'ok' for VID:PID 0403:601c."""
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            v = open(d + "/idVendor").read().strip(); p = open(d + "/idProduct").read().strip()
        except OSError:
            continue
        if v == "0403" and p == "601c":
            bus = int(open(d + "/busnum").read()); dev = int(open(d + "/devnum").read())
            node = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
            return ("ok" if os.access(node, os.R_OK | os.W_OK) else "noperm"), node
    return "absent", None

def frames(stop, status_cb=None):
    """Generator of (timestamp, raw_frame). Survives unplug / re-enumeration / bad sync."""
    dev, last = None, None
    def note(s):
        nonlocal last
        if s != last:
            log(s); last = s
            if status_cb: status_cb(s)
    while not stop.is_set():
        state, node = usb_state()
        if state == "absent":
            note("FT4222 not present (radio off, USB unplugged, or SCU-LAN10 menu OFF); waiting"); stop.wait(2); continue
        if state == "noperm":
            note(f"no permission on {node}; install the udev rule (see the project README); waiting"); stop.wait(5); continue
        try:
            if dev is None:
                dev = FT4222()
            dev.open()
            note(f"FT4222 opened at {node}")
            bad = 0
            while not stop.is_set():
                f = dev.read_frame()
                if len(f) == FRAME and f.endswith(TRAILER):
                    bad = 0
                    if last != "streaming": note("streaming")
                    yield time.time(), f
                    continue
                bad += 1
                if bad > 20:
                    raise OSError("no valid frames (is SCU-LAN10 ON?)")
                if not dev.resync(f):
                    raise OSError("resync failed")
        except OSError as e:
            note(f"FT4222 error: {e}; reopening")
            if dev: dev.close()
            stop.wait(2)
    if dev: dev.close()

# ---------------------------------------------------------------- server
class Server:
    def __init__(self, bind, port):
        self.clients, self.lock = [], threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind, port)); self.sock.listen(8)
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            c, addr = self.sock.accept()
            if not any(ipaddress.ip_address(addr[0]) in n for n in ALLOW):
                log(f"refused {addr[0]} (not on LAN allowlist)"); c.close(); continue
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            q = queue.Queue(maxsize=8)
            with self.lock:
                if len(self.clients) >= 8:
                    c.close(); continue
                self.clients.append(q)
            log(f"client {addr[0]}:{addr[1]} connected")
            threading.Thread(target=self.sender, args=(c, addr, q), daemon=True).start()

    def sender(self, c, addr, q):
        try:
            c.settimeout(10)
            while True:
                c.sendall(q.get())
        except OSError:
            pass
        finally:
            with self.lock:
                self.clients.remove(q)
            c.close(); log(f"client {addr[0]}:{addr[1]} gone")

    def publish(self, msg):
        with self.lock:
            for q in self.clients:
                if q.full():
                    try: q.get_nowait()          # slow client: drop its oldest frame
                    except queue.Empty: pass
                q.put_nowait(msg)

def serve(a):
    srv = Server(a.bind, a.port)
    log(f"listening on {a.bind}:{a.port} (allow {', '.join(map(str, ALLOW))})")
    stop, seq, n, t0 = threading.Event(), 0, 0, time.time()
    online = threading.Event()
    def keepalive():   # status frames while there is no data, so clients can say why
        s = 0
        while True:
            time.sleep(1)
            if not online.is_set():
                srv.publish(pack(s)); s += 1
            online.clear()
    threading.Thread(target=keepalive, daemon=True).start()
    last = 0.0
    src = fake_frames(stop) if a.fake else frames(stop)
    for ts, f in src:
        online.set()
        if a.max_fps and ts - last < 1.0 / a.max_fps:
            continue
        last = ts
        srv.publish(pack(seq, decode(f), ts)); seq = (seq + 1) & 0xFFFFFFFF

def fake_frames(stop):
    """Synthetic frames in the FT-710 layout, for testing clients without the radio."""
    import random
    k = 0
    while not stop.is_set():
        f = bytearray(FRAME)
        for i in range(850):
            v = 30 + random.randint(0, 12) + (120 if abs(i - 425) < 3 else 0) + (80 if abs(i - (k % 850)) < 2 else 0)
            f[i] = 255 - min(v, 255)
        m = bytearray(150); m[32] = 4; m[52] = 0; m[60] = 1
        m[64:69] = bytes.fromhex("0014074000")   # 14.074000 MHz
        f[2900:3050] = m; f[-4:] = SYNC
        k += 3
        yield time.time(), bytes(f)
        stop.wait(1 / 30)

# ---------------------------------------------------------------- tools
def read_stream(host, port, seconds):
    s = socket.create_connection((host, port), timeout=5)
    out, end, buf = [], time.time() + seconds, b""
    while time.time() < end:
        chunk = s.recv(65536)
        if not chunk: break
        buf += chunk
        while len(buf) >= HDR.size:
            h = HDR.unpack_from(buf)
            total = h[2] + h[14] + h[3]
            if len(buf) < total: break
            out.append((h, buf[h[2]:h[2] + h[14]], buf[h[2] + h[14]:total])); buf = buf[total:]
    s.close()
    return out

def flrig(method, host=FLRIG_HOST, port=FLRIG_PORT):
    import xmlrpc.client
    return xmlrpc.client.ServerProxy(f"http://{host}:{port}/RPC2").__getattr__(method)()

def analyze(a):
    msgs = read_stream(a.host, a.port, a.seconds)
    data = [m for m in msgs if m[0][9] & 1]
    print(f"messages: {len(msgs)}  data frames: {len(data)}  status-only: {len(msgs) - len(data)}")
    if not data:
        return 1
    ts = [m[0][5] for m in data]
    fps = (len(ts) - 1) / (ts[-1] - ts[0]) if len(ts) > 1 else 0
    h, meta, bins = data[-1]
    print(f"frame rate: {fps:.1f} fps   bins: {h[3]}   bin min/max/median: {min(bins)}/{max(bins)}/{sorted(bins)[len(bins)//2]}")
    print(f"decoded: VFO-A {h[6]} Hz, span code {h[11]} -> {h[8]} Hz, scope mode byte {h[12]} ({'fixed' if h[9] & 2 else 'center/cursor'}), "
          f"start {h[7]} Hz, mode byte {h[10]}, S-meter byte {h[13]}")
    print("meta[0:150] hex:", meta.hex())
    try:
        v = flrig("rig.get_vfo")
        print(f"flrig rig.get_vfo: {v}  ->  {'MATCH' if int(float(v)) == h[6] else 'MISMATCH'}")
    except Exception as e:
        print("flrig read failed:", e)
    if a.png:
        from PIL import Image
        rows = [m[2] for m in data][-a.rows:]
        w = len(rows[-1])
        img = Image.new("RGB", (w, len(rows)))
        lo = min(min(r) for r in rows); hi = max(max(r) for r in rows); rng = max(hi - lo, 1)
        def col(x):   # dark blue -> cyan -> yellow -> red
            t = (x - lo) / rng
            pts = [(0, (0, 0, 40)), (0.35, (0, 120, 200)), (0.6, (230, 230, 0)), (1, (255, 30, 0))]
            for (t0, c0), (t1, c1) in zip(pts, pts[1:]):
                if t <= t1:
                    u = (t - t0) / (t1 - t0) if t1 > t0 else 0
                    return tuple(int(c0[j] + (c1[j] - c0[j]) * max(0, min(1, u))) for j in range(3))
            return pts[-1][1]
        lut = [col(x) for x in range(256)]
        px = img.load()
        for y, r in enumerate(reversed(rows)):
            for x, b in enumerate(r):
                px[x, y] = lut[b]
        img = img.resize((w, max(len(rows), 1) * 2))
        img.save(a.png); print("waterfall PNG:", a.png)
    if a.raw:
        with open(a.raw, "wb") as f:
            for m in data: f.write(HDR.pack(*m[0]) + m[1] + m[2])
        print("raw messages:", a.raw)
    return 0

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("serve"); s.add_argument("--bind", default=os.environ.get("FT710_SCOPE_BIND", "0.0.0.0"))
    s.add_argument("--port", type=int, default=int(os.environ.get("FT710_SCOPE_PORT", DEFAULT_PORT)))
    s.add_argument("--max-fps", type=float, default=0); s.add_argument("--fake", action="store_true", help="synthetic frames (testing)")
    s = sp.add_parser("analyze", help="connect to the running streamer, report fps/bins/metadata vs flrig, save PNG")
    s.add_argument("--host", default="127.0.0.1"); s.add_argument("--port", type=int, default=DEFAULT_PORT)
    s.add_argument("--seconds", type=float, default=5); s.add_argument("--png"); s.add_argument("--rows", type=int, default=300)
    s.add_argument("--raw")
    a = p.parse_args()
    if a.cmd == "serve":
        serve(a)
    else:
        sys.exit(analyze(a))

if __name__ == "__main__":
    main()
