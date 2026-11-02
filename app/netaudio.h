// Network audio ("LAN stream"): the FT-710's receive audio sent straight from the rig host to this
// computer with PipeWire. Receive-only: nothing here ever plays audio into the radio or keys it
// (TX goes through the TX gate, txgate.h).
//
//   rig host:  ft710-netaudio-send  (pipewire roc-sink / rtp-sink, or GStreamer opusenc for Opus)
//              reads the radio's USB capture node (shared, read-only) and sends it to this computer.
//   here:      ft710-netaudio-recv  (pipewire roc-source / rtp-source) receives it as "FT-710 RX
//              (network, before RX gain)" (ft710_rx_net_raw); a filter-chain with the bundled
//              ft710_rxlimit LADSPA plugin (RX gain + lookahead limiter at -1 dBFS, so no gain clips)
//              turns that into the capture device "FT-710 RX (network)" (node ft710_rx_net) for
//              WSJT-X, fldigi, the level meter and the listen monitor. The gain changes live (pw-cli).
//
// Both run as systemd --user units (transient via systemd-run, or the optional installed unit files
// from server/netaudio and app/netaudio). Their configs are generated from the templates in those
// directories (compiled in as Qt resources). A health monitor watches the received level, both
// units and the radio's USB device and restarts things with a 1/2/5/10 s backoff.
//
// Included by main.cpp after Config / shq() / sshArgs(); not a standalone header.

struct NetAudioCfg {
    int mode = 1;                 // 1 = ROC, 2 = RTP (an old 0 = Mumble setting loads as ROC)
    int compression = 0;          // 0 = none (L16), 1 = Opus (RTP only; ROC 0.4 is PCM-only)
    int opusKbps = 32;            // 24 / 32 / 48 / 64
    int port = 10001;             // RTP: this port; ROC: port, port+1 (repair), port+2 (control)
    int latencyMs = 120;          // receiver buffer
    int packetMs = 10;            // RTP packet (ptime); L16 max 10 ms (fits a 1280-byte MTU)
    QString fec = "rs8m";         // ROC FEC: rs8m, ldpc, disable
    QString sourceNode;           // rig host capture node; "" = RB_SOURCE from bridge.env in the rig config dir
    QString destAddr;             // address the rig host sends to; "" = this computer as seen by ssh
    bool keepOnExit = false;      // leave the stream running when the app quits
    int listenVol = 100;          // local monitor (speakers) of the network stream
    double rxGain = 3.0;          // RX gain on this computer, linear (3x = +9.5 dB)
    static constexpr double RX_GAIN_MIN = 0.5, RX_GAIN_MAX = 10.0, RX_CEILING_DB = -1.0;
    static double gainDb(double g) { return 20.0 * std::log10(g); }
    static double clampGain(double g) { return std::isfinite(g) ? qBound(RX_GAIN_MIN, g, RX_GAIN_MAX) : 3.0; }
    // "3", "3.0", "9.5dB" / "+9.5 dB" -> linear gain (NaN if unparsable)
    static double parseGain(QString t) {
        t = t.trimmed().toLower(); bool db = t.endsWith("db"); if (db) t.chop(2);
        bool ok = false; const double v = t.trimmed().toDouble(&ok);
        return !ok ? std::nan("") : db ? std::pow(10.0, v / 20.0) : v;
    }
    bool listenMute = false;      // the listen monitor is the only local playback of the radio
    // TX: mic -> RTP -> ft710-txgate on the rig host, PTT over its TCP control link
    QString micSource;            // PipeWire capture node for the mic; "" = default input
    int txPort = 10010;           // TX gate TCP control + UDP audio port on the rig host
    QString txUnit = "ft710-txgate";   // systemd --user unit of the gate on the rig host

    bool enabled() const { return mode == 1 || mode == 2; }
    int effCompression() const { return mode == 2 ? compression : 0; }
    QString modeName() const { return mode == 2 ? "RTP" : "ROC"; }
    QString describe() const {
        QString s = modeName() + " · " + (effCompression() ? QString("Opus %1 kbit/s").arg(opusKbps) : QString("L16"));
        if (mode == 1) s += " · FEC " + fec;
        if (mode == 2) s += QString(" · %1 ms packets").arg(packetMs);
        return s + QString(" · %1 ms buffer · RX gain %2%3 dB").arg(latencyMs).arg(gainDb(rxGain) >= 0 ? "+" : "").arg(gainDb(rxGain), 0, 'f', 1);
    }
    void load() {
        QSettings st; NetAudioCfg d;
        mode = st.value("netaudio/mode", d.mode).toInt() == 2 ? 2 : 1;   // 0 (the removed Mumble mode) -> ROC
        compression = qBound(0, st.value("netaudio/compression", d.compression).toInt(), 1);
        opusKbps = st.value("netaudio/opusKbps", d.opusKbps).toInt();
        port = st.value("netaudio/port", d.port).toInt();
        latencyMs = st.value("netaudio/latencyMs", d.latencyMs).toInt();
        packetMs = st.value("netaudio/packetMs", d.packetMs).toInt();
        fec = st.value("netaudio/fec", d.fec).toString().trimmed();
        sourceNode = st.value("netaudio/sourceNode").toString().trimmed();
        destAddr = st.value("netaudio/destAddr").toString().trimmed();
        keepOnExit = st.value("netaudio/keepOnExit", d.keepOnExit).toBool();
        listenVol = qBound(0, st.value("netaudio/listenVolume", d.listenVol).toInt(), 150);
        listenMute = st.value("netaudio/listenMute", d.listenMute).toBool();
        rxGain = clampGain(st.value("netaudio/rxGain", d.rxGain).toDouble());
        micSource = st.value("netaudio/micSource").toString().trimmed();
        txPort = st.value("netaudio/txPort", d.txPort).toInt();
        txUnit = st.value("netaudio/txUnit", d.txUnit).toString().trimmed();
    }
    void save() const {
        QSettings st;
        st.setValue("netaudio/mode", mode); st.setValue("netaudio/compression", compression);
        st.setValue("netaudio/opusKbps", opusKbps); st.setValue("netaudio/port", port);
        st.setValue("netaudio/latencyMs", latencyMs); st.setValue("netaudio/packetMs", packetMs);
        st.setValue("netaudio/fec", fec); st.setValue("netaudio/sourceNode", sourceNode);
        st.setValue("netaudio/destAddr", destAddr); st.setValue("netaudio/keepOnExit", keepOnExit);
        st.setValue("netaudio/listenVolume", listenVol); st.setValue("netaudio/listenMute", listenMute);
        st.setValue("netaudio/rxGain", rxGain);
        st.setValue("netaudio/micSource", micSource); st.setValue("netaudio/txPort", txPort);
        st.setValue("netaudio/txUnit", txUnit);
    }
    static bool nodeOk(const QString &n) { static const QRegularExpression re("^[A-Za-z0-9_][A-Za-z0-9._:@+-]{0,200}$"); return re.match(n).hasMatch(); }
    static bool ipOk(const QString &a) { static const QRegularExpression re("^([0-9]{1,3}(\\.[0-9]{1,3}){3}|[0-9A-Fa-f:]*:[0-9A-Fa-f:.]+)$"); return re.match(a).hasMatch(); }
    QString validate() const {
        if (!enabled()) return {};
        if (port < 1024 || port > 65533) return "Network audio port must be 1024-65533.";
        if (latencyMs < 20 || latencyMs > 2000) return "Network audio buffer must be 20-2000 ms.";
        if (!(rxGain >= RX_GAIN_MIN && rxGain <= RX_GAIN_MAX)) return "RX gain must be 0.5x-10x (-6 to +20 dB).";
        if (!QList<int>{24, 32, 48, 64}.contains(opusKbps)) return "Opus bitrate must be 24, 32, 48 or 64 kbit/s.";
        if (!QStringList{"rs8m", "ldpc", "disable"}.contains(fec)) return "ROC FEC must be rs8m, ldpc or disable.";
        if (!QList<int>{5, 10, 20, 40}.contains(packetMs)) return "Packet size must be 5, 10, 20 or 40 ms.";
        if (mode == 2 && effCompression() == 0 && packetMs > 10) return "Uncompressed (L16) RTP packets can be at most 10 ms (MTU).";
        if (!sourceNode.isEmpty() && !nodeOk(sourceNode)) return "Source node: use the PipeWire node name (pactl list short sources).";
        if (!destAddr.isEmpty() && !ipOk(destAddr)) return "Send-to address must be an IPv4/IPv6 address (blank = automatic).";
        if (txPort < 1024 || txPort > 65535) return "TX gate port must be 1024-65535.";
        if (txPort >= port && txPort <= port + 2) return "TX gate port must differ from the receive ports.";
        if (!micSource.isEmpty() && !nodeOk(micSource)) return "Microphone: use the PipeWire node name (pactl list short sources).";
        static const QRegularExpression unit("^[A-Za-z0-9_][A-Za-z0-9_.@-]{0,100}$");
        if (!unit.match(txUnit).hasMatch()) return "TX gate service: invalid unit name.";
        return {};
    }
};
static NetAudioCfg naCfg;

class NetAudio : public QObject {
public:
    static constexpr const char *RECV_UNIT = "ft710-netaudio-recv", *SEND_UNIT = "ft710-netaudio-send",
                                *NODE = "ft710_rx_net", *GAIN_NODE = "ft710_rx_net_gain", *RAW_NODE = "ft710_rx_net_raw",
                                *REMOTE_DIR = "$HOME/.config/ft710-netaudio";
    std::function<void()> changed;            // UI refresh hook
    std::function<void(const QString &)> logLine;

    explicit NetAudio(QObject *parent = nullptr) : QObject(parent) {
        health.setInterval(500);
        connect(&health, &QTimer::timeout, this, [this] { tick(); });
        remotePoll.setInterval(3000);
        connect(&remotePoll, &QTimer::timeout, this, [this] { pollRemote(); });
        recover.setSingleShot(true);
        connect(&recover, &QTimer::timeout, this, [this] { doRecover(); });
        silentSince.start(); graceSince.start();
    }
    ~NetAudio() override { killProc(meter); killProc(listen); }

    bool active() const { return wanted; }
    bool linkUp() const { return up; }
    double levelDb() const { return level; }
    int restartCount() const { return restarts; }
    QString source() const { return srcNode; }
    QString destination() const { return destIp; }
    QString localState() const { return localUnit; }
    QString remoteState() const { return remoteUnit; }

    QString statusText() const {
        if (!wanted) return "stopped — " + naCfg.describe();
        if (!error.isEmpty()) return error;
        QString s;
        if (srcPresent == 0) s = QString("FT-710 USB audio (%1) missing on %2 — waiting for it").arg(srcNode, cfg.rigName());
        else if (up) s = QString("up %1 · %2").arg(fmtDur(upSince.elapsed() / 1000), naCfg.describe());
        else if (recover.isActive()) s = QString("link down (%1) — restart in %2 s").arg(downWhy).arg((recover.remainingTime() + 999) / 1000);
        else if (busy) s = "starting…";
        else s = "connecting… " + naCfg.describe();
        if (restarts) s += QString(" · %1 restart%2").arg(restarts).arg(restarts == 1 ? "" : "s");
        return s;
    }

    // Start (or restart) both ends. adopt: units already running (app restarted) -> just monitor.
    void start(bool adopt = false) {
        wanted = true; error.clear(); up = false; backoff = 0; failsInRow = 0;
        resolve([this, adopt](bool ok) {
            if (!ok || !wanted) { emitChanged(); return; }
            health.start(); remotePoll.start();
            if (adopt && !runningHasChain()) {   // receiver from an older version (no RX gain): rebuild it, ~1 s dropout
                say("running receiver has no RX gain stage — restarting it");
                busy = true;
                restartLocal([this](bool) { busy = false; graceSince.restart(); silentSince.restart(); pollRemote(); emitChanged(); });
                health.start(); remotePoll.start(); emitChanged(); return;
            }
            if (adopt) { chainRunning = true; graceSince.restart(); startMeter(); syncListen(); applyRxGain(); pollRemote(); emitChanged(); return; }
            restartBoth();
        });
        emitChanged();
    }
    // Stop both units. sync: block until done (Disconnect / quit).
    void stop(bool sync) {
        const bool was = wanted || localUnit == "active";
        wanted = false; up = false; health.stop(); remotePoll.stop(); recover.stop();
        killProc(meter); killProc(listen);
        if (!was && !sync) { emitChanged(); return; }
        const QString loc = stopScript(RECV_UNIT), rem = stopScript(SEND_UNIT);
        if (sync) {
            QProcess a; a.start("bash", {"-c", loc});
            QProcess b; if (cfg.configured()) { b.start("ssh", sshArgs("bash -s")); b.write(rem.toUtf8()); b.closeWriteChannel(); }
            a.waitForFinished(5000); if (cfg.configured()) b.waitForFinished(8000);
        } else {
            run("bash", {"-c", loc}, {}, 5000, nullptr);
            if (cfg.configured()) run("ssh", sshArgs("bash -s"), rem.toUtf8(), 8000, nullptr);
        }
        localUnit = "inactive"; remoteUnit = "inactive"; level = -120;
        say("stopped");
        emitChanged();
    }
    // Is the local receiver unit running right now? (used at startup to adopt a running stream)
    static bool localRunning() {
        QProcess p; p.start("systemctl", {"--user", "is-active", RECV_UNIT});
        p.waitForFinished(3000); return p.readAllStandardOutput().trimmed() == "active";
    }
    // RX gain: saved, and applied live to the running receiver's filter-chain (no restart, no dropout;
    // the plugin smooths the change over 20 ms). save=false: CLI test override.
    void setRxGain(double g, bool save = true) {
        naCfg.rxGain = NetAudioCfg::clampGain(g);
        if (save) QSettings().setValue("netaudio/rxGain", naCfg.rxGain);
        if (wanted) applyRxGain();
    }
    double appliedRxGain() const { return gainApplied; }
    static QString receiverConfText() { return receiverConf(limiterPlugin()); }   // --print-receiver-conf
    void setListen(int vol, bool mute) {
        naCfg.listenVol = qBound(0, vol, 150); naCfg.listenMute = mute;
        QSettings().setValue("netaudio/listenVolume", naCfg.listenVol); QSettings().setValue("netaudio/listenMute", mute);
        listenApplied = -1; syncListen();
    }

    // ---- config generation (templates compiled in from server/netaudio and app/netaudio) ----
    static QString tmpl(const QString &name, const QMap<QString, QString> &vars) {
        QFile f(":/netaudio/" + name);
        QString t = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
        if (vars.contains("RXGAIN_CHAIN")) t.replace("@RXGAIN_CHAIN@", vars["RXGAIN_CHAIN"]);   // inserted text is already filled in
        for (auto it = vars.begin(); it != vars.end(); ++it) t.replace("@" + it.key() + "@", it.value());
        return t;
    }
    // The RX gain/limiter LADSPA plugin is compiled into the app (Qt resource) and written out next to
    // the receiver config, so a copied binary is all that's needed. Rewritten only when it changed
    // (write + rename: a running receiver keeps its mapped copy). "" if it can't be written.
    static QString limiterPlugin() {
        QFile res(":/netaudio/ft710_rxlimit.so");
        if (!res.open(QIODevice::ReadOnly)) return {};
        const QByteArray so = res.readAll();
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("FT-710_remote");
        const QString path = dir + "/ft710_rxlimit.so";
        QFile cur(path);
        if (cur.open(QIODevice::ReadOnly) && cur.readAll() == so) return path;
        cur.close();
        QDir().mkpath(dir);
        QFile tmp(path + ".tmp");
        if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate) || tmp.write(so) != so.size()) return {};
        tmp.close();
        QFile::remove(path);
        return QFile::rename(path + ".tmp", path) ? path : QString();
    }
    static void put(QMap<QString, QString> &m, std::initializer_list<std::pair<QString, QString>> kv) { for (const auto &e : kv) m[e.first] = e.second; }
    static QString receiverConf(const QString &plugin) {
        const NetAudioCfg &c = naCfg;
        const bool chain = !plugin.isEmpty();
        QMap<QString, QString> v;
        v["RAW_NODE"] = chain ? RAW_NODE : NODE;
        v["RAW_DESC"] = chain ? "FT-710 RX (network, before RX gain)" : "FT-710 RX (network)";
        v["RXGAIN_CHAIN"] = chain ? tmpl("receiver-rxgain.inc.in", {{"NODE", NODE}, {"RAW_NODE", RAW_NODE}, {"DESC", "FT-710 RX (network)"},
                                         {"PLUGIN", plugin}, {"GAIN", QString::number(NetAudioCfg::clampGain(c.rxGain), 'f', 4)},
                                         {"CEILING", QString::number(NetAudioCfg::RX_CEILING_DB, 'f', 1)}})
                                  : QString("    # RX gain unavailable (couldn't write the limiter plugin): unity gain\n");
        if (c.mode == 1) {
            put(v, {{"PORT_SOURCE", QString::number(c.port)}, {"PORT_REPAIR", QString::number(c.port + 1)},
                      {"PORT_CONTROL", QString::number(c.port + 2)}, {"FEC", c.fec}, {"LATENCY", QString::number(c.latencyMs)}});
            return tmpl("receiver-roc.conf.in", v);
        }
        const bool opus = c.effCompression() == 1;
        put(v, {{"PORT", QString::number(c.port)}, {"LATENCY", QString::number(c.latencyMs)},
                  {"MEDIA", opus ? "opus" : "audio"}, {"FORMAT_LINE", opus ? "" : "audio.format = \"S16BE\""}});
        return tmpl("receiver-rtp.conf.in", v);
    }
    // Returns the file name (sender.conf / sender.sh) and its contents.
    static QPair<QString, QString> senderFile(const QString &src, const QString &dest) {
        const NetAudioCfg &c = naCfg;
        if (c.mode == 1)
            return {"sender.conf", tmpl("sender-roc.conf.in", {{"DEST_IP", dest}, {"PORT_SOURCE", QString::number(c.port)},
                    {"PORT_REPAIR", QString::number(c.port + 1)}, {"PORT_CONTROL", QString::number(c.port + 2)}, {"FEC", c.fec}, {"SOURCE", src}})};
        if (c.effCompression() == 1)
            return {"sender.sh", tmpl("sender-rtp-opus.sh.in", {{"DEST_IP", dest}, {"PORT", QString::number(c.port)},
                    {"BITRATE", QString::number(c.opusKbps * 1000)}, {"PTIME", QString::number(c.packetMs)}, {"SOURCE", src}})};
        return {"sender.conf", tmpl("sender-rtp.conf.in", {{"DEST_IP", dest}, {"PORT", QString::number(c.port)},
                {"PTIME", QString::number(c.packetMs)}, {"SOURCE", src}})};
    }
    static QString localDir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("FT-710_remote/netaudio"); }

private:
    bool wanted = false, up = false, busy = false;
    QString srcNode, destIp, error, downWhy;
    QString localUnit = "unknown", remoteUnit = "unknown";
    int srcPresent = -1, backoff = 0, restarts = 0, failsInRow = 0, listenApplied = -1;
    double gainApplied = -1;      // gain last confirmed on the running receiver (-1 unknown)
    bool gainBusy = false, gainAgain = false, chainRunning = false;
    double level = -120;
    QTimer health, remotePoll, recover;
    QElapsedTimer silentSince, graceSince, upSince, lastData, lastLocalPoll;
    QProcess *meter = nullptr, *listen = nullptr;
    QByteArray meterBuf;
    bool remoteBusy = false, localBusy = false;
    static constexpr int BACKOFF_S[] = {1, 2, 5, 10};
    static constexpr int SILENCE_MS = 2000, GRACE_MS = 5000;

    static QString fmtDur(qint64 s) { return s < 60 ? QString("%1 s").arg(s) : s < 3600 ? QString("%1 min").arg(s / 60) : QString("%1 h %2 min").arg(s / 3600).arg(s / 60 % 60); }
    void emitChanged() { if (changed) changed(); }
    void say(const QString &m) { if (logLine) logLine(m); }
    static void killProc(QProcess *&p) { if (!p) return; p->disconnect(); p->kill(); p->waitForFinished(1000); p->deleteLater(); p = nullptr; }

    void run(const QString &prog, const QStringList &args, const QByteArray &in, int ms, std::function<void(bool, QByteArray)> cb) {
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [p, cb](int code, QProcess::ExitStatus st) {
            const QByteArray out = p->readAllStandardOutput(); p->deleteLater();
            if (cb) cb(st == QProcess::NormalExit && code == 0, out);
        });
        connect(p, &QProcess::errorOccurred, this, [p, cb](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) { p->deleteLater(); if (cb) cb(false, {}); }
        });
        p->start(prog, args);
        if (!in.isEmpty()) p->write(in);
        p->closeWriteChannel();
        QTimer::singleShot(ms, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    static QMap<QString, QString> kv(const QByteArray &out) {
        QMap<QString, QString> m;
        for (const QString &l : QString::fromUtf8(out).split('\n')) { int e = l.indexOf('='); if (e > 0) m[l.left(e)] = l.mid(e + 1).trimmed(); }
        return m;
    }

    // Source node (setting, else RB_SOURCE from bridge.env) and our address as the rig host sees it.
    void resolve(std::function<void(bool)> done) {
        if (!cfg.configured()) { error = "rig host not configured (Settings…)"; done(false); return; }
        const QString script = "echo \"client=${SSH_CONNECTION%% *}\"; f=" + cfg.remoteFile("bridge.env")
            + "; echo \"rbsrc=$(sed -n 's/^[[:space:]]*RB_SOURCE=//p' \"$f\" 2>/dev/null | tail -n 1 | tr -d \"\\\"' \")\"\n";
        busy = true;
        run("ssh", sshArgs("bash -s"), script.toUtf8(), 10000, [this, done](bool ok, QByteArray out) {
            busy = false;
            if (!ok) { error = cfg.rigName() + " unreachable (ssh)"; say(error); done(false); return; }
            auto m = kv(out);
            srcNode = naCfg.sourceNode.isEmpty() ? m.value("rbsrc") : naCfg.sourceNode;
            destIp = naCfg.destAddr.isEmpty() ? m.value("client") : naCfg.destAddr;
            if (srcNode.isEmpty() || !NetAudioCfg::nodeOk(srcNode)) { error = "no source node: set it in Settings > Network audio (or RB_SOURCE in bridge.env)"; say(error); done(false); return; }
            if (!NetAudioCfg::ipOk(destIp)) { error = "couldn't determine this computer's address; set Send-to address in Settings"; say(error); done(false); return; }
            error.clear(); done(true);
        });
    }

    // Shell: (re)start unit U from files already written to D. Transient via systemd-run unless an
    // installed unit file exists. NEED_SRC: refuse to start if that capture node isn't present.
    static QString startScript(const QString &unit, const QString &dir, const QString &desc, const QString &cmd,
                               const QList<QPair<QString, QString>> &files, const QStringList &removeFiles, const QString &needSrc) {
        QString s = QString("U=%1\nD=\"%2\"\nmkdir -p \"$D\" || exit 1\n").arg(shq(unit), dir);
        for (const auto &f : files)
            s += QString("cat > \"$D/%1.tmp\" <<'FT710_NETAUDIO_EOF'\n%2\nFT710_NETAUDIO_EOF\nmv -f \"$D/%1.tmp\" \"$D/%1\"\n").arg(f.first, f.second);
        for (const auto &r : removeFiles) s += QString("rm -f \"$D/%1\"\n").arg(r);
        if (!needSrc.isEmpty())
            s += QString("if ! pactl list sources short 2>/dev/null | cut -f2 | grep -qxF %1; then systemctl --user stop \"$U\" 2>/dev/null; echo result=nosource; exit 0; fi\n").arg(shq(needSrc));
        s += "frag=$(systemctl --user show -p FragmentPath --value \"$U.service\" 2>/dev/null)\n"
             "case \"$frag\" in ''|/run/*) inst=0;; *) inst=1;; esac\n"
             "if [ $inst = 1 ]; then systemctl --user restart \"$U\" && echo result=ok || echo result=fail\n"
             "else systemctl --user stop \"$U\" 2>/dev/null; systemctl --user reset-failed \"$U\" 2>/dev/null\n"
             "  systemd-run --user --quiet --collect --unit=\"$U\" -p Description=" + shq(desc) + " " + cmd
             + " && echo result=ok || echo result=fail\nfi\n";
        return s;
    }
    static QString stopScript(const QString &unit) {
        return QString("systemctl --user stop %1 2>/dev/null; systemctl --user reset-failed %1 2>/dev/null; true\n").arg(shq(unit));
    }

    static bool runningHasChain() {
        QFile f(localDir() + "/receiver.conf");
        return f.open(QIODevice::ReadOnly) && f.readAll().contains("ft710_rxlimit");
    }
    // pw-cli set-param on the filter-chain; one at a time, the latest value wins (slider drags).
    void applyRxGain() {
        if (!chainRunning) return;
        if (gainBusy) { gainAgain = true; return; }
        const double g = NetAudioCfg::clampGain(naCfg.rxGain);
        gainBusy = true; gainAgain = false;
        run("pw-cli", {"set-param", GAIN_NODE, "Props", QString("{ params = [ \"rx:Gain\" %1 ] }").arg(g, 0, 'f', 4)}, {}, 3000,
            [this, g](bool ok, QByteArray) {
                gainBusy = false;
                if (ok) gainApplied = g; else say("RX gain: couldn't reach the receiver's gain stage");
                if (gainAgain || (ok && std::abs(NetAudioCfg::clampGain(naCfg.rxGain) - g) > 1e-6)) applyRxGain();
                emitChanged();
            });
    }
    void restartLocal(std::function<void(bool)> done) {
        killProc(meter); killProc(listen);
        const QString d = localDir();
        const QString plugin = limiterPlugin();
        if (plugin.isEmpty()) say("RX gain: couldn't write the limiter plugin — receiving at unity gain");
        const QString sc = startScript(RECV_UNIT, d, "FT-710 Remote network audio receiver", "pipewire -c \"$D/receiver.conf\"",
                                       {{"receiver.conf", receiverConf(plugin)}}, {}, {});
        chainRunning = false; gainApplied = -1;
        localBusy = true;
        run("bash", {"-s"}, sc.toUtf8(), 8000, [this, done](bool, QByteArray out) {
            localBusy = false;
            const bool ok = kv(out).value("result") == "ok";
            localUnit = ok ? "active" : "failed";
            if (ok && runningHasChain()) { chainRunning = true; gainApplied = NetAudioCfg::clampGain(naCfg.rxGain); }
            say(ok ? QString("receiver started (RX gain %1 dB)").arg(NetAudioCfg::gainDb(naCfg.rxGain), 0, 'f', 1) : "receiver failed to start");
            if (ok) QTimer::singleShot(700, this, [this] { if (wanted) { startMeter(); syncListen(); } });
            done(ok);
        });
    }
    void restartRemote(std::function<void(bool)> done) {
        const auto f = senderFile(srcNode, destIp);
        const QString other = f.first == "sender.sh" ? "sender.conf" : "sender.sh";
        const QString cmd = f.first == "sender.sh" ? "/bin/sh \"$D/sender.sh\"" : "pipewire -c \"$D/sender.conf\"";
        const QString sc = startScript(SEND_UNIT, REMOTE_DIR, "FT-710 Remote network audio sender", cmd, {f}, {other}, srcNode);
        remoteBusy = true;
        run("ssh", sshArgs("bash -s"), sc.toUtf8(), 12000, [this, done](bool sshOk, QByteArray out) {
            remoteBusy = false;
            const QString r = kv(out).value("result");
            if (r == "nosource") { srcPresent = 0; remoteUnit = "inactive"; say("FT-710 USB audio not present on the rig host"); done(false); return; }
            if (!sshOk && r.isEmpty()) { remoteUnit = "unreachable"; say("sender: rig host unreachable"); done(false); return; }
            srcPresent = 1; remoteUnit = r == "ok" ? "active" : "failed";
            say(r == "ok" ? QString("sender started on %1 → %2").arg(cfg.rigName(), destIp) : QString("sender failed to start on %1").arg(cfg.rigName()));
            done(r == "ok");
        });
    }
    void restartBoth() {
        busy = true; up = false; emitChanged();
        restartLocal([this](bool) {
            if (!wanted) { busy = false; return; }
            restartRemote([this](bool) { busy = false; graceSince.restart(); silentSince.restart(); emitChanged(); });
        });
    }

    // Level meter on the receive node (8 kHz mono s16 is plenty for a level/silence check).
    void startMeter() {
        killProc(meter);
        meter = new QProcess(this);
        meterBuf.clear();
        connect(meter, &QProcess::readyReadStandardOutput, this, [this] {
            meterBuf += meter->readAllStandardOutput();
            const int n = int(meterBuf.size() / 2);
            if (n < 400) return;   // 50 ms blocks
            const qint16 *s = reinterpret_cast<const qint16 *>(meterBuf.constData());
            double sum = 0; bool nonzero = false;
            for (int i = 0; i < n; ++i) { sum += double(s[i]) * s[i]; if (s[i]) nonzero = true; }
            meterBuf.remove(0, n * 2);
            lastData.restart();
            if (nonzero) silentSince.restart();
            const double db = sum > 0 ? 10 * std::log10(sum / n / (32768.0 * 32768.0)) : -120;
            level = level < -100 ? db : 0.7 * level + 0.3 * db;
        });
        connect(meter, &QProcess::finished, this, [this] {
            if (!meter) return;
            meter->deleteLater(); meter = nullptr;
            if (wanted) QTimer::singleShot(1000, this, [this] { if (wanted && !meter && localUnit == "active") startMeter(); });
        });
        meter->start("pw-record", {"--target", NODE, "-P", "{ node.name = ft710_rx_meter node.description = \"FT-710 Remote level meter\" "
                                   "node.dont-fallback = true node.dont-reconnect = true }",
                                   "--rate", "8000", "--channels", "1", "--format", "s16", "--raw", "-"});
        lastData.restart();
    }
    // Local monitor through the speakers: a pw-loopback from the network source, only while unmuted,
    // so the level apps record from "FT-710 RX (network)" is never affected.
    void syncListen() {
        const bool want = wanted && !naCfg.listenMute && localUnit == "active";
        if (!want) { killProc(listen); return; }
        if (!listen) {
            listen = new QProcess(this);
            connect(listen, &QProcess::finished, this, [this] { if (listen) { listen->deleteLater(); listen = nullptr; listenApplied = -1; } });
            listen->start("pw-loopback", {"-n", "ft710-netaudio-listen", "-c", "1", "-m", "[ MONO ]", "-C", NODE,
                                          "-i", "{ node.dont-fallback = true node.dont-reconnect = true }",
                                          "-o", "{ node.name = ft710_rx_listen node.description = \"FT-710 RX (network) listen\" "
                                                "application.name = \"FT-710 Remote network audio\" }"});
            listenApplied = -1;
        }
        if (listenApplied == naCfg.listenVol) return;
        run("pactl", {"-f", "json", "list", "sink-inputs"}, {}, 3000, [this](bool ok, QByteArray out) {
            if (!ok) return;
            for (const QJsonValue &it : QJsonDocument::fromJson(out).array()) {
                const QJsonObject o = it.toObject();
                if (o["properties"].toObject()["node.name"].toString() != "ft710_rx_listen") continue;
                run("pactl", {"set-sink-input-volume", QString::number(o["index"].toInt()), QString("%1%").arg(naCfg.listenVol)}, {}, 3000, nullptr);
                listenApplied = naCfg.listenVol;
            }
        });
    }

    void pollRemote() {
        if (!wanted || remoteBusy || busy || srcNode.isEmpty()) return;
        remoteBusy = true;
        const QString sc = QString("echo unit=$(systemctl --user is-active %1 2>/dev/null)\n"
                                   "pactl list sources short 2>/dev/null | cut -f2 | grep -qxF %2 && echo src=1 || echo src=0\n").arg(SEND_UNIT, shq(srcNode));
        run("ssh", sshArgs("bash -s"), sc.toUtf8(), 8000, [this](bool ok, QByteArray out) {
            remoteBusy = false;
            if (!wanted) return;
            if (!ok) { remoteUnit = "unreachable"; emitChanged(); return; }
            auto m = kv(out);
            remoteUnit = m.value("unit", "unknown");
            const int was = srcPresent;
            srcPresent = m.value("src") == "1" ? 1 : 0;
            if (was == 0 && srcPresent == 1 && !busy) {   // USB audio came back: restart the sender right away
                say("FT-710 USB audio is back — restarting the sender");
                recover.stop(); backoff = 0;
                remoteBusy = true;
                restartRemote([this](bool) { graceSince.restart(); silentSince.restart(); emitChanged(); });
            }
            emitChanged();
        });
    }
    void pollLocal() {
        if (localBusy) return;
        localBusy = true;
        run("systemctl", {"--user", "is-active", RECV_UNIT}, {}, 3000, [this](bool, QByteArray out) {
            localBusy = false; localUnit = QString::fromUtf8(out).trimmed();
            if (localUnit.isEmpty()) localUnit = "unknown";
            // Keep the listen monitor in step with the receiver (needed when adopting a running stream:
            // at adopt time the unit state is still unknown, so the first syncListen() did nothing).
            if (wanted && !busy) syncListen();
        });
    }

    void tick() {
        if (!wanted) return;
        if (!lastLocalPoll.isValid() || lastLocalPoll.elapsed() > 1500) { lastLocalPoll.restart(); pollLocal(); }
        if (meter && lastData.elapsed() > 3000) { killProc(meter); if (localUnit == "active") startMeter(); }   // meter stuck
        if (busy) { emitChanged(); return; }
        const bool grace = graceSince.elapsed() < GRACE_MS;
        const bool silent = silentSince.elapsed() > SILENCE_MS;
        QString why;
        if (localUnit != "active" && localUnit != "activating" && localUnit != "unknown") why = "receiver " + localUnit;
        else if (srcPresent == 0) why = "no USB audio";
        else if (remoteUnit != "active" && remoteUnit != "activating" && remoteUnit != "unknown") why = "sender " + remoteUnit;
        else if (silent && !grace) why = "no audio";
        if (why.isEmpty()) {
            if (!up && !silent && !grace) { up = true; upSince.restart(); failsInRow = 0; recover.stop(); say("link up"); }
            if (up && upSince.elapsed() > 30000) backoff = 0;
        } else {
            if (up) { up = false; say("link down: " + why); }
            downWhy = why;
            // Waiting for the USB device: pollRemote restarts the sender when it reappears.
            if (srcPresent != 0 && !recover.isActive() && !grace) {
                const int d = BACKOFF_S[qMin(backoff, 3)];
                ++backoff;
                recover.start(d * 1000);
            }
        }
        emitChanged();
    }
    void doRecover() {
        if (!wanted || busy) return;
        ++restarts; ++failsInRow;
        say(QString("recovering (%1), attempt %2").arg(downWhy).arg(failsInRow));
        const bool localBad = localUnit != "active";
        if (localBad || failsInRow % 3 == 0) {   // every third try: rebuild both ends and re-check our address
            busy = true;
            resolve([this](bool ok) { busy = false; if (ok && wanted) restartBoth(); else { graceSince.restart(); emitChanged(); } });
        } else {
            busy = true;
            restartRemote([this](bool) { busy = false; graceSince.restart(); silentSince.restart(); emitChanged(); });
        }
        emitChanged();
    }
};
