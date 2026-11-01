// SPDX-License-Identifier: Apache-2.0
// TX without Mumble (network audio mode): the app streams the selected microphone as RTP to the
// ft710-txgate service on the rig host and drives PTT over the gate's authenticated TCP control
// link. The gate is the only thing that keys flrig or plays into the radio; it unkeys on its own
// when this link drops, heartbeats stop (~0.5 s) or audio stops (~0.2 s) while PTT is held.
//
// Included from main.cpp after cfg, shq(), sshArgs() and netaudio.h (naCfg).
#pragma once
#include <QMessageAuthenticationCode>
#include <QUdpSocket>
#include <QSet>
#include <QRandomGenerator>
#include <opus/opus.h>

// Prepare the rig host for direct TX (ssh, one round trip). Never stops radio-bridge unless flrig
// reports PTT=0 and neither keyed marker exists. Prints key=value lines.
static const char *TXG_PREPARE = R"SH(
rpc() { curl -s -m 2 -H 'Content-Type: text/xml' -d "<?xml version=\"1.0\"?><methodCall><methodName>$1</methodName></methodCall>" "$FLRIG" | tr -d '\n\t ' | sed -n 's/.*<value>\(<[a-z0-9]*>\)\{0,1\}\([^<]*\).*/\2/p'; }
set -- $SSH_CONNECTION; echo "server=$3"
ENVF="$HOME/.config/ft710-txgate/txgate.env"
TOK="$HOME/.config/ft710-txgate/token"; GPORT=10010
if [ -r "$ENVF" ]; then
  t=$(sed -n 's/^TG_TOKEN_FILE=//p' "$ENVF" | tail -1); [ -n "$t" ] && TOK=$(eval echo "$t")
  p=$(sed -n 's/^TG_PORT=//p' "$ENVF" | tail -1); [ -n "$p" ] && GPORT=$p
fi
echo "gport=$GPORT"
if ! systemctl --user cat "$UNIT" >/dev/null 2>&1; then echo result=notinstalled; exit 0; fi
if [ "$(systemctl --user is-active "$UNIT")" != active ]; then
  P=$(rpc rig.get_ptt)
  if [ "$P" != 0 ]; then echo "result=busy flrig PTT=${P:-no answer}"; exit 0; fi
  if [ -e "$HOME/.cache/radio-bridge/keyed" ] || [ -e "$HOME/.cache/ft710-txgate/keyed" ]; then echo "result=busy keyed marker present"; exit 0; fi
  if [ "$(systemctl --user is-active "$BRIDGE")" = active ]; then
    systemctl --user stop "$BRIDGE" || { echo "result=fail could not stop $BRIDGE"; exit 0; }
    echo bridge_stopped=1
  fi
  systemctl --user start "$UNIT" || { echo "result=fail could not start $UNIT"; exit 0; }
fi
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s "$TOK" ] && break; sleep 0.1; done
[ -s "$TOK" ] || { echo "result=fail no token file"; exit 0; }
echo "token=$(head -c 200 "$TOK")"
echo result=ok
)SH";

// Stop the gate (it unkeys before exiting) and optionally start radio-bridge again, only when the
// bridge is enabled, flrig reports PTT=0 and no keyed marker exists.
static const char *TXG_STOP = R"SH(
rpc() { curl -s -m 2 -H 'Content-Type: text/xml' -d "<?xml version=\"1.0\"?><methodCall><methodName>$1</methodName></methodCall>" "$FLRIG" | tr -d '\n\t ' | sed -n 's/.*<value>\(<[a-z0-9]*>\)\{0,1\}\([^<]*\).*/\2/p'; }
systemctl --user stop "$UNIT" 2>/dev/null; echo "gate=$(systemctl --user is-active "$UNIT" 2>/dev/null)"
if [ "$RESTORE" = 1 ]; then
  if [ "$(systemctl --user is-enabled "$BRIDGE" 2>/dev/null)" != enabled ]; then echo bridge=not-enabled
  elif [ "$(systemctl --user is-active "$BRIDGE")" = active ]; then echo bridge=active
  else
    P=$(rpc rig.get_ptt)
    if [ "$P" = 0 ] && [ ! -e "$HOME/.cache/radio-bridge/keyed" ] && [ ! -e "$HOME/.cache/ft710-txgate/keyed" ]; then
      systemctl --user start "$BRIDGE" && echo bridge=started || echo bridge=start-failed
    else echo "bridge=not-started (flrig PTT=${P:-no answer} or keyed marker)"; fi
  fi
fi
)SH";

class TxLink : public QObject {
public:
    enum St { Off, Preparing, Connecting, Authing, Ready, Backoff, Failed };
    std::function<void()> changed;                    // any visible state change
    std::function<void(const QString &)> logLine;
    std::function<void(const QString &)> pttLost;     // PTT was held and the link dropped / gate unkeyed

    explicit TxLink(QObject *parent = nullptr) : QObject(parent) {
        hb.setInterval(100);
        connect(&hb, &QTimer::timeout, this, [this] { if (st == Ready) sendLine("HB"); });
        retry.setSingleShot(true);
        connect(&retry, &QTimer::timeout, this, [this] { attempt(); });
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, this, [this] { if (st == Connecting || st == Authing) fail("no answer from the TX gate"); });
        connect(&sock, &QTcpSocket::connected, this, [this] { sock.setSocketOption(QAbstractSocket::LowDelayOption, 1); setSt(Authing); });
        connect(&sock, &QTcpSocket::readyRead, this, [this] { readCtl(); });
        connect(&sock, &QTcpSocket::disconnected, this, [this] { if (st != Off) fail(lastErr.isEmpty() ? "TX gate closed the connection" : lastErr); });
        connect(&sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (st == Off) return;
            fail(sock.error() == QAbstractSocket::ConnectionRefusedError ? "TX gate not listening (service stopped?)" : "TX link: " + sock.errorString());
        });
        micRestart.setSingleShot(true); micRestart.setInterval(1000);
        connect(&micRestart, &QTimer::timeout, this, [this] { if (st != Off) startMic(); });
        tone.setInterval(10);
        connect(&tone, &QTimer::timeout, this, [this] { toneTick(); });
        ssrc = QRandomGenerator::global()->generate();
    }
    ~TxLink() override { stopLocal(); if (enc) opus_encoder_destroy(enc); }

    bool active() const { return st != Off; }
    bool ready() const { return st == Ready; }
    bool pttHeld() const { return ptt; }
    bool gateKeyed() const { return gst.value("keyed") == "1"; }
    QString rigPtt() const { return st == Ready ? gst.value("ptt") : QString(); }   // flrig readout via the gate
    double micDb() const { return micLevel; }
    bool micUp() const { return toneMode || (micAlive.isValid() && micAlive.elapsed() < 1000); }
    // Why the mic is not usable ("" when it is capturing).
    QString micProblem() const {
        if (micUp()) return {};
        return micErr.isEmpty() ? QString("microphone is not capturing") : "microphone: " + micErr;
    }
    QString codec() const { return opusKbps ? QString("Opus %1 kbit/s").arg(opusKbps) : QString("L16"); }
    QString lastEvent() const { return event; }
    QString statusText() const {
        switch (st) {
        case Off: return "off";
        case Preparing: return "starting the TX gate on " + cfg.rigName() + "…";
        case Connecting: case Authing: return "connecting to the TX gate…";
        case Backoff: return err + QString(" — retrying in %1 s").arg(qMax(1, qRound(retry.remainingTime() / 1000.0)));
        case Failed: return err;
        case Ready: break;
        }
        QString s = gateKeyed() ? "KEYED" : "ready";
        s += " · " + codec();
        if (gst.value("tx_enabled") == "0") s += " · TX keying off";
        if (gst.value("bridge") == "1") s += " · radio-bridge active (gate blocked)";
        if (gst.value("safe") == "1") s += " · gate in safe mode";
        const QString l = gst.value("latched");
        if (!l.isEmpty() && l != "-") s += " · latched: " + QString(l).replace('_', ' ') + " (release PTT)";
        if (!event.isEmpty() && evAge.isValid() && evAge.elapsed() < 8000) s += " · " + event;
        return s;
    }

    // Normal start: prepare the rig host over ssh (stops radio-bridge, starts the gate), then connect.
    void start() {
        if (st != Off && st != Failed) return;
        direct = false; tries = 0; err.clear();
        configureCodec();
        startMic();
        attempt();
    }
    // Test hook: connect straight to host:port with a known token (no ssh, nothing started remotely).
    void startDirect(const QString &host, quint16 port, const QByteArray &tok, bool useTone) {
        direct = true; server = host; gport = port; token = tok; toneMode = useTone; tries = 0;
        configureCodec();
        if (toneMode) tone.start(); else startMic();
        attempt();
    }
    // Stop: release PTT, say BYE, stop capture. remote: also stop the gate (and restore the bridge if asked).
    void stop(bool remote, bool restoreBridge, bool sync) {
        setPtt(false);
        if (sock.state() == QAbstractSocket::ConnectedState) { sendLine("BYE"); sock.flush(); sock.waitForBytesWritten(200); }
        setSt(Off);
        stopLocal();
        token.fill(0); token.clear();
        if (!remote || direct || !cfg.configured()) return;
        const QString vars = QString("UNIT=%1\nBRIDGE=%2\nFLRIG=%3\nRESTORE=%4\n")
            .arg(shq(naCfg.txUnit), shq(cfg.bridgeService), shq(cfg.flrigUrl()), restoreBridge ? "1" : "0");
        auto *p = new QProcess(this);
        p->start("ssh", sshArgs("bash -s"));
        p->write((vars + TXG_STOP).toUtf8()); p->closeWriteChannel();
        if (sync) {
            p->waitForFinished(12000);
            log("stop: " + QString::fromUtf8(p->readAllStandardOutput()).simplified());
            p->deleteLater();
        } else {
            connect(p, &QProcess::finished, this, [this, p] { log("stop: " + QString::fromUtf8(p->readAllStandardOutput()).simplified()); p->deleteLater(); if (changed) changed(); });
            QTimer::singleShot(15000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
        }
    }
    // PTT: true only if the link is ready (the gate decides whether it actually keys).
    bool setPtt(bool on) {
        if (on == ptt) return true;
        if (on) {
            if (st != Ready) return false;
            if (!micUp()) { log("PTT refused: " + micProblem()); return false; }   // never key without mic audio
            micBuf.clear(); toneSent = 0;
            if (enc) opus_encoder_ctl(enc, OPUS_RESET_STATE);
            ptt = true; pttSince.start();
            sendLine("PTT 1");
        } else {
            ptt = false;
            if (sock.state() == QAbstractSocket::ConnectedState) { sendLine("PTT 0"); sock.flush(); }
        }
        if (changed) changed();
        return true;
    }

private:
    St st = Off;
    QTcpSocket sock; QUdpSocket udp; QByteArray ctlBuf;
    QString server; quint16 gport = 10010; QByteArray token; bool direct = false, toneMode = false;
    QTimer hb, retry, guard, micRestart, tone;
    int tries = 0; QString err, lastErr, event; QElapsedTimer evAge;
    QMap<QString, QString> gst;
    bool ptt = false; QElapsedTimer pttSince;
    QProcess *mic = nullptr, *prep = nullptr; QByteArray micBuf;
    double micLevel = -120; QElapsedTimer micAlive; QString micErr, micErrLogged; QByteArray micErrBuf;
    OpusEncoder *enc = nullptr; int opusKbps = 0; int frame = 480;   // samples per packet
    quint16 seq = 0; quint32 ts = 0, ssrc = 0; qint64 toneSent = 0; QElapsedTimer toneClock;

    void log(const QString &m) { if (logLine) logLine(m); }
    void setSt(St s) { if (st == s) return; st = s; if (s == Ready) hb.start(); else hb.stop(); if (changed) changed(); }
    void sendLine(const QByteArray &l) { if (sock.state() == QAbstractSocket::ConnectedState) sock.write(l + "\n"); }

    void configureCodec() {
        if (enc) { opus_encoder_destroy(enc); enc = nullptr; }
        opusKbps = naCfg.effCompression() == 1 ? naCfg.opusKbps : 0;
        frame = 480;   // 10 ms packets either way (lowest latency; L16 10 ms fits the MTU)
        if (opusKbps) {
            int e = 0;
            enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &e);
            if (!enc || e != OPUS_OK) { log("opus encoder failed; using L16"); enc = nullptr; opusKbps = 0; return; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(opusKbps * 1000));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
        }
    }
    void attempt() {
        if (st == Off && tries > 0) return;
        retry.stop();
        if (direct || (!token.isEmpty() && tries % 3 != 2)) { connectGate(); return; }
        prepare();
    }
    void prepare() {
        if (!cfg.configured()) { failHard("rig host not configured"); return; }
        setSt(Preparing);
        if (prep) { prep->disconnect(); prep->kill(); prep->deleteLater(); }
        prep = new QProcess(this);
        const QString vars = QString("UNIT=%1\nBRIDGE=%2\nFLRIG=%3\n").arg(shq(naCfg.txUnit), shq(cfg.bridgeService), shq(cfg.flrigUrl()));
        connect(prep, &QProcess::finished, this, [this](int code, QProcess::ExitStatus es) {
            const QString out = QString::fromUtf8(prep->readAllStandardOutput());
            prep->deleteLater(); prep = nullptr;
            if (st == Off) return;
            if (es != QProcess::NormalExit || code == 255) { fail(cfg.rigName() + " unreachable (ssh)"); return; }
            QMap<QString, QString> kv;
            for (const QString &l : out.split('\n')) { const int i = l.indexOf('='); if (i > 0) kv[l.left(i)] = l.mid(i + 1).trimmed(); }
            const QString r = kv.value("result");
            if (kv.contains("bridge_stopped")) log("stopped radio-bridge (TX goes through the TX gate now)");
            if (r == "notinstalled") { failHard(QString("TX gate (%1) is not installed on %2 — see README").arg(naCfg.txUnit, cfg.rigName())); return; }
            if (r.startsWith("busy")) { fail("rig busy, not switching TX path: " + r.mid(5)); return; }
            if (r != "ok") { fail(r.isEmpty() ? "TX gate start failed" : r.mid(r.indexOf(' ') + 1)); return; }
            token = kv.value("token").toLatin1(); kv["token"].fill('0');
            server = kv.value("server");
            const int gp = kv.value("gport").toInt();
            gport = quint16(naCfg.txPort);
            if (gp > 0 && gp != naCfg.txPort) log(QString("note: gate's TG_PORT is %1, settings say %2").arg(gp).arg(naCfg.txPort));
            if (server.isEmpty() || token.size() < 32) { fail("TX gate: bad answer from rig host"); return; }
            connectGate();
        });
        prep->start("ssh", sshArgs("bash -s"));
        prep->write((vars + TXG_PREPARE).toUtf8()); prep->closeWriteChannel();
        QTimer::singleShot(15000, prep, [p = prep] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void connectGate() {
        lastErr.clear(); ctlBuf.clear(); gst.clear();
        sock.abort();
        setSt(Connecting);
        guard.start(4000);
        sock.connectToHost(server, gport);
    }
    void readCtl() {
        ctlBuf += sock.readAll();
        if (ctlBuf.size() > 65536) { fail("TX gate: protocol error"); return; }
        int nl;
        while ((nl = ctlBuf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(ctlBuf.left(nl)).trimmed(); ctlBuf.remove(0, nl + 1);
            if (line.startsWith("FT710-TXGATE ")) {
                const QStringList p = line.split(' ');
                if (p.size() < 3 || p[1] != "1") { failHard("TX gate: unsupported protocol version"); return; }
                sendLine("AUTH " + QMessageAuthenticationCode::hash(p[2].toUtf8(), token, QCryptographicHash::Sha256).toHex());
            } else if (line == "OK") {
                guard.stop(); tries = 0; err.clear(); event.clear(); setSt(Ready);
                log(QString("TX gate ready at %1:%2 (%3)").arg(server).arg(gport).arg(codec()));
            } else if (line.startsWith("ERR")) {
                lastErr = line == "ERR auth" ? "TX gate refused the login (token changed?)" : "TX gate: " + line.mid(4);
                if (line == "ERR auth") { token.clear(); tries = 1; }   // next attempt re-reads the token over ssh
            } else if (line.startsWith("STATE ")) {
                const bool wasKeyed = gateKeyed();
                gst.clear();
                for (const QString &kv : line.mid(6).split(' ', Qt::SkipEmptyParts)) { const int i = kv.indexOf('='); if (i > 0) gst[kv.left(i)] = kv.mid(i + 1); }
                if (ptt && wasKeyed && !gateKeyed() && pttLost) pttLost("the TX gate unkeyed: " + (event.isEmpty() ? QString("see status") : event));
                if (changed) changed();
            } else if (line.startsWith("EVENT ")) {
                event = line.mid(6); evAge.start(); log("gate: " + event);
                if (changed) changed();
            }
        }
    }
    void fail(const QString &why) {
        if (st == Off || st == Backoff || st == Failed) return;   // already handled (error + disconnected both fire)
        st = Backoff;
        guard.stop(); hb.stop();
        const bool wasHeld = ptt;
        ptt = false;
        sock.abort();
        err = why; log(why);
        static const int backoffS[] = {1, 2, 5, 10};
        retry.start(1000 * backoffS[qMin(tries, 3)]);
        ++tries;
        if (changed) changed();
        if (wasHeld && pttLost) pttLost(why + " — the gate unkeys on its own");
    }
    void failHard(const QString &why) { if (st == Off || st == Backoff) st = Preparing; fail(why); retry.stop(); st = Failed; if (changed) changed(); }

    // --- microphone (pw-record raw s16 48 kHz mono) ---
    // No node.dont-fallback: with PipeWire 1.6 / WirePlumber 0.5 it makes a capture stream with an
    // explicit --target fail ("defined target not found") even when the source exists, so nothing
    // was ever captured. Instead the configured source must exist before we start (so the stream
    // cannot silently fall back to another mic), and node.dont-reconnect keeps it from moving later.
    static bool sourceExists(const QString &name) {
        QProcess p; p.start("pactl", {"list", "short", "sources"});
        if (!p.waitForFinished(2000)) { p.kill(); p.waitForFinished(200); return true; }   // cannot tell: let pw-record decide
        for (const QByteArray &l : p.readAllStandardOutput().split('\n')) { const auto f = l.split('\t'); if (f.size() > 1 && f[1] == name.toUtf8()) return true; }
        return false;
    }
    void micNote(const QString &e) {
        micErr = e;
        if (e != micErrLogged) { micErrLogged = e; if (!e.isEmpty()) log("mic: " + e); }
        if (changed) changed();
    }
    void startMic() {
        if (mic) return;
        if (!naCfg.micSource.isEmpty() && !sourceExists(naCfg.micSource)) {
            micNote("source not found: " + naCfg.micSource + " (plugged in? see Settings)");
            if (st != Off) micRestart.start();
            return;
        }
        mic = new QProcess(this);
        QStringList a{"--rate", "48000", "--channels", "1", "--format", "s16", "--latency", "480", "--raw",
                      "-P", "{ node.name = ft710_remote_tx_mic node.description = \"FT-710 Remote TX mic\" media.role = Communication"
                            + QString(naCfg.micSource.isEmpty() ? "" : " node.dont-reconnect = true") + " }"};
        if (!naCfg.micSource.isEmpty()) a << "--target" << naCfg.micSource;
        a << "-";
        connect(mic, &QProcess::readyReadStandardOutput, this, [this] { if (!micErr.isEmpty()) micNote({}); micData(mic->readAllStandardOutput()); });
        connect(mic, &QProcess::readyReadStandardError, this, [this] {   // pw-record errors go to the log (were dropped before)
            micErrBuf += mic->readAllStandardError();
            int nl; while ((nl = micErrBuf.indexOf('\n')) >= 0) { const QString l = QString::fromUtf8(micErrBuf.left(nl)).trimmed(); micErrBuf.remove(0, nl + 1); if (!l.isEmpty()) micNote(l); }
            if (micErrBuf.size() > 4096) micErrBuf.clear();
        });
        connect(mic, &QProcess::finished, this, [this](int code) {
            mic->deleteLater(); mic = nullptr; micLevel = -120; micErrBuf.clear();
            if (micErr.isEmpty()) micNote(QString("capture stopped (pw-record exit %1)").arg(code));
            if (ptt && pttLost) { setPtt(false); pttLost("microphone capture stopped"); }
            if (changed) changed();
            if (st != Off) micRestart.start();
        });
        connect(mic, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && mic) { mic->deleteLater(); mic = nullptr; log("pw-record not found"); }
        });
        mic->start("pw-record", a);
        if (!naCfg.micSource.isEmpty()) QTimer::singleShot(1500, mic, [this] { verifyMicLink(); });
    }
    // Make sure the capture stream is linked to the configured source and not a fallback mic.
    void verifyMicLink() {
        if (!mic) return;
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p] {
            const QStringList lines = QString::fromUtf8(p->readAllStandardOutput()).split('\n');
            p->deleteLater();
            if (!mic) return;
            QSet<QString> from; bool in = false;
            for (const QString &l : lines) {
                if (!l.startsWith(' ')) { in = l.startsWith("ft710_remote_tx_mic:"); continue; }
                const int i = l.indexOf("|<- ");
                if (in && i >= 0) from << l.mid(i + 4).section(':', 0, 0).trimmed();
            }
            if (!from.isEmpty() && !from.contains(naCfg.micSource)) {
                micNote("capturing from " + QStringList(from.values()).join(", ") + " instead of " + naCfg.micSource + " — stopped");
                mic->kill();   // finished handler retries in 1 s; PTT stays blocked meanwhile
            }
        });
        p->start("pw-link", {"-l"});
        QTimer::singleShot(3000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void stopLocal() {
        hb.stop(); retry.stop(); guard.stop(); tone.stop(); micRestart.stop();
        sock.abort();
        if (prep) { prep->disconnect(); prep->kill(); prep->waitForFinished(500); prep->deleteLater(); prep = nullptr; }
        if (mic) { mic->disconnect(); mic->kill(); mic->waitForFinished(500); mic->deleteLater(); mic = nullptr; }
        micBuf.clear(); micLevel = -120;
    }
    void micData(const QByteArray &d) {
        micAlive.start();
        // level meter (fast attack, slow release, on the raw chunk)
        const qint16 *s = reinterpret_cast<const qint16 *>(d.constData()); const int n = d.size() / 2;
        double sum = 0; for (int i = 0; i < n; ++i) sum += double(s[i]) * s[i];
        if (n) { const double db = 10 * std::log10(sum / n / (32768.0 * 32768.0) + 1e-12); micLevel = db > micLevel ? db : micLevel * 0.85 + db * 0.15; }
        if (!ptt || st != Ready) { micBuf.clear(); return; }
        micBuf += d;
        const int fb = frame * 2;
        while (micBuf.size() >= fb) { sendFrame(micBuf.constData()); micBuf.remove(0, fb); }
        if (micBuf.size() > 48000) micBuf.clear();   // never let a backlog build up
    }
    void toneTick() {   // test hook: 700 Hz at -20 dBFS, paced by the clock
        if (!toneClock.isValid()) toneClock.start();
        if (!ptt || st != Ready) { toneSent = 0; toneClock.restart(); return; }
        while (toneSent * 10 < toneClock.elapsed()) {
            qint16 buf[480];
            for (int i = 0; i < 480; ++i) buf[i] = qint16(3277 * std::sin(2 * M_PI * 700 * double(toneSent * 480 + i) / 48000));
            micLevel = -23; micAlive.start();
            sendFrame(reinterpret_cast<const char *>(buf)); ++toneSent;
        }
    }
    void sendFrame(const char *pcm) {   // one 10 ms frame, native s16le in
        QByteArray pkt(12, 0);
        uchar *h = reinterpret_cast<uchar *>(pkt.data());
        h[0] = 0x80; h[1] = opusKbps ? 97 : 96;
        h[2] = uchar(seq >> 8); h[3] = uchar(seq); ++seq;
        for (int i = 0; i < 4; ++i) { h[4 + i] = uchar(ts >> (24 - 8 * i)); h[8 + i] = uchar(ssrc >> (24 - 8 * i)); }
        ts += quint32(frame);
        const qint16 *s = reinterpret_cast<const qint16 *>(pcm);
        if (enc) {
            unsigned char out[1500];
            const int n = opus_encode(enc, s, frame, out, sizeof out);
            if (n < 0) return;
            pkt.append(reinterpret_cast<const char *>(out), n);
        } else {
            pkt.resize(12 + frame * 2);
            uchar *p = reinterpret_cast<uchar *>(pkt.data()) + 12;
            for (int i = 0; i < frame; ++i) { const quint16 v = quint16(s[i]); p[2 * i] = uchar(v >> 8); p[2 * i + 1] = uchar(v); }
        }
        udp.writeDatagram(pkt, QHostAddress(server), gport);
    }
};
