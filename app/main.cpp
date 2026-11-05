// FT-710 Remote: small Qt6 front end for a remote Yaesu FT-710. Receive audio comes over the LAN
// (PipeWire ROC/RTP, netaudio.h), PTT and your microphone go to the ft710-txgate service on the
// "rig host" (txgate.h), CAT and status go through flrig there (ssh + curl), and the band scope comes
// from the ft710-scope streamer. All station settings come from the Settings dialog.
#include <QtWidgets>
#include <QTcpSocket>
#include <functional>
#include <memory>
#include <cmath>
#include "tune.h"
#include "devname.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------------------------
// Station configuration (QSettings, edited in the Settings dialog). Nothing station-specific is
// compiled in. The "rig host" is the machine with the FT-710, flrig, ft710-txgate, the network-audio
// sender and ft710-scope; this app reaches it with ssh (key-based login, BatchMode) for status, TX
// enable/level and CAT via flrig.
static QString shq(const QString &s) { QString r = s; r.replace("'", "'\\''"); return "'" + r + "'"; }
static QString hostPart(const QString &dest) { const int at = dest.lastIndexOf('@'); return at >= 0 ? dest.mid(at + 1) : dest; }

struct Config {
    QString sshHost;                                  // ssh destination: alias, host or user@host
    QString flrigHost = "127.0.0.1"; int flrigPort = 12345;   // flrig XML-RPC as seen from the rig host
    // Config dir on the rig host: tx_enabled (TX keying arm switch), tx_gain (TX level) and bridge.env
    // (RB_SOURCE, the radio's USB capture node). ft710-txgate reads the same dir (TG_CONFIG_DIR); the
    // default name is historical (it was radio-bridge's config dir before Mumble was removed).
    QString rigCfgDir = "~/.config/radio-bridge";
    QString scopeHost; int scopePort = 7355;          // "" = same host as the rig host
    QString stationLabel;                             // shown in the window title (e.g. your callsign)
    QString helpFile;                                 // optional local Markdown notes shown in Help

    // Keys of the removed Mumble / radio-bridge mode. Ignored when loading; deleted on the next save.
    static QStringList obsoleteKeys() {
        return {"rig/bridgeService", "rig/murmurService", "rig/bridgeConfigDir", "rig/bridgeMumbleUser", "mumble",
                "startMumbleMinimized", "rxVolume", "netaudio/txGate", "netaudio/restoreBridge"};
    }
    bool configured() const { return !sshHost.isEmpty(); }
    QString rigName() const { return configured() ? hostPart(sshHost) : QString("rig host"); }
    QString effScopeHost() const { return scopeHost.isEmpty() ? hostPart(sshHost) : scopeHost; }
    QString flrigUrl() const {
        return QString("http://%1:%2/RPC2").arg(flrigHost.contains(':') ? "[" + flrigHost + "]" : flrigHost).arg(flrigPort);
    }
    // Shell expression for a file in the rig config dir on the rig host ("~/" stays expandable).
    QString remoteFile(const QString &name) const {
        QString d = rigCfgDir; while (d.endsWith('/') && d.size() > 1) d.chop(1);
        if (d == "~") return "\"$HOME\"/" + shq(name);
        if (d.startsWith("~/")) return "\"$HOME\"/" + shq(d.mid(2) + "/" + name);
        return shq(d + "/" + name);
    }
    void load() {
        QSettings st; Config def;
        sshHost = st.value("rig/sshHost").toString().trimmed();
        flrigHost = st.value("rig/flrigHost", def.flrigHost).toString().trimmed();
        flrigPort = st.value("rig/flrigPort", def.flrigPort).toInt();
        // rig/configDir; an older install kept the same dir as rig/bridgeConfigDir
        rigCfgDir = st.value("rig/configDir", st.value("rig/bridgeConfigDir", def.rigCfgDir)).toString().trimmed();
        scopeHost = st.value("scopeHost").toString().trimmed();
        scopePort = st.value("scopePort", def.scopePort).toInt();
        stationLabel = st.value("station/label").toString().trimmed();
        helpFile = st.value("station/helpFile").toString().trimmed();
    }
    void save() const {
        QSettings st;
        st.setValue("rig/sshHost", sshHost); st.setValue("rig/flrigHost", flrigHost); st.setValue("rig/flrigPort", flrigPort);
        st.setValue("rig/configDir", rigCfgDir);
        st.setValue("scopeHost", scopeHost); st.setValue("scopePort", scopePort);
        st.setValue("station/label", stationLabel); st.setValue("station/helpFile", helpFile);
        for (const QString &k : obsoleteKeys()) st.remove(k);   // incl. a Mumble password an old version stored
        st.sync();
        QFile::setPermissions(st.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    // Returns "" if valid, else a message. Strict on anything that reaches a shell or ssh's argv.
    QString validate() const {
        static const QRegularExpression dest("^([A-Za-z0-9._-]+@)?[A-Za-z0-9_][A-Za-z0-9._-]*$"),
            host("^([A-Za-z0-9_][A-Za-z0-9._-]*|\\[?[0-9A-Fa-f:.]+\\]?)$"),
            path("^(~|~/|/)?[A-Za-z0-9._/ +-]*$");
        if (sshHost.isEmpty()) return "Rig host (ssh) is required.";
        if (!dest.match(sshHost).hasMatch()) return "Rig host (ssh) must look like host, alias or user@host.";
        if (!host.match(flrigHost).hasMatch()) return "flrig host is not a valid host name or address.";
        if (rigCfgDir.isEmpty() || !path.match(rigCfgDir).hasMatch()) return "Rig config dir must be a plain path (~/... or /...).";
        if (!scopeHost.isEmpty() && !host.match(scopeHost).hasMatch()) return "Scope host is not a valid host name or address.";
        return {};
    }
};
static Config cfg;

static QStringList sshArgs(const QString &cmd) {
    const QString cp = QDir::home().filePath(".ssh/cm-ft710-%r@%h:%p");
    return {"-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "ControlMaster=auto",
            "-o", "ControlPath=" + cp, "-o", "ControlPersist=120", "--", cfg.sshHost, cmd};
}

// Blocking ssh helper for short commands. Returns stdout; ok=false on failure.
static QByteArray sshRun(const QString &cmd, bool *ok = nullptr, int ms = 10000) {
    if (!cfg.configured()) { if (ok) *ok = false; return {}; }
    QProcess p;
    p.start("ssh", sshArgs(cmd));
    bool good = p.waitForFinished(ms) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (!good && p.state() != QProcess::NotRunning) p.kill(), p.waitForFinished(1000);
    if (ok) *ok = good;
    return p.readAllStandardOutput();
}

#include "netaudio.h"   // network audio (LAN stream) mode; uses cfg, shq() and sshArgs() above
#include "txgate.h"     // TX: mic -> ft710-txgate on the rig host; uses naCfg

// One ssh round trip per poll. Prints key=value lines, then the TX gate's log if asked.
// Settings arrive as shell variables (TXEN, TXGAIN, FLRIG, TXUNIT) prepended by pollScript().
static const char *POLL_SCRIPT = R"SH(
[ -n "$TXUNIT" ] && echo "txgate=$(systemctl --user is-active "$TXUNIT" 2>/dev/null)"
[ -e "$TXEN" ] && echo tx=on || echo tx=off
rpc() { curl -s -m 2 -H 'Content-Type: text/xml' -d "<?xml version=\"1.0\"?><methodCall><methodName>$1</methodName></methodCall>" "$FLRIG" | tr -d '\n\t ' | sed -n 's/.*<value>\(<[a-z0-9]*>\)\{0,1\}\([^<]*\).*/\2/p'; }
echo "ptt=$(rpc rig.get_ptt)"
echo "vfo=$(rpc rig.get_vfo)"
echo "txgain=$(cat "$TXGAIN" 2>/dev/null || echo none)"
if [ "$1" = log ]; then echo ---LOG---; journalctl --user -u "$TXUNIT" -n 20 --no-pager 2>/dev/null; fi
)SH";
static QByteArray pollScript() {
    const QString vars = QString("TXEN=%1\nTXGAIN=%2\nFLRIG=%3\nTXUNIT=%4\n")
        .arg(cfg.remoteFile("tx_enabled"), cfg.remoteFile("tx_gain"), shq(cfg.flrigUrl()), shq(naCfg.txUnit));
    return (vars + POLL_SCRIPT).toUtf8();
}

// ---------------------------------------------------------------------------------------------
// Band scope: spectrum + waterfall from the ft710-scope streamer on the rig host (TCP 7355 by default). The FT-710
// sends its own scope data (magnitude only, ~850 bins) over the FT4222 inside the radio when
// MENU SCU-LAN10 is ON; server/ft710-scope/ft710_scope.py on the rig host reads it and serves it here.
// Wire format (little-endian): header "<4sBBHIdQQIBBBBBxxxH" (50 bytes) = magic "F7SC", version,
// hdr_len, nbins, seq, unix time (double), vfoa_hz, start_hz, span_hz, flags, mode, span_code,
// scope_mode, smeter, meta_len; then meta_len raw parameter bytes, then nbins magnitude bytes.
// flags bit0 = data, bit1 = fixed-edge scope, bit7 = status only (no data from the radio).
// Read-only: the only thing it can do to the radio is a frequency change (flrig rig.set_vfo)
// when you click in it, and only while the radio is receiving.
class ScopeWidget : public QWidget {
public:
    std::function<void(double)> onTune;          // set by Main; called with Hz on a click
    std::function<bool()> canTune;               // false while transmitting
    std::function<void(int)> onSpanCode;         // span code (0-9) from the frame header, when it changes
    bool clickToTune = true;
    bool snap = true;            // "Snap to": round the dial frequency a click tunes to ...
    double snapStepHz = 500;     // ... to this step (500 / 1000 Hz)
    double ssbOffsetHz = 1500;   // LSB/USB: the clicked point lands this far into the passband (dial = click ± offset)
    // The dial (VFO) frequency a click at x would tune to, for the rig's current mode (from the scope frame).
    double dialForX(double x) const { return clickDialHz(freqAt(x), mode, ssbOffsetHz, span, snap ? snapStepHz : 0); }
    bool autoContrast = false;                   // off: fixed colour scale, so the radio's scope LEVEL shows
    int lastSpanCode = -1;

    explicit ScopeWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(260);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        retry.setSingleShot(true); retry.setInterval(3000);
        QObject::connect(&retry, &QTimer::timeout, this, [this] { open(); });
        QObject::connect(&sock, &QTcpSocket::readyRead, this, [this] { readData(); });
        QObject::connect(&sock, &QTcpSocket::connected, this, [this] { setStatus("connected — waiting for scope data"); });
        QObject::connect(&sock, &QTcpSocket::disconnected, this, [this] { lost("disconnected"); });
        QObject::connect(&sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { lost(sock.errorString()); });
        stale.setInterval(1000);
        QObject::connect(&stale, &QTimer::timeout, this, [this] {
            if (lastData.isValid() && lastData.elapsed() > 2500 && status.isEmpty()) { setStatus("no scope data"); }
        });
    }
    void start(const QString &host, quint16 port) { this->host = host; this->port = port; running = true; stale.start(); open(); }
    void stop() {
        running = false; retry.stop(); stale.stop();
        sock.abort(); buf.clear(); lastSpanCode = -1;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(8, 8, 20));
        const QRect area = rect().adjusted(0, 0, 0, -axisH);
        const int specH = area.height() * 35 / 100;
        const QRect spec(area.left(), area.top(), area.width(), specH), wfr(area.left(), specH, area.width(), area.height() - specH);
        if (!wf.isNull()) p.drawImage(wfr, wf);
        // spectrum trace
        if (!spectrum.isEmpty()) {
            p.fillRect(spec, QColor(10, 14, 30));
            QPolygonF line; const int n = spectrum.size();
            for (int i = 0; i < n; ++i) {
                double x = spec.left() + (i + 0.5) * spec.width() / n;
                double y = spec.bottom() - qBound(0.0, (uchar(spectrum[i]) - lo) / qMax(1.0, hi - lo), 1.0) * (spec.height() - 4);
                line << QPointF(x, y);
            }
            QPolygonF fill = line; fill << QPointF(spec.right(), spec.bottom()) << QPointF(spec.left(), spec.bottom());
            p.setPen(Qt::NoPen); p.setBrush(QColor(40, 120, 220, 90)); p.drawPolygon(fill);
            p.setPen(QPen(QColor(120, 210, 255), 1)); p.setBrush(Qt::NoBrush); p.drawPolyline(line);
        }
        // passband + VFO marker
        if (span > 0 && vfo > 0) {
            const double xv = xOf(vfo);
            p.fillRect(passRect(vfo, area), QColor(255, 255, 255, 38));
            p.setPen(QPen(QColor(255, 80, 80), 1)); p.drawLine(QPointF(xv, area.top()), QPointF(xv, area.bottom()));
        }
        // axis
        p.fillRect(QRect(0, area.bottom() + 1, width(), axisH), QColor(20, 20, 30));
        p.setPen(QColor(200, 200, 200));
        if (span > 0) {
            const double step = niceStep(span / qMax(2.0, width() / 90.0));
            const int dec = step >= 1e6 ? 0 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : step >= 1e3 ? 3 : step >= 100 ? 4 : 5;
            for (double f = std::ceil(startHz / step) * step; f <= startHz + span; f += step) {
                const double x = xOf(f);
                p.drawLine(QPointF(x, area.bottom() + 1), QPointF(x, area.bottom() + 5));
                const QString t = QString::number(f / 1e6, 'f', dec);
                const int tw = p.fontMetrics().horizontalAdvance(t);
                p.drawText(QPointF(qBound(0.0, x - tw / 2.0, double(width() - tw)), height() - 4), t);
            }
        }
        // hover readout / status
        p.setPen(QColor(255, 255, 160));
        QString top = status;
        if (top.isEmpty() && span > 0) {
            top = QString("%1 kHz span%2").arg(span / 1000.0, 0, 'g', 4).arg(fixedEdge ? " (fixed)" : "");
            if (hoverX >= 0) {
                top += QString("   cursor %1 MHz").arg(freqAt(hoverX) / 1e6, 0, 'f', 4);
                if (clickToTune) {
                    const double d = dialForX(hoverX);
                    top += QString(" → dial %1").arg(d / 1e6, 0, 'f', (snap && snapStepHz >= 1000) ? 3 : 4);
                    if (tuneSide(mode) && ssbOffsetHz > 0)
                        top += QString(" (%1 %2%3 kHz)").arg(tuneSideName(mode)).arg(tuneSide(mode) < 0 ? "+" : "−").arg(ssbOffsetHz / 1000.0, 0, 'g', 3);
                }
            }
            if (clickToTune) top += (canTune && !canTune()) ? "   (tuning disabled while TX)" : "   click to tune";
        }
        p.drawText(rect().adjusted(6, 4, -6, 0), Qt::AlignTop | Qt::AlignLeft, top);
        if (hoverX >= 0 && span > 0 && status.isEmpty()) {
            p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DashLine));
            p.drawLine(QPointF(hoverX, area.top()), QPointF(hoverX, area.bottom()));
            // where a click would put the dial and the passband (outline), so the signal under the cursor sits inside it
            if (clickToTune && (!canTune || canTune())) {
                const double d = dialForX(hoverX);
                if (d > 0 && passband().second > passband().first) {   // exactly the stripe the rig will have after the click
                    p.setPen(QPen(QColor(255, 200, 80, 170), 1, Qt::DashLine)); p.setBrush(Qt::NoBrush);
                    p.drawRect(passRect(d, area));
                }
                if (d > 0) { p.setPen(QPen(QColor(255, 140, 60, 200), 1, Qt::DotLine)); p.drawLine(QPointF(xOf(d), area.top()), QPointF(xOf(d), area.bottom())); }
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override { hoverX = e->position().x(); update(); }
    void leaveEvent(QEvent *) override { hoverX = -1; update(); }
    void mousePressEvent(QMouseEvent *e) override { if (e->button() == Qt::LeftButton) pressPos = e->position(); }
    void mouseReleaseEvent(QMouseEvent *e) override {
        // Only a deliberate left click (no drag) in the scope tunes, and never while transmitting.
        if (e->button() != Qt::LeftButton || !clickToTune || span <= 0 || !status.isEmpty()) return;
        if ((e->position() - pressPos).manhattanLength() > 6) return;
        if (canTune && !canTune()) return;
        const double hz = dialForX(e->position().x());   // mode-aware (SSB offset), then snapped
        if (onTune && hz > 0) onTune(hz);
    }

public:   // state + mapping are public for --scope-selftest
    QTcpSocket sock; QTimer retry, stale; QString host; quint16 port = 7355; bool running = false;
    QByteArray buf, spectrum; QImage wf; QString status = "connecting…";
    QElapsedTimer lastData;
    qint64 vfo = 0, startHz = 0, span = 0; int mode = -1; bool fixedEdge = false;
    double lo = 20, hi = 160; double hoverX = -1; QPointF pressPos;
    static constexpr int axisH = 18, WF_ROWS = 240;

    void open() {
        if (!running) return;
        buf.clear(); sock.abort(); sock.connectToHost(host, port);
    }
    void lost(const QString &why) {
        if (!running) return;
        setStatus(QString("scope streamer %1:%2 — %3 (retrying)").arg(host).arg(port).arg(why));
        if (!retry.isActive()) retry.start();
    }
    void setStatus(const QString &s) { if (status != s) { status = s; update(); } }
    double xOf(double hz) const { return (hz - startHz) * width() / double(span); }
    double freqAt(double x) const { return startHz + x * double(span) / qMax(1, width()); }
    static double niceStep(double raw) {
        const double p10 = std::pow(10.0, std::floor(std::log10(qMax(raw, 1.0))));
        for (double m : {1.0, 2.0, 5.0, 10.0}) if (m * p10 >= raw) return m * p10;
        return 10 * p10;
    }
    std::pair<double, double> passband() const { return passbandHz(mode); }
public:
    // The passband stripe for a given dial frequency — one code path for the grey stripe (dial = VFO)
    // and the hover preview (dial = where a click would tune), so the preview is what you get.
    QRectF passRect(double dial, const QRect &area) const {
        auto [a, b] = passband();
        const double x0 = xOf(dial + a), x1 = xOf(dial + b);
        return QRectF(qMin(x0, x1), area.top(), qMax(1.0, qAbs(x1 - x0)), area.height());
    }
    // (readData/frame are public so --scope-selftest can feed frames)
    void readData() {
        buf += sock.readAll();
        static const int H = 50;
        while (buf.size() >= H) {
            const uchar *d = reinterpret_cast<const uchar *>(buf.constData());
            if (memcmp(d, "F7SC", 4) != 0 || d[4] != 1) { lost("bad stream header"); sock.abort(); return; }
            const int hdr = d[5]; quint16 nb; quint16 ml;
            memcpy(&nb, d + 6, 2); memcpy(&ml, d + 48, 2);
            const int total = hdr + ml + nb;
            if (buf.size() < total) break;
            quint64 v, st; quint32 sp; memcpy(&v, d + 20, 8); memcpy(&st, d + 28, 8); memcpy(&sp, d + 36, 4);
            const uchar flags = d[40];
            if (flags & 0x80) setStatus("radio scope not streaming (radio off, SCU-LAN10 OFF, or no USB permission on the rig host)");
            else if ((flags & 1) && nb > 0 && d[42] != lastSpanCode) { lastSpanCode = d[42]; if (onSpanCode) onSpanCode(d[42]); }
            if (!(flags & 0x80) && (flags & 1) && nb > 0) {
                const qint64 start = scopeStartHz(d + hdr, ml, qint64(v), qint64(sp), qint64(st), flags & 2);   // radio's real centre (SSB: dial ∓ 1.5 kHz)
                frame(qint64(v), start, qint64(sp), d[41], flags & 2, buf.mid(hdr + ml, nb));
            }
            buf.remove(0, total);
        }
    }
    void frame(qint64 v, qint64 st, qint64 sp, int md, bool fixed, const QByteArray &bins) {
        lastData.start();
        if (!status.isEmpty()) status.clear();
        if (st != startHz || sp != span || wf.width() != bins.size()) {   // new view: start a fresh waterfall
            wf = QImage(bins.size(), WF_ROWS, QImage::Format_RGB32); wf.fill(QColor(0, 0, 40));
        }
        vfo = v; startHz = st; span = sp; mode = md; fixedEdge = fixed; spectrum = bins;
        // adaptive range: noise floor (20th percentile) and peak, smoothed
        QByteArray s = bins; std::sort(s.begin(), s.end(), [](char a, char b) { return uchar(a) < uchar(b); });
        const double floor = uchar(s[s.size() / 5]), peak = uchar(s[s.size() - 1 - s.size() / 200]);
        if (autoContrast) {
            lo = lo * 0.95 + (floor - 4) * 0.05;
            hi = hi * 0.95 + qMax(peak, floor + 40) * 0.05;
        } else { lo = 20; hi = 160; }   // fixed: radio scope LEVEL moves the data ~2 counts/dB (measured)
        // scroll down one row, draw the new row on top
        const int bpl = wf.bytesPerLine();
        memmove(wf.bits() + bpl, wf.bits(), size_t(bpl) * (wf.height() - 1));
        auto *row = reinterpret_cast<QRgb *>(wf.scanLine(0));
        for (int i = 0; i < bins.size(); ++i) row[i] = color(uchar(bins[i]));
        update();
    }
    QRgb color(int x) const {   // dark blue -> cyan -> yellow -> red
        const double t = qBound(0.0, (x - lo) / qMax(1.0, hi - lo), 1.0);
        static const double P[4][4] = {{0, 0, 0, 40}, {0.35, 0, 120, 200}, {0.65, 230, 230, 0}, {1, 255, 30, 0}};
        for (int k = 0; k < 3; ++k)
            if (t <= P[k + 1][0]) {
                const double u = (t - P[k][0]) / (P[k + 1][0] - P[k][0]);
                return qRgb(int(P[k][1] + (P[k + 1][1] - P[k][1]) * u), int(P[k][2] + (P[k + 1][2] - P[k][2]) * u),
                            int(P[k][3] + (P[k + 1][3] - P[k][3]) * u));
            }
        return qRgb(255, 30, 0);
    }
};

class Main : public QWidget {
public:
    Main() {
        updateTitle();
        auto *v = new QVBoxLayout(this);

        // Status line (Start/Stop in the "RX network" row connects and disconnects)
        auto *row = new QHBoxLayout;
        statusLbl = new QLabel;
        row->addWidget(statusLbl, 1);
        v->addLayout(row);

        // PTT
        ptt = new QPushButton("PTT\n(hold, or hold Space)");
        ptt->setMinimumHeight(140);
        QFont f = ptt->font(); f.setPointSize(f.pointSize() + 10); f.setBold(true); ptt->setFont(f);
        v->addWidget(ptt);

        // TX level: gain the TX gate on the rig host applies to your microphone audio before it plays it into
        // the FT-710 USB input (<rig config dir>/tx_gain, linear factor = (slider%/100)^3, same curve as a
        // PulseAudio volume). The gate re-reads the file, so changes apply live and survive restarts.
        auto *txRow = new QHBoxLayout;
        auto *txName = new QLabel("TX level (to radio):");
        txRow->addWidget(txName);
        txSlider = new QSlider(Qt::Horizontal);
        txSlider->setRange(0, 150); txSlider->setPageStep(5); txSlider->setSingleStep(1);
        txSlider->setTickPosition(QSlider::TicksBelow); txSlider->setTickInterval(25);
        txLbl = new QLabel("—"); txLbl->setMinimumWidth(txLbl->fontMetrics().horizontalAdvance("150%  "));
        txResetBtn = new QToolButton; txResetBtn->setText("100%");
        txRow->addWidget(txSlider, 1); txRow->addWidget(txLbl); txRow->addWidget(txResetBtn);
        v->addLayout(txRow);
        setTxEnabled(false);

        // Network audio (Settings > Network audio, ROC/RTP mode): RX level of the "FT-710 RX (network)"
        // device, a local listen monitor (volume/mute; never changes what WSJT-X etc. record) and Start/Stop.
        na = new NetAudio(this);
        netRow = new QWidget; auto *nr = new QHBoxLayout(netRow); nr->setContentsMargins(0, 0, 0, 0);
        auto *netName = new QLabel("RX network:");
        netLevel = new QProgressBar; netLevel->setRange(0, 60); netLevel->setValue(0); netLevel->setTextVisible(true);
        netLevel->setFormat("—"); netLevel->setMaximumWidth(140);
        netLevel->setToolTip("Level on \"FT-710 RX (network)\" after RX gain — what WSJT-X/fldigi get (dBFS). "
                             "Digital silence for more than 2 s counts as link down.");
        // RX gain: 0.1 dB steps, -6 .. +20 dB (0.5x .. 10x). Applied on this computer by a limiter that keeps
        // peaks at -1 dBFS, so every position is clip-free. Changes are live (no restart).
        auto *gainName = new QLabel("RX gain:");
        netGain = new QSlider(Qt::Horizontal);
        netGain->setRange(int(std::lround(NetAudioCfg::gainDb(NetAudioCfg::RX_GAIN_MIN) * 10)), int(std::lround(NetAudioCfg::gainDb(NetAudioCfg::RX_GAIN_MAX) * 10)));
        netGain->setSingleStep(5); netGain->setPageStep(30); netGain->setTickPosition(QSlider::TicksBelow); netGain->setTickInterval(60);
        netGain->setToolTip("RX gain of the network audio on this computer (-6 to +20 dB, default +9.5 dB = 3x). "
                            "A limiter holds peaks at -1 dBFS, so no setting clips. Sets what WSJT-X/fldigi and the "
                            "listen monitor get from \"FT-710 RX (network)\". Double-click the value for the default.");
        netGain->setMinimumWidth(90);
        netGainLbl = new QLabel; netGainLbl->setMinimumWidth(netGainLbl->fontMetrics().horizontalAdvance("+20.0 dB "));
        netGainLbl->setToolTip("RX gain (double-click: back to +9.5 dB)"); netGainLbl->installEventFilter(this);
        setGainSlider(naCfg.rxGain);
        netVol = new QSlider(Qt::Horizontal); netVol->setRange(0, 150); netVol->setPageStep(5);
        netVol->setTickPosition(QSlider::TicksBelow); netVol->setTickInterval(25); netVol->setValue(naCfg.listenVol);
        netVol->setToolTip("Volume of the local listen monitor of the network stream (speakers). Does not change the level "
                           "WSJT-X/fldigi get from \"FT-710 RX (network)\".");
        netVolLbl = new QLabel(QString("%1%").arg(naCfg.listenVol)); netVolLbl->setMinimumWidth(netVolLbl->fontMetrics().horizontalAdvance("150%  "));
        netMuteBtn = new QToolButton; netMuteBtn->setCheckable(true); netMuteBtn->setChecked(naCfg.listenMute);
        netMuteBtn->setToolTip("Listen to the network stream on this computer's speakers.");
        netStartBtn = new QPushButton("Start");
        netStartBtn->setToolTip("Start: the network receive audio and the link to the TX gate on the rig host (PTT).\n"
                                "Stop: releases PTT and stops the network receive audio and the TX gate (it unkeys first).");
        nr->addWidget(netName); nr->addWidget(netLevel); nr->addWidget(gainName); nr->addWidget(netGain, 1); nr->addWidget(netGainLbl); nr->addWidget(netVol, 1); nr->addWidget(netVolLbl); nr->addWidget(netMuteBtn); nr->addWidget(netStartBtn);
        v->addWidget(netRow);
        // TX mic: level of what PTT would send to the TX gate.
        micRow = new QWidget; auto *mr = new QHBoxLayout(micRow); mr->setContentsMargins(0, 0, 0, 0);
        auto *micName = new QLabel("TX mic:");
        micBar = new QProgressBar; micBar->setRange(0, 60); micBar->setValue(0); micBar->setTextVisible(true); micBar->setFormat("—");
        micBar->setToolTip("Level of the microphone the app sends to the TX gate while PTT is held (dBFS). "
                           "Aim for peaks around -12 to -6 dBFS; set the mic level in your desktop sound settings "
                           "and the level into the radio with \"TX level\".");
        micDev = new QLabel; micDev->setStyleSheet("color:gray;");
        mr->addWidget(micName); mr->addWidget(micBar, 1); mr->addWidget(micDev);
        v->addWidget(micRow);
        { QLabel *a = qobject_cast<QLabel *>(nr->itemAt(0)->widget());
          int w = qMax(qMax(a->sizeHint().width(), micName->sizeHint().width()), txName->sizeHint().width());
          a->setMinimumWidth(w); micName->setMinimumWidth(w); txName->setMinimumWidth(w); }
        tx = new TxLink(this);
        tx->changed = [this] { updateTxUi(); };
        tx->logLine = [](const QString &m) { QTextStream(stderr) << "txgate: " << m << Qt::endl; };
        tx->catEvent = [this](const QString &e) { catGateEvent(e); };
        tx->pttLost = [this](const QString &why) {
            if (!talking || !talkNet) return;
            talking = false; talkNet = false; tx->setPtt(false);
            resetPttButton(); note("PTT dropped: " + why);
            fastPollSince.start(); adjustPollRate();
        };
        micUi.setInterval(100);
        connect(&micUi, &QTimer::timeout, this, [this] { updateMicUi(); });
        auto syncMute = [this](bool on) { netMuteBtn->setText(on ? "Listen: off" : "Listen: on"); netMuteBtn->setStyleSheet(on ? "" : "background:#2a2; color:white;"); };
        syncMute(naCfg.listenMute);
        connect(netMuteBtn, &QToolButton::toggled, this, [this, syncMute](bool on) { syncMute(on); na->setListen(netVol->value(), on); });
        connect(netVol, &QSlider::valueChanged, this, [this](int val) { netVolLbl->setText(QString("%1%").arg(val)); netVolApply.start(); });
        netVolApply.setSingleShot(true); netVolApply.setInterval(100);
        connect(netGain, &QSlider::valueChanged, this, [this](int v10) {
            netGainLbl->setText(fmtGainDb(v10 / 10.0));
            if (!gainSync) netGainApply.start();
        });
        netGainApply.setSingleShot(true); netGainApply.setInterval(80);   // live, coalesced while dragging
        connect(&netGainApply, &QTimer::timeout, this, [this] { na->setRxGain(std::pow(10.0, netGain->value() / 200.0)); });
        connect(&netVolApply, &QTimer::timeout, this, [this] { na->setListen(netVol->value(), netMuteBtn->isChecked()); });
        connect(netStartBtn, &QPushButton::clicked, this, [this] { if (na->active() || tx->active()) doDisconnect(); else doConnect(); });
        na->changed = [this] { updateNetUi(); };
        na->logLine = [](const QString &m) { QTextStream(stderr) << "netaudio: " << m << Qt::endl; };
        txApply.setSingleShot(true); txApply.setInterval(150);   // coalesce slider drags (one ssh per settle)
        connect(&txApply, &QTimer::timeout, this, [this] { applyTx(txSlider->value()); });
        connect(txSlider, &QSlider::valueChanged, this, [this](int val) {
            txLbl->setText(QString("%1%").arg(val));
            if (txSync) return;
            txTouched.start();
            txApply.start();
        });
        connect(txResetBtn, &QToolButton::clicked, this, [this] { txSlider->setValue(100); });

        // Band scope (spectrum + waterfall from the radio, via the ft710-scope streamer on the rig host)
        auto *scopeRow = new QHBoxLayout;
        scopeChk = new QCheckBox("Show band scope");
        scopeChk->setToolTip("Spectrum + waterfall from the FT-710's own scope (needs MENU SCU-LAN10 = ON and the "
                             "ft710-scope service on the rig host, see Settings). Receive-only display.");
        scopeTuneChk = new QCheckBox("Click to tune");
        scopeTuneChk->setToolTip("A single left click in the scope sets VFO-A to that frequency through flrig "
                                 "(rig.set_vfo). Disabled while transmitting. Never keys the radio.");
        // Click-to-tune options: snap step and SSB offset (LSB: dial = click + offset, USB: dial = click − offset)
        scopeTuneOpts = new QWidget; auto *to = new QHBoxLayout(scopeTuneOpts); to->setContentsMargins(0, 0, 0, 0);
        scopeSnapChk = new QCheckBox("Snap to");
        scopeSnapChk->setToolTip("Round the dial frequency a click tunes to (after the SSB offset) to the step on the right. "
                                 "Off: 10/100/500 Hz depending on the span.");
        scopeSnapStep = new QComboBox; scopeSnapStep->addItem("0.5 kHz", 500); scopeSnapStep->addItem("1 kHz", 1000);
        scopeSnapStep->setToolTip("Snap step for click to tune (applied to the final dial frequency)");
        auto *offLbl = new QLabel("SSB offset");
        scopeSsbOff = new QSpinBox; scopeSsbOff->setRange(0, 3000); scopeSsbOff->setSingleStep(100); scopeSsbOff->setSuffix(" Hz");
        scopeSsbOff->setToolTip("Click the middle of an SSB signal and it lands this far into the passband: LSB/DATA-L "
                                "dial = click + offset, USB/DATA-U dial = click − offset (1500 Hz = middle of 300–2700 Hz). "
                                "CW, AM, FM, RTTY: the click is the dial frequency. 0 = always direct.");
        offLbl->setToolTip(scopeSsbOff->toolTip());
        to->addWidget(scopeSnapChk); to->addWidget(scopeSnapStep); to->addSpacing(8); to->addWidget(offLbl); to->addWidget(scopeSsbOff);
        scopeRow->addWidget(scopeChk); scopeRow->addWidget(scopeTuneChk); scopeRow->addWidget(scopeTuneOpts); scopeRow->addStretch(1);
        v->addLayout(scopeRow);
        // Scope SPAN and LEVEL on the radio (CAT SS05 / SS04 via flrig rig.cat_string: display settings only)
        scopeCtl = new QWidget;
        auto *sc = new QHBoxLayout(scopeCtl); sc->setContentsMargins(0, 0, 0, 0);
        spanBox = new QComboBox;
        for (const char *t : {"1 kHz", "2 kHz", "5 kHz", "10 kHz", "20 kHz", "50 kHz", "100 kHz", "200 kHz", "500 kHz", "1 MHz"})
            spanBox->addItem(t);
        spanBox->setCurrentIndex(-1);
        spanBox->setToolTip("Scope span on the radio (CAT SS05). Current value comes from the scope stream.");
        lvlSlider = new QSlider(Qt::Horizontal); lvlSlider->setRange(-60, 60); lvlSlider->setSingleStep(1); lvlSlider->setPageStep(10);
        lvlSlider->setToolTip("Scope LEVEL on the radio, -30.0 to +30.0 dB in 0.5 dB steps (CAT SS04). Raises/lowers the "
                              "whole scope and waterfall intensity. Double-click the value to re-read it from the radio.");
        lvlSlider->setMinimumWidth(160); lvlSlider->setEnabled(false);
        lvlLbl = new QLabel("…"); lvlLbl->setMinimumWidth(64);
        autoChk = new QCheckBox("Auto contrast");
        autoChk->setToolTip("Off: fixed colours, so the radio's LEVEL setting changes what you see. On: the app "
                            "rescales colours to the noise floor and peaks (LEVEL then has little visible effect).");
        sc->addWidget(new QLabel("Span")); sc->addWidget(spanBox); sc->addSpacing(12);
        sc->addWidget(new QLabel("Level")); sc->addWidget(lvlSlider, 1); sc->addWidget(lvlLbl); sc->addSpacing(12);
        sc->addWidget(autoChk);
        v->addWidget(scopeCtl);
        scope = new ScopeWidget;
        v->addWidget(scope, 1);
        scope->canTune = [this] { return radioRx && !talking && !holdTimer.isActive(); };
        scope->onTune = [this](double hz) { tuneTo(hz); };
        scope->onSpanCode = [this](int c) {
            if (c < 0 || c > 9 || (spanTouched.isValid() && spanTouched.elapsed() < 3000)) return;
            QSignalBlocker b(spanBox); spanBox->setCurrentIndex(c);
        };
        connect(spanBox, qOverload<int>(&QComboBox::activated), this, [this](int c) {
            if (c < 0 || c > 9) return;
            spanTouched.start();
            scopeCat(QString("SS05%1").arg(c) + "0000;");
        });
        geomSave.setSingleShot(true); geomSave.setInterval(600);   // debounced size save (survives a kill)
        connect(&geomSave, &QTimer::timeout, this, [this] { saveGeom(); });
        lvlApply.setSingleShot(true); lvlApply.setInterval(300);   // debounce slider drags: one set per settle
        connect(&lvlApply, &QTimer::timeout, this, [this] { scopeCat(levelCmd(lvlSlider->value())); });
        connect(lvlSlider, &QSlider::valueChanged, this, [this](int h) {
            lvlLbl->setText(QString("%1 dB").arg(h / 2.0, 0, 'f', 1));
            if (lvlSync) return;
            lvlTouched.start(); lvlApply.start();
        });
        connect(&lvlPoll, &QTimer::timeout, this, [this] { queryLevel(); });
        lvlPoll.setInterval(15000);
        {
            const bool ac = QSettings().value("scopeAutoContrast", false).toBool();
            autoChk->setChecked(ac); scope->autoContrast = ac;
        }
        connect(autoChk, &QCheckBox::toggled, this, [this](bool on) { QSettings().setValue("scopeAutoContrast", on); scope->autoContrast = on; });
        {
            QSettings st;
            scopeTuneChk->setChecked(st.value("scopeClickTune", true).toBool());
            scope->clickToTune = scopeTuneChk->isChecked();
            scopeSnapChk->setChecked(st.value("scopeSnapKHz", true).toBool());
            scope->snap = scopeSnapChk->isChecked();
            const int stepHz = st.value("scopeSnapStep", 500).toInt() == 1000 ? 1000 : 500;
            scopeSnapStep->setCurrentIndex(stepHz == 1000 ? 1 : 0); scope->snapStepHz = stepHz;
            scopeSnapStep->setEnabled(scope->snap);
            scopeSsbOff->setValue(qBound(0, st.value("scopeSsbOffset", 1500).toInt(), 3000)); scope->ssbOffsetHz = scopeSsbOff->value();
            scopeTuneOpts->setEnabled(scope->clickToTune);
            scopeChk->setChecked(st.value("showScope", true).toBool());
        }
        connect(scopeTuneChk, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue("scopeClickTune", on); scope->clickToTune = on; scopeTuneOpts->setEnabled(on); scope->update();
        });
        connect(scopeSnapChk, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue("scopeSnapKHz", on); scope->snap = on; scopeSnapStep->setEnabled(on); scope->update();
        });
        connect(scopeSnapStep, &QComboBox::currentIndexChanged, this, [this](int) {
            const int hz = scopeSnapStep->currentData().toInt(); QSettings().setValue("scopeSnapStep", hz); scope->snapStepHz = hz; scope->update();
        });
        connect(scopeSsbOff, &QSpinBox::valueChanged, this, [this](int hz) { QSettings().setValue("scopeSsbOffset", hz); scope->ssbOffsetHz = hz; scope->update(); });
        connect(scopeChk, &QCheckBox::toggled, this, [this](bool on) { QSettings().setValue("showScope", on); showScope(on); });
        showScope(scopeChk->isChecked());

        // Status
        statusBox = new QGroupBox; auto *box = statusBox;
        auto *g = new QFormLayout(box);
        for (auto *l : {&txEnLbl, &pttLbl, &vfoLbl}) *l = new QLabel("…");
        QFont big = pttLbl->font(); big.setBold(true); big.setPointSize(big.pointSize() + 4);
        pttLbl->setFont(big); vfoLbl->setFont(big);
        vfoLbl->setToolTip("Arrow keys step VFO-A through flrig (only while the radio is receiving): "
                           "Left/Right \u00b1 5 kHz, Up/Down \u00b1 500 Hz. The dial itself moves: mode does not matter, "
                           "unlike a scope click. A widget that uses the arrows keeps them while it has focus.");
        g->addRow("TX keying:", txEnLbl);
        g->addRow("Radio PTT (flrig):", pttLbl);
        g->addRow("VFO:", vfoLbl);
        netLbl = new QLabel("…"); netLbl->setWordWrap(true);
        g->addRow("Network audio:", netLbl);
        txgLbl = new QLabel("…"); txgLbl->setWordWrap(true);
        g->addRow("TX gate:", txgLbl);
        statusForm = g;
        v->addWidget(box);

        auto *row2 = new QHBoxLayout;
        txBtn = new QPushButton("…");
        auto *helpBtn = new QPushButton("Help / About");
        settingsBtn = new QPushButton("Settings…");
        settingsBtn->setToolTip("Rig host, flrig, network audio / TX gate and band-scope settings.");
        catBtn = new QPushButton("CAT reset");
        catBtn->setToolTip("Power-cycle the FT-710 over its CAT serial port on the rig host, bypassing flrig (for a stuck rig, "
                           "as long as CAT still answers). flrig is stopped for it and started again. Refused while PTT is held.");
        logBtn = new QToolButton; logBtn->setText("Show log"); logBtn->setCheckable(true);
        row2->addWidget(txBtn, 1); row2->addWidget(logBtn); row2->addWidget(settingsBtn); row2->addWidget(catBtn); row2->addWidget(helpBtn);
        v->addLayout(row2);

        log = new QPlainTextEdit; log->setReadOnly(true); log->setVisible(false);
        log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        log->setMinimumHeight(220);
        v->addWidget(log);

        for (auto *b : findChildren<QAbstractButton *>()) b->setFocusPolicy(Qt::NoFocus);
        log->setFocusPolicy(Qt::NoFocus);

        connect(catBtn, &QPushButton::clicked, this, [this] { doCatReset(false, true); });
        catGuard.setSingleShot(true);
        connect(&catGuard, &QTimer::timeout, this, [this] {
            if (resetProc) { resetProc->disconnect(); resetProc->kill(); resetProc->deleteLater(); resetProc = nullptr; }
            catFinish(false, "no result in time" + QString(catViaLink ? " from the TX gate (the reset runs on in its own unit on the rig host; "
                                                                        "check the radio and flrig)" : ""));
        });
        connect(ptt, &QPushButton::pressed, this, [this] { pressPtt(); });
        connect(ptt, &QPushButton::released, this, [this] { stopTalk(); });
        connect(txBtn, &QPushButton::clicked, this, [this] { toggleTx(); });
        connect(logBtn, &QToolButton::toggled, this, [this](bool on) {
            log->setVisible(on); logBtn->setText(on ? "Hide log" : "Show log");
            if (on) poll();
            else if (!restoreSavedGeometry()) adjustSize();   // back to the remembered size
        });
        connect(helpBtn, &QPushButton::clicked, this, [this] { showHelp(); });
        connect(settingsBtn, &QPushButton::clicked, this, [this] { editSettings(); });

        qApp->installEventFilter(this);
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            stopTalk(true);
            if (tx->active()) tx->stop(true, true);   // the gate unkeys before it exits
            if (na->active() && !naCfg.keepOnExit) na->stop(true);   // leave nothing streaming behind
        });

        timer.setInterval(POLL_IDLE_MS);
        connect(&timer, &QTimer::timeout, this, [this] { poll(); adjustPollRate(); });
        // PTT debounce: only start talking if PTT is still held after PTT_MIN_HOLD_MS.
        holdTimer.setSingleShot(true); holdTimer.setInterval(PTT_MIN_HOLD_MS);
        connect(&holdTimer, &QTimer::timeout, this, [this] { beginTalk(); });
        timer.start();
        applyConfig();
        // App restarted while the network stream kept running (Settings: keep running): just monitor it.
        if (cfg.configured() && NetAudio::localRunning()) na->start(true);
        if (!cfg.configured()) QTimer::singleShot(300, this, [this] { editSettings(); });
    }

    ~Main() override {
        qApp->removeEventFilter(this);
        timer.stop();
        if (pollProc) { pollProc->disconnect(); pollProc->kill(); pollProc->waitForFinished(1000); }
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == netGainLbl && e->type() == QEvent::MouseButtonDblClick) {   // RX gain value: back to the default
            netGainApply.stop(); setGainSlider(NetAudioCfg().rxGain); na->setRxGain(NetAudioCfg().rxGain); return true;
        }
        if (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) {
            auto *k = static_cast<QKeyEvent *>(e);
            if (k->key() == Qt::Key_Space && isActiveWindow() && !QApplication::activeModalWidget()) {
                if (!k->isAutoRepeat()) {
                    if (e->type() == QEvent::KeyPress) pressPtt(); else stopTalk();
                }
                return true;
            }
            // Arrow-key tuning (auto-repeat allowed: presses during a round trip coalesce into it).
            // The keypad modifier (arrows with NumLock off) is fine; other modifiers are not.
            if (e->type() == QEvent::KeyPress && arrowModsOk(k->modifiers()) &&
                isActiveWindow() && !QApplication::activeModalWidget() &&
                !arrowsBlockedByFocus(QApplication::focusWidget())) {
                const TuneKey key = arrowTuneKey(k->key());
                if (key != TuneKey::None) { stepTune(key); return true; }
            }
        } else if (e->type() == QEvent::MouseButtonRelease && (talking || holdTimer.isActive()) && !QApplication::activeModalWidget()) {
            // Release anywhere (even dragged off the button) ends TX.
            QTimer::singleShot(0, this, [this] { if (!ptt->isDown()) stopTalk(); });
        }
        return QWidget::eventFilter(o, e);
    }
    void changeEvent(QEvent *e) override {
        if (e->type() == QEvent::ActivationChange && !isActiveWindow()) stopTalk();
        QWidget::changeEvent(e);
    }
    void closeEvent(QCloseEvent *e) override { stopTalk(true); geomSave.stop(); saveGeom(); QWidget::closeEvent(e); }
    void resizeEvent(QResizeEvent *e) override { QWidget::resizeEvent(e); if (geomReady) geomSave.start(); }
    void moveEvent(QMoveEvent *e) override { QWidget::moveEvent(e); if (geomReady) geomSave.start(); }
public:
    // Window size memory (QSettings file, see Settings > General), kept separately for scope shown / hidden.
    // Not saved while the log pane is open (that size is temporary). On Wayland only the size comes back;
    // the compositor places the window.
    bool restoreSavedGeometry() {
        const QByteArray g = QSettings().value(geomKey(scopeChk->isChecked())).toByteArray();
        return !g.isEmpty() && restoreGeometry(g);
    }
    void enableGeometrySaving() { geomReady = true; }
private:
    QTimer geomSave; bool geomReady = false;
    static QString geomKey(bool scopeShown) { return scopeShown ? "geometry/scopeShown" : "geometry/scopeHidden"; }
    void saveGeom(int scopeState = -1) {
        if (!geomReady || !isVisible() || log->isVisible()) return;
        QSettings().setValue(geomKey(scopeState < 0 ? scopeChk->isChecked() : scopeState == 1), saveGeometry());
    }

private:
    QPushButton *ptt, *txBtn, *catBtn;
    QProcess *resetProc = nullptr; QTimer catGuard; bool catBusy = false, catViaLink = false, catDry = false; QStringList catLog; QByteArray catBuf;
    // --- band scope ---
    QCheckBox *scopeChk, *scopeTuneChk, *scopeSnapChk; QWidget *scopeTuneOpts; QComboBox *scopeSnapStep; QSpinBox *scopeSsbOff;
    ScopeWidget *scope;
    bool radioRx = false;          // flrig last reported PTT=0 (tuning by click is only allowed then)
    double vfoHz = 0;              // VFO shown in the status row: the confirmed poll value or the last commanded target
    // Stale-poll guard: while a dial change (tuneTargetHz) is not yet confirmed, parse() ignores
    // polls that started before the last set_vfo finished (generation <= tuneDoneGen) unless they
    // already report the commanded value. pollGen counts poll() starts; tuneSentHz is what the
    // ssh call in flight carries (0 = none).
    double tuneTargetHz = 0, tuneSentHz = 0;
    int pollGen = 0, tuneDoneGen = 0;
    QProcess *tuneProc = nullptr;
    // scope span/level controls
    QWidget *scopeCtl; QComboBox *spanBox; QSlider *lvlSlider; QLabel *lvlLbl; QCheckBox *autoChk;
    QTimer lvlApply, lvlPoll; QElapsedTimer lvlTouched, spanTouched; bool lvlSync = false;
    QProcess *catProc = nullptr; QString catPending; int lvlRetries = 0;
    // The only CAT strings this app ever sends: "SS05<0-9>0000;" (span) and "SS04<+|-><00.0-30.0>;" (level)
    // and the queries "SS04;". Anything else is refused here.
    static bool scopeCmdOk(const QString &c) {
        static const QRegularExpression ok("^(SS05[0-9]0000;|SS04[+-](30\\.0|[0-2][0-9]\\.[05]);|SS04;)$");
        return ok.match(c).hasMatch();
    }
    static QString levelCmd(int half) {   // half-dB units -60..60
        half = qBound(-60, half, 60);
        return QString("SS04%1%2;").arg(half < 0 ? '-' : '+').arg(std::abs(half) / 2.0, 4, 'f', 1, QChar('0'));
    }
    static QString catXml(const QString &cmd) {   // flrig rig.cat_string wants each byte as "xHH"
        QString hx;
        for (QChar ch : cmd) hx += "x" + QString("%1").arg(ch.unicode() & 0xFF, 2, 16, QChar('0')).toUpper();
        return "<?xml version=\"1.0\"?><methodCall><methodName>rig.cat_string</methodName><params><param><value>"
               + hx + "</value></param></params></methodCall>";
    }
    static QString catReply(const QString &xml) {   // " 53 53 30 34 ..." -> "SS04..."
        static const QRegularExpression v("<value>(?:<string>)?([^<]*)");
        auto m = v.match(xml); if (!m.hasMatch()) return {};
        QString out;
        for (const QString &t : m.captured(1).split(' ', Qt::SkipEmptyParts)) { bool ok; int b = t.toInt(&ok, 16); if (!ok) return {}; out += QChar(b); }
        return out;
    }
    // One CAT exchange via ssh + curl -> flrig on the rig host (flrig owns the serial port and serialises access).
    void scopeCat(const QString &cmd) {
        if (!scopeCmdOk(cmd)) { QTextStream(stderr) << "refused CAT " << cmd << Qt::endl; return; }
        if (catProc) { catPending = cmd; return; }   // send the newest value when the current one finishes
        catProc = new QProcess(this);
        connect(catProc, &QProcess::finished, this, [this, cmd](int code, QProcess::ExitStatus st) {
            const QString out = QString::fromUtf8(catProc->readAllStandardOutput());
            catProc->deleteLater(); catProc = nullptr;
            if (st == QProcess::NormalExit && code == 0 && cmd == "SS04;") gotLevel(catReply(out));
            if (!catPending.isEmpty()) { const QString c = catPending; catPending.clear(); scopeCat(c); }
            else if (cmd.startsWith("SS04") && cmd != "SS04;") QTimer::singleShot(800, this, [this] { queryLevel(); });
        });
        connect(catProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && catProc) { catProc->deleteLater(); catProc = nullptr; }
        });
        catProc->start("ssh", sshArgs("curl -s -m 3 -H 'Content-Type: text/xml' --data-binary @- " + shq(cfg.flrigUrl())));
        catProc->write(catXml(cmd).toUtf8()); catProc->closeWriteChannel();
        QTimer::singleShot(8000, catProc, [p = catProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void queryLevel() {
        if (!scope->isVisible() || (lvlTouched.isValid() && lvlTouched.elapsed() < 3000)) return;
        scopeCat("SS04;");
    }
    void gotLevel(const QString &r) {
        // flrig can hand back a reply meant for its own polling; accept only a well-formed SS04 answer
        static const QRegularExpression re("SS04([+-]\\d\\d\\.\\d);");
        auto m = re.match(r);
        if (!m.hasMatch()) { if (++lvlRetries <= 3) QTimer::singleShot(700, this, [this] { queryLevel(); }); return; }
        lvlRetries = 0;
        if (lvlTouched.isValid() && lvlTouched.elapsed() < 3000) return;
        const int half = int(std::lround(m.captured(1).toDouble() * 2));
        lvlSync = true; lvlSlider->setValue(half); lvlSync = false;
        lvlLbl->setText(QString("%1 dB").arg(half / 2.0, 0, 'f', 1));
        lvlSlider->setEnabled(true);
    }
    void showScope(bool on) {
        if (geomReady) { geomSave.stop(); saveGeom(on ? 0 : 1); }   // remember the size for the state we're leaving
        const bool remembered = geomReady && !QSettings().value(geomKey(on)).toByteArray().isEmpty();
        scope->setVisible(on); scopeTuneChk->setVisible(on); scopeTuneOpts->setVisible(on); scopeCtl->setVisible(on);
        if (remembered) QTimer::singleShot(0, this, [this] { restoreSavedGeometry(); });
        if (on) { queryLevel(); lvlPoll.start(); } else lvlPoll.stop();
        if (on) {
            if (cfg.effScopeHost().isEmpty()) scope->stop(); else scope->start(cfg.effScopeHost(), quint16(cfg.scopePort));
            if (!remembered && width() < 760) resize(760, qMax(height(), 820));
        } else {
            scope->stop();
            if (!remembered) QTimer::singleShot(0, this, [this] { adjustSize(); });
        }
    }
    // Dial changes (scope click or arrow-key step) -> flrig rig.set_vfo on the rig host (frequency
    // only; never PTT, never menu/EX settings). The VFO label follows the commanded target right
    // away; the newest poll-generation guard in parse() keeps stale reads from undoing it.
    // While a set_vfo is in flight the target is only remembered (coalesced): the finished handler
    // sends one follow-up carrying the newest target, so a fast burst of keys or clicks converges
    // to its last frequency with at most one ssh call per round trip. Returns false if refused.
    // The guard for every dial change (click, arrow key, coalesced re-send): RX only, never while
    // talking, range only, and hz == the shown dial skips a pointless set_vfo. A static pure
    // function so --tune-keys-selftest can pin it down (it is not security-relevant: tuneTo is the
    // only writer, and set_vfo carries a frequency and nothing else).
    static bool tuneAllowed(double hz, double curHz, bool rx, bool talking) {
        return rx && !talking && hz >= kMinDialHz && hz <= kMaxDialHz && hz != curHz;
    }
    bool tuneTo(double hz) {
        if (!tuneAllowed(hz, vfoHz, radioRx, talking)) return false;
        vfoHz = hz;
        vfoLbl->setText(QString::number(hz / 1e6, 'f', 6) + " MHz");
        tuneTargetHz = hz;
        if (!tuneProc) sendTune(hz);   // busy: the finished handler picks tuneTargetHz up
        return true;
    }
    void sendTune(double hz) {
        tuneSentHz = hz;
        tuneProc = new QProcess(this);
        connect(tuneProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            const bool ok = st == QProcess::NormalExit && code == 0;
            tuneProc->deleteLater(); tuneProc = nullptr;
            if (!ok) QTextStream(stderr) << "rig.set_vfo failed" << Qt::endl;
            // A failed send leaves tuneTargetHz set: the authoritative poll below will restore
            // the VFO the radio actually has.
            if (ok && tuneTargetHz > 0 && tuneTargetHz != tuneSentHz) {
                sendTune(tuneTargetHz);   // coalesced follow-up for the newest target
                return;
            }
            tuneSentHz = 0;
            tuneDoneGen = pollGen;   // polls started from now on see the effect of the last send
            poll();
        });
        connect(tuneProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && tuneProc) {
                tuneProc->deleteLater(); tuneProc = nullptr; tuneSentHz = 0;
                tuneDoneGen = pollGen;
            }
        });
        const QString body = QString("<?xml version=\"1.0\"?><methodCall><methodName>rig.set_vfo</methodName><params><param>"
                                     "<value><double>%1</double></value></param></params></methodCall>").arg(hz, 0, 'f', 0);
        tuneProc->start("ssh", sshArgs("curl -s -m 3 -H 'Content-Type: text/xml' --data-binary @- " + shq(cfg.flrigUrl()) + " >/dev/null"));
        tuneProc->write(body.toUtf8()); tuneProc->closeWriteChannel();
        QTimer::singleShot(8000, tuneProc, [p = tuneProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    // Arrow-key tuning: step the dial from the VFO last shown. Moves the dial itself
    // (Left/Right ±5 kHz, Up/Down ±500 Hz), so — unlike a scope click — mode does not matter.
    // Every accepted press moves the display; presses during a round trip coalesce into it.
    void stepTune(TuneKey key) {
        if (!cfg.configured()) return;
        if (talking || holdTimer.isActive()) { note("Arrow keys do not tune while transmitting"); return; }
        if (!radioRx) { note("Arrow keys tune only while the radio is receiving"); return; }
        if (vfoHz <= 0) { note("Arrow keys tune once flrig reports the VFO"); return; }
        const double hz = stepDialHz(vfoHz, keyStepHz(key));
        if (hz > 0) tuneTo(hz);
    }
    // Left/Right coarse, Up/Down fine; any other key is None (modifier keys are filtered by arrowModsOk).
    static TuneKey arrowTuneKey(int key) {
        switch (key) {
            case Qt::Key_Left:  return TuneKey::Left;
            case Qt::Key_Right: return TuneKey::Right;
            case Qt::Key_Up:    return TuneKey::Up;
            case Qt::Key_Down:  return TuneKey::Down;
            default:            return TuneKey::None;
        }
    }
    // No modifiers, or only the keypad modifier (arrow keys on the numeric keypad, NumLock off).
    static bool arrowModsOk(Qt::KeyboardModifiers m) { return (m & ~Qt::KeypadModifier) == Qt::NoModifier; }
    // The arrows tune unless the focus widget uses them for editing or scrolling (sliders, spin
    // boxes, combo boxes, line edits, lists, the log): those keep their normal keys. Buttons and
    // checkboxes do not use the arrows, so they tune even when a button has the focus.
    static bool arrowsBlockedByFocus(const QWidget *w) {
        return w && (qobject_cast<const QAbstractSlider *>(w) || qobject_cast<const QAbstractSpinBox *>(w) ||
                     qobject_cast<const QComboBox *>(w) || qobject_cast<const QLineEdit *>(w) ||
                     qobject_cast<const QAbstractScrollArea *>(w) || qobject_cast<const QAbstractItemView *>(w));
    }
public:
    // --tune-keys-selftest (ctest tune_keys): the static key-routing logic of the arrow keys —
    // key map, modifier mask (keypad arrows) and the focus rules — with no rig, no settings, no network.
    static int tuneKeysSelfTest() {
        int fails = 0, n = 0;
        QTextStream o(stdout);
        auto check = [&](bool ok, const QString &what) { ++n; if (!ok) ++fails; o << (ok ? "PASS " : "FAIL ") << what << "\n"; };
        using K = TuneKey;
        check(arrowTuneKey(Qt::Key_Left) == K::Left && keyStepHz(K::Left) == -5000, "Left: -5 kHz");
        check(arrowTuneKey(Qt::Key_Right) == K::Right && keyStepHz(K::Right) == 5000, "Right: +5 kHz");
        check(arrowTuneKey(Qt::Key_Up) == K::Up && keyStepHz(K::Up) == 500, "Up: +500 Hz");
        check(arrowTuneKey(Qt::Key_Down) == K::Down && keyStepHz(K::Down) == -500, "Down: -500 Hz");
        check(arrowTuneKey(Qt::Key_Space) == K::None && arrowTuneKey(Qt::Key_5) == K::None, "other keys: None (PTT, typing)");
        check(arrowModsOk(Qt::NoModifier), "no modifiers: tune");
        check(arrowModsOk(Qt::KeypadModifier), "keypad arrows (NumLock off): tune");
        check(!arrowModsOk(Qt::ShiftModifier) && !arrowModsOk(Qt::ControlModifier) &&
              !arrowModsOk(Qt::AltModifier), "Shift/Ctrl/Alt: not ours, widgets/desktop keep them");
        check(!arrowModsOk(Qt::KeypadModifier | Qt::ShiftModifier), "keypad + Shift: not ours");
        QSlider slider; QSpinBox spin; QDoubleSpinBox dspin; QComboBox combo; QLineEdit edit;
        QPlainTextEdit text; QTableWidget table; QCheckBox box; QPushButton button; QWidget plain;
        check(!arrowsBlockedByFocus(nullptr), "focus on the window itself: tune");
        check(arrowsBlockedByFocus(&slider), "slider focused: arrows scroll it");
        check(arrowsBlockedByFocus(&spin) && arrowsBlockedByFocus(&dspin), "spin box focused: arrows step it");
        check(arrowsBlockedByFocus(&combo), "combo box focused: arrows navigate it");
        check(arrowsBlockedByFocus(&edit), "line edit focused: arrows edit");
        check(arrowsBlockedByFocus(&text), "log focused: arrows scroll it");
        check(arrowsBlockedByFocus(&table), "item view focused: arrows navigate it");
        check(!arrowsBlockedByFocus(&box) && !arrowsBlockedByFocus(&button), "button/checkbox focused: still tunes");
        check(!arrowsBlockedByFocus(&plain), "plain widget (scope) focused: tune");
        check(tuneAllowed(14079000, 14074000, true, false), "tune guard: RX, in range, new frequency: allowed");
        check(!tuneAllowed(14079000, 14079000, true, false), "tune guard: hz equals the shown dial: no redundant set_vfo");
        check(!tuneAllowed(14079000, 14074000, false, false), "tune guard: radio not receiving (flrig PTT=1): refused");
        check(!tuneAllowed(14079000, 14074000, true, true), "tune guard: we are talking: refused");
        check(!tuneAllowed(20000, 14074000, true, false) && !tuneAllowed(80e6, 14074000, true, false),
              "tune guard: outside 30 kHz .. 75 MHz: refused");
        o << (fails ? QString("\n%1 of %2 FAILED\n").arg(fails).arg(n) : QString("\nall %1 passed\n").arg(n)) << Qt::flush;
        return fails ? 1 : 0;
    }
private:
    QToolButton *logBtn; QPushButton *settingsBtn; QGroupBox *statusBox;
    QLabel *statusLbl, *txEnLbl, *pttLbl, *vfoLbl;
    QPlainTextEdit *log;
    QTimer timer;
    QProcess *pollProc = nullptr;
    bool talking = false;          // PTT sent to the TX gate and not released yet
    static constexpr int POLL_IDLE_MS = 3000, POLL_TALK_MS = 500, PTT_MIN_HOLD_MS = 150;
    QTimer holdTimer;
    QElapsedTimer fastPollSince;   // keep fast polling briefly after release so the readout shows the unkey
    QString txState = "unknown";

    // --- TX level (applied by the TX gate on the rig host) ---
    QSlider *txSlider; QLabel *txLbl; QToolButton *txResetBtn;
    QTimer txApply;
    QProcess *txProc = nullptr;
    int txPending = -1;          // value queued while an ssh write is in flight
    bool txSync = false;
    QElapsedTimer txTouched;

    static double pctToGain(int pct) { double v = qBound(0, pct, 150) / 100.0; return v * v * v; }
    static int gainToPct(double g) { return int(std::lround(std::cbrt(qMax(0.0, g)) * 100.0)); }
    void setTxEnabled(bool on) {
        txSlider->setEnabled(on); txResetBtn->setEnabled(on);
        const QString tip = on ? "Level of your microphone audio the TX gate plays into the FT-710's USB input. "
                                 "Lower it if you're driving the radio too hard; 100% = unchanged. Saved on the rig host "
                                 "(tx_gain in the rig config dir), so it survives restarts. "
                                 "Does not key the radio or change tx_enabled."
                               : "The rig host isn't reachable right now.";
        txSlider->setToolTip(tip); txLbl->setToolTip(tip); txResetBtn->setToolTip(tip);
        if (!on) txLbl->setText("—");
    }
    void applyTx(int pct) {
        pct = qBound(0, pct, 150);
        QSettings().setValue("txVolume", pct);
        if (txProc) { txPending = pct; return; }
        txProc = new QProcess(this);
        connect(txProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            if (st != QProcess::NormalExit || code != 0) QTextStream(stderr) << "tx_gain write failed" << Qt::endl;
            txProc->deleteLater(); txProc = nullptr;
            if (txPending >= 0) { int p = txPending; txPending = -1; applyTx(p); }
        });
        connect(txProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && txProc) { txProc->deleteLater(); txProc = nullptr; }
        });
        // Atomic replace, so the gate never reads a half-written file.
        const QString g = QString::number(pctToGain(pct), 'f', 6);
        txProc->start("ssh", sshArgs(QString("f=%1; echo %2 > \"$f.tmp\" && mv -f \"$f.tmp\" \"$f\"").arg(cfg.remoteFile("tx_gain")).arg(g)));
        QTimer::singleShot(8000, txProc, [p = txProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void parseTx(const QString &val) {   // file contents, "none" if absent, empty if the poll failed
        const QString v = val.trimmed();
        if (v.isEmpty()) { setTxEnabled(false); return; }
        setTxEnabled(true);
        bool ok = false; double g = v.toDouble(&ok);
        const int pct = (v == "none" || !ok) ? 100 : qBound(0, gainToPct(g), 150);   // gate default is 1.0
        if (txProc || txApply.isActive()) return;
        if (txTouched.isValid() && txTouched.elapsed() < 2500) return;
        if (txSlider->isSliderDown()) return;
        txSync = true;
        txSlider->setValue(pct);
        txLbl->setText(QString("%1%").arg(pct));
        txSync = false;
        QSettings st;
        if (st.value("txVolume", -1).toInt() != pct) st.setValue("txVolume", pct);
    }
public:
    void setTxForTest(int pct) { txSlider->setValue(pct); }
    int txForTest() const { return txSlider->value(); }
    bool txEnabledForTest() const { return txSlider->isEnabled(); }
private:
    // --- network audio ---
    NetAudio *na; QWidget *netRow; QProgressBar *netLevel; QSlider *netVol; QLabel *netVolLbl, *netLbl;
    QToolButton *netMuteBtn; QPushButton *netStartBtn; QFormLayout *statusForm = nullptr; QTimer netVolApply;
    QSlider *netGain; QLabel *netGainLbl; QTimer netGainApply; bool gainSync = false;
    static QString fmtGainDb(double db) { return QString("%1%2 dB").arg(db >= 0 ? "+" : "").arg(db, 0, 'f', 1); }
    // Show a gain without re-saving it (the stored 3.0 stays exactly 3.0 unless the user moves the slider).
    void setGainSlider(double g) {
        gainSync = true; netGain->setValue(int(std::lround(NetAudioCfg::gainDb(NetAudioCfg::clampGain(g)) * 10))); gainSync = false;
        netGainLbl->setText(fmtGainDb(NetAudioCfg::gainDb(NetAudioCfg::clampGain(g))));
    }

    // --- TX (TX gate) ---
    TxLink *tx; QWidget *micRow; QProgressBar *micBar; QLabel *micDev, *txgLbl; QTimer micUi; AudioNodeNames nodeNames;
    bool talkNet = false;          // the current over is keyed through the TX gate
    QString gateSvc;               // last polled state of the gate unit
    QString noteText; QElapsedTimer noteAge;
    void note(const QString &t) { noteText = t; noteAge.start(); statusLbl->setText(t); }
    void updateMicUi() {
        if (!tx->active()) { micBar->setValue(0); micBar->setFormat("—"); return; }
        const double db = tx->micDb();
        if (tx->micUp() && db > -119) {
            micBar->setValue(int(qBound(0.0, db + 60, 60.0)));
            micBar->setFormat(QString("%1 dBFS").arg(db, 0, 'f', 0));
            micBar->setStyleSheet(QString("QProgressBar::chunk { background:%1; }").arg(db > -3 ? "#c00" : talking && talkNet ? "#c60" : "#2a2"));
        } else {
            micBar->setValue(0); micBar->setStyleSheet("");
            micBar->setFormat(tx->micUp() ? QString("no mic signal") : "not capturing — PTT blocked");
        }
        micBar->setToolTip(tx->micProblem());
    }
    void updateTxUi() {
        if (naCfg.micSource.isEmpty()) { micDev->setText("default input"); micDev->setToolTip("Default input (follows your desktop sound settings)"); }
        else { micDev->setText(nodeNames.label(naCfg.micSource)); micDev->setToolTip("PipeWire source: " + naCfg.micSource); }
        if (tx->active()) micUi.start(); else { micUi.stop(); updateMicUi(); }
        if (tx->active()) colored(txgLbl, tx->statusText(), tx->gateKeyed() ? "#c00" : tx->ready() ? "green" : "#c60");
        else colored(txgLbl, "not connected (Start starts it)" + (gateSvc.isEmpty() ? QString() : " · service " + gateSvc), "gray");
        if (tx->ready()) showPtt(tx->rigPtt());
        if (talking && talkNet) {
            ptt->setText(tx->gateKeyed() ? "TALKING" : "TALKING?\n(" + (tx->lastEvent().isEmpty() ? QString("gate not keyed yet") : tx->lastEvent()) + ")");
        }
        updateUi();
    }
    void showPtt(const QString &p) {
        radioRx = (p == "0");
        if (p.isEmpty() || p == "?") colored(pttLbl, "flrig not answering", "gray");
        else if (p == "1") { pttLbl->setText("  TX  "); pttLbl->setStyleSheet("color:white; background:#c00;"); }
        else colored(pttLbl, "RX", "green");
    }
    void updateNetUi() {
        const bool act = na->active();
        netStartBtn->setText(act || tx->active() ? "Stop" : "Start");
        QString st = na->statusText();
        if (act && naCfg.listenMute) st += " · Listen is off (not playing on this computer's speakers)";
        colored(netLbl, st, !act ? "gray" : na->linkUp() ? "green" : "#c60");
        const double db = na->levelDb();
        if (act && db > -119) {
            netLevel->setValue(int(qBound(0.0, db + 60, 60.0)));
            netLevel->setFormat(QString("%1 dBFS").arg(db, 0, 'f', 0));
        } else { netLevel->setValue(0); netLevel->setFormat("—"); }
        netLevel->setStyleSheet(act && na->linkUp() ? "QProgressBar::chunk { background:#2a2; }" : "QProgressBar::chunk { background:#999; }");
    }
public:
    NetAudio *netAudio() { return na; }
private:
    // Press: arm the debounce timer; a release within PTT_MIN_HOLD_MS cancels without keying.
    void pressPtt() {
        if (talking || holdTimer.isActive()) return;
        if (catBusy) { ptt->setDown(false); note("Not keying — a CAT reset is running"); return; }
        if (!tx->ready()) { ptt->setDown(false); note("TX link not ready — " + (tx->active() ? tx->statusText() : QString("press Start first"))); return; }
        if (txState != "on") { ptt->setDown(false); note("TX keying is disabled (Enable TX keying first)"); return; }
        if (!tx->micUp()) { ptt->setDown(false); note("Not keying — " + tx->micProblem()); return; }
        ptt->setStyleSheet("background:#e90; color:black;");
        ptt->setText("hold…");
        holdTimer.start();
    }
    void beginTalk() {
        holdTimer.stop();
        if (talking) return;
        if (!tx->setPtt(true)) { resetPttButton(); note(tx->ready() && !tx->micUp() ? "Not keying — " + tx->micProblem() : QString("TX link not ready — not keyed")); return; }
        talking = true; talkNet = true;
        ptt->setStyleSheet("background:#c00; color:white;");
        ptt->setText("TALKING?\n(waiting for the gate)");
        updateTxUi(); adjustPollRate(); poll();
    }
    void stopTalk(bool force = false) {
        Q_UNUSED(force);
        if (holdTimer.isActive()) { holdTimer.stop(); resetPttButton(); return; }   // short tap: never keyed
        if (talking) {   // the gate drains, hangs and unkeys (and unkeys by itself if this link is lost)
            talking = false; talkNet = false;
            tx->setPtt(false);
            fastPollSince.start(); adjustPollRate();
        }
        resetPttButton();
    }
    void resetPttButton() {
        ptt->setDown(false);
        ptt->setStyleSheet("");
        ptt->setText("PTT\n(hold, or hold Space)");
    }
    // Poll the status (incl. flrig PTT) every 0.5 s while talking and for 4 s after release, else 3 s.
    void adjustPollRate() {
        bool fast = talking || holdTimer.isActive() || (fastPollSince.isValid() && fastPollSince.elapsed() < 4000);
        int want = fast ? POLL_TALK_MS : POLL_IDLE_MS;
        if (timer.interval() != want) timer.start(want);
    }

public:
    // Start (was Connect): start the network receive audio and the TX gate link (PTT and the mic go to the gate).
    void doConnect() {
        if (!cfg.configured()) {
            QMessageBox::information(this, "Start", "Set the rig host in Settings first.");
            editSettings(); return;
        }
        if (!na->active()) na->start();
        if (!tx->active()) tx->start();
        note("Connected: RX " + naCfg.modeName() + ", TX via the TX gate");
        updateTxUi();
    }

    // Hidden test hook (--txgate-gui-test HOST PORT TOKENFILE HOLD_MS): the real PTT button path
    // against a test gate (direct connection, 700 Hz test tone, settings not saved). Prints each step.
    void txGuiTest(const QString &host, quint16 port, const QByteArray &tok, int holdMs) {
        naCfg.mode = 2;                               // in memory only
        auto out = [this](const QString &m) {
            QTextStream(stdout) << "gui-test: " << m << " | button=" << ptt->text().replace('\n', ' ') << " enabled=" << ptt->isEnabled()
                                << " talking=" << talking << " | gate: " << txgLbl->text() << " | rigPtt: " << pttLbl->text().trimmed() << Qt::endl; };
        tx->startDirect(host, port, tok, true);
        updateTxUi();
        auto *wait = new QTimer(this); wait->setInterval(50);
        auto t0 = std::make_shared<QElapsedTimer>(); t0->start();
        connect(wait, &QTimer::timeout, this, [=, this] {
            if (!tx->ready() && t0->elapsed() < 10000) return;
            wait->stop(); wait->deleteLater();
            out("ready");
            txState = "on";                            // the test gate decides; the real tx_enabled is not touched
            pressPtt(); out("pressed");
            QTimer::singleShot(holdMs / 2, this, [=, this] { out("holding"); });
            QTimer::singleShot(holdMs, this, [=, this] {
                stopTalk(); out("released");
                QTimer::singleShot(1500, this, [=, this] {
                    out("after hang");
                    txState = "on"; pressPtt(); QTimer::singleShot(600, this, [=, this] {
                        out("second over held");
                        QEvent fo(QEvent::ActivationChange); changeEvent(&fo);   // focus loss must release
                        out("focus lost");
                        QTimer::singleShot(1500, qApp, [] { qApp->quit(); });
                    });
                });
            });
        });
        wait->start();
    }
private:
    // Disconnect: release PTT, stop the network stream and the TX gate (the gate unkeys before it exits).
    void doDisconnect() {
        stopTalk(true);
        if (na->active()) na->stop(true);
        if (tx->active()) { tx->stop(true, false); note("Stopped: TX gate stopping"); }
        updateTxUi(); updateNetUi();
    }

    // CAT reset: power-cycle the FT-710 over CAT on the rig host (flrig is stopped and restarted there).
    // Through the TX gate's control link when it is ready, else over ssh with the gate's own code
    // (ft710_txgate.py --cat-reset --in-unit), which also works when the gate can't start (stuck PTT).
    static constexpr const char *CAT_SSH = R"SH(
GATE="$HOME/ft710-txgate/ft710_txgate.py"
[ -r "$GATE" ] || { echo "result=fail TX gate code not found at ~/ft710-txgate/ft710_txgate.py (see README)"; exit 0; }
exec python3 -u "$GATE" --cat-reset --in-unit $DRY
)SH";
public:
    void doCatReset(bool dry, bool ask) {
        if (catBusy) return;
        if (!cfg.configured()) { QMessageBox::information(this, "CAT reset", "Set the rig host in Settings first."); return; }
        if (talking || tx->gateKeyed()) { note("CAT reset refused: PTT is active (release PTT first)"); return; }
        if (ask && QMessageBox::question(this, "CAT reset", "Power-cycle the FT-710 over CAT? flrig will be restarted.",
                                         QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        if (talking || tx->gateKeyed()) { note("CAT reset refused: PTT is active (release PTT first)"); return; }
        catBusy = true; catDry = dry; catBtn->setEnabled(false); catBtn->setText("CAT reset…");
        catLog.clear();
        catViaLink = tx->sendCatReset(dry);
        catSay(QString(dry ? "dry run" : "power-cycle") + " requested " + (catViaLink ? "via the TX gate link" : "over ssh (TX gate link not up)"));
        catGuard.start(dry ? 30000 : 120000);
        if (catViaLink) return;
        resetProc = new QProcess(this); catBuf.clear();
        connect(resetProc, &QProcess::readyReadStandardOutput, this, [this] {
            catBuf += resetProc->readAllStandardOutput();
            int nl;
            while ((nl = catBuf.indexOf('\n')) >= 0) {
                const QString l = QString::fromUtf8(catBuf.left(nl)).trimmed(); catBuf.remove(0, nl + 1);
                if (l.startsWith("step: ")) catSay(l.mid(6));
                else if (l.startsWith("result=")) { const QString r = l.mid(7); catFinish(r.startsWith("ok"), r.mid(r.indexOf(' ') + 1)); }
                else if (!l.isEmpty()) catSay(l);
            }
        });
        connect(resetProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus es) {
            QProcess *p = resetProc; resetProc = nullptr; if (p) p->deleteLater();
            if (catBusy) catFinish(false, es != QProcess::NormalExit || code == 255 ? cfg.rigName() + " unreachable (ssh)"
                                                                               : QString("ended without a result (exit %1)").arg(code));
        });
        resetProc->start("ssh", sshArgs("bash -s"));
        resetProc->write((QString("DRY=%1\n").arg(dry ? "--dry-run" : "") + CAT_SSH).toUtf8()); resetProc->closeWriteChannel();
    }
private:
    void catSay(const QString &t) {
        const QString line = QDateTime::currentDateTime().toString("HH:mm:ss") + " " + t;
        catLog << line; while (catLog.size() > 60) catLog.removeFirst();
        QTextStream(stderr) << "catreset: " << t << Qt::endl;
        if (catBusy) note("CAT reset: " + t);
        if (log->isVisible()) log->appendPlainText("CAT reset: " + line);
    }
    void catGateEvent(const QString &e) {
        if (!catBusy) { catSay("(gate) " + e); return; }
        if (e.startsWith("catreset-done ")) {
            const QString r = e.mid(14); catFinish(r.startsWith("ok"), r.mid(r.indexOf(' ') + 1)); return;
        }
        const QString t = e.startsWith("catreset: ") ? e.mid(10) : e;
        if (t.startsWith("refused")) { catFinish(false, t); return; }
        catSay(t);
    }
    void catFinish(bool ok, const QString &summary) {
        if (!catBusy) return;
        catGuard.stop();
        catSay(QString(ok ? "OK" : "FAILED") + ": " + summary);
        catBusy = false; catBtn->setEnabled(true); catBtn->setText("CAT reset");
        note(QString(catDry ? "CAT reset dry run " : "CAT reset ") + (ok ? "OK: " : "FAILED: ") + summary);
        fastPollSince.start(); adjustPollRate();
        if (!ok && !catDry) QMessageBox::warning(this, "CAT reset", "CAT reset failed:\n" + summary);
    }

    void toggleTx() {
        stopTalk();
        bool enable = txState != "on";
        if (enable && QMessageBox::question(this, "TX keying",
                "Enable TX keying? The PTT button / Space in this app will key the FT-710 through the TX gate.") != QMessageBox::Yes)
            return;
        bool ok;
        sshRun((enable ? "touch " : "rm -f ") + cfg.remoteFile("tx_enabled"), &ok);
        if (!ok) QMessageBox::warning(this, "TX keying", "Command failed on the rig host.");
        poll();
    }

    void updateTitle() {
        setWindowTitle(cfg.stationLabel.isEmpty() ? QString("FT-710 Remote") : "FT-710 Remote — " + cfg.stationLabel);
    }
    // Apply the current cfg without a restart: title, status box, scope connection, status poll.
    void applyConfig() {
        updateTitle();
        statusBox->setTitle(QString("Status (%1)").arg(cfg.rigName()));
        if (scopeChk->isChecked()) {
            scope->stop();
            if (cfg.configured() || !cfg.scopeHost.isEmpty()) scope->start(cfg.effScopeHost(), quint16(cfg.scopePort));
        }
        poll(); updateUi(); updateNetUi(); updateTxUi();
    }
    void editSettings() {
        stopTalk();
        QDialog d(this); d.setWindowTitle("Settings");
        auto *tabs = new QTabWidget;
        auto mk = [](const QString &v, const QString &ph, const QString &tip) {
            auto *e = new QLineEdit(v); e->setPlaceholderText(ph); e->setToolTip(tip); return e; };
        auto port = [](int v, const QString &tip) { auto *b = new QSpinBox; b->setRange(1, 65535); b->setValue(v); b->setToolTip(tip); return b; };
        // Rig host
        auto *w1 = new QWidget; auto *f1 = new QFormLayout(w1);
        auto *sshE = mk(cfg.sshHost, "e.g. rigpi, pi@192.168.1.20", "ssh destination of the computer with the FT-710, flrig, ft710-txgate, "
                        "the network-audio sender and ft710-scope (an ~/.ssh/config alias, host, or user@host). Needs key-based ssh login.");
        auto *flrigE = mk(cfg.flrigHost, "127.0.0.1", "flrig XML-RPC host as seen FROM the rig host (usually 127.0.0.1).");
        auto *flrigP = port(cfg.flrigPort, "flrig XML-RPC port (flrig default 12345).");
        auto *dirE = mk(cfg.rigCfgDir, "~/.config/radio-bridge", "Config dir on the rig host: tx_enabled (TX keying on/off), tx_gain (TX level) "
                        "and bridge.env (RB_SOURCE = the radio's USB audio node). ft710-txgate reads the same dir (TG_CONFIG_DIR); "
                        "the default name is historical.");
        f1->addRow("Rig host (ssh):", sshE); f1->addRow("flrig host:", flrigE); f1->addRow("flrig port:", flrigP);
        f1->addRow("Rig config dir:", dirE);
        auto *testBtn = new QPushButton("Test ssh"); auto *testLbl = new QLabel; testLbl->setWordWrap(true);
        f1->addRow(testBtn, testLbl);
        connect(testBtn, &QPushButton::clicked, &d, [sshE, testLbl] {
            Config t = cfg; t.sshHost = sshE->text().trimmed();
            static const QRegularExpression dest("^([A-Za-z0-9._-]+@)?[A-Za-z0-9_][A-Za-z0-9._-]*$");
            if (!dest.match(t.sshHost).hasMatch()) { testLbl->setText("Invalid ssh destination."); return; }
            testLbl->setText("Testing…"); QApplication::processEvents();
            const Config saved = cfg; cfg.sshHost = t.sshHost;
            bool ok; QByteArray out = sshRun("echo ok", &ok, 8000); cfg = saved;
            testLbl->setText(ok && out.trimmed() == "ok" ? "ssh OK" : "ssh failed (needs key-based login without a prompt)");
        });
        tabs->addTab(w1, "Rig host");
        // Scope
        auto *w3 = new QWidget; auto *f3 = new QFormLayout(w3);
        auto *shE = mk(cfg.scopeHost, "blank = rig host", "Host running ft710-scope (as reachable from this computer).");
        auto *spP = port(cfg.scopePort, "ft710-scope TCP port (default 7355).");
        f3->addRow("Scope streamer host:", shE); f3->addRow("Scope streamer port:", spP);
        tabs->addTab(w3, "Band scope");
        // Network audio
        auto *w5 = new QWidget; auto *f5 = new QFormLayout(w5);
        auto *naMode = new QComboBox; naMode->addItems({"ROC (FEC, recommended on LAN)", "RTP"});
        naMode->setCurrentIndex(naCfg.mode == 2 ? 1 : 0);
        naMode->setToolTip("How the rig host sends the radio's receive audio to this computer (PipeWire). Here it is the capture "
                           "device \"FT-710 RX (network)\" for WSJT-X, fldigi etc., and Listen plays it on the speakers.");
        auto *naComp = new QComboBox; naComp->addItems({"None (L16, LAN)", "Opus 24 kbit/s", "Opus 32 kbit/s", "Opus 48 kbit/s", "Opus 64 kbit/s"});
        static const int kb[] = {24, 32, 48, 64};
        naComp->setCurrentIndex(naCfg.compression == 1 ? 1 + int(std::find(kb, kb + 4, naCfg.opusKbps) - kb) % 4 : 0);
        naComp->setToolTip("Opus for VPN / slow links (RTP only; sent with GStreamer opusenc on the rig host). "
                           "ROC (roc-toolkit 0.4) is PCM only, so ROC always uses L16.");
        auto *naPort = new QSpinBox; naPort->setRange(1024, 65533); naPort->setValue(naCfg.port);
        naPort->setToolTip("UDP port on this computer. ROC uses this port and the next two (repair, control).");
        auto *naLat = new QSpinBox; naLat->setRange(20, 2000); naLat->setSingleStep(10); naLat->setSuffix(" ms"); naLat->setValue(naCfg.latencyMs);
        naLat->setToolTip("Receive buffer (end-to-end latency is roughly this plus 20-50 ms). 100-150 ms suits a LAN.");
        auto *naPkt = new QComboBox; for (int ms : {5, 10, 20, 40}) naPkt->addItem(QString("%1 ms").arg(ms), ms);
        naPkt->setCurrentIndex(qMax(0, naPkt->findData(naCfg.packetMs)));
        naPkt->setToolTip("RTP packet size. L16: 5 or 10 ms (MTU). Opus: 10-40 ms (20 ms usual). ROC chooses its own.");
        auto *naFec = new QComboBox; naFec->addItems({"rs8m", "ldpc", "disable"}); naFec->setCurrentText(naCfg.fec);
        naFec->setToolTip("ROC forward error correction (rs8m: Reed-Solomon, good for audio). Both ends use the same.");
        auto *naSrc = mk(naCfg.sourceNode, "blank = RB_SOURCE from bridge.env in the rig config dir",
                         "PipeWire capture node of the FT-710's USB audio on the rig host (pactl list short sources).");
        auto *naDest = mk(naCfg.destAddr, "blank = automatic (this computer as the rig host sees it)",
                          "IP address the rig host sends the stream to. Automatic uses the ssh connection's source address.");
        auto *naKeep = new QCheckBox("Keep streaming when FT-710 Remote closes"); naKeep->setChecked(naCfg.keepOnExit);
        f5->addRow("RX stream:", naMode); f5->addRow("Compression:", naComp); f5->addRow("Port:", naPort);
        f5->addRow("Buffer / latency:", naLat); f5->addRow("Packet size:", naPkt); f5->addRow("ROC FEC:", naFec);
        f5->addRow("Source node (rig host):", naSrc); f5->addRow("Send to (this computer):", naDest); f5->addRow("", naKeep);
        auto *naNote = new QLabel("Needs PipeWire with pipewire-module-roc (ROC) on both computers; Opus needs GStreamer "
                                  "(pipewire-gstreamer, opus plugins) on the rig host. Pick \"FT-710 RX (network)\" as the input in WSJT-X/fldigi.");
        naNote->setWordWrap(true); f5->addRow(naNote);
        // TX: your microphone -> ft710-txgate on the rig host
        auto *txMic = new QComboBox; txMic->setEditable(true); txMic->addItem("", QString());
        txMic->setItemText(0, "(default input)");
        {
            QProcess pc; pc.start("pactl", {"-f", "json", "list", "sources"}); pc.waitForFinished(3000);
            for (const QJsonValue &v : QJsonDocument::fromJson(pc.readAllStandardOutput()).array()) {
                const QJsonObject o = v.toObject(); const QString name = o["name"].toString();
                if (name.isEmpty() || name.endsWith(".monitor") || name.startsWith("ft710_")) continue;
                txMic->addItem(o["description"].toString().isEmpty() ? name : o["description"].toString() + "  [" + name + "]", name);
            }
        }
        if (!naCfg.micSource.isEmpty()) {
            int k = txMic->findData(naCfg.micSource);
            if (k < 0) { txMic->addItem(naCfg.micSource, naCfg.micSource); k = txMic->count() - 1; }
            txMic->setCurrentIndex(k);
        }
        txMic->setToolTip("Microphone (PipeWire source) sent to the radio. Default input follows your desktop setting.");
        auto *txPort = new QSpinBox; txPort->setRange(1024, 65535); txPort->setValue(naCfg.txPort);
        txPort->setToolTip("TCP (control) and UDP (audio) port of ft710-txgate on the rig host (TG_PORT there).");
        auto *txUnit = mk(naCfg.txUnit, "ft710-txgate", "systemd --user unit of the TX gate on the rig host.");
        f5->addRow("Microphone:", txMic); f5->addRow("TX gate port:", txPort); f5->addRow("TX gate service:", txUnit);
        auto syncNa = [naMode, naComp, naPkt, naFec] {
            const int m = naMode->currentIndex() + 1;   // 1 = ROC, 2 = RTP
            if (m == 1) naComp->setCurrentIndex(0);
            naComp->setEnabled(m == 2); naPkt->setEnabled(m == 2); naFec->setEnabled(m == 1);
        };
        connect(naMode, &QComboBox::currentIndexChanged, &d, syncNa);
        connect(naComp, &QComboBox::currentIndexChanged, &d, [naComp, naPkt] {
            const int ms = naPkt->currentData().toInt();
            if (naComp->currentIndex() > 0 && ms < 20) naPkt->setCurrentIndex(naPkt->findData(20));
            if (naComp->currentIndex() == 0 && ms > 10) naPkt->setCurrentIndex(naPkt->findData(10));
        });
        syncNa();
        tabs->addTab(w5, "Network audio / TX");
        // General
        auto *w4 = new QWidget; auto *f4 = new QFormLayout(w4);
        auto *lblE = mk(cfg.stationLabel, "e.g. your callsign", "Shown in the window title.");
        auto *hfE = mk(cfg.helpFile, "optional, e.g. ~/notes.md", "Your own Markdown notes, shown at the top of Help / About.");
        f4->addRow("Station label:", lblE); f4->addRow("Help notes file:", hfE);
        f4->addRow(new QLabel("Settings file: " + QSettings().fileName()));
        tabs->addTab(w4, "General");
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto *lay = new QVBoxLayout(&d); lay->addWidget(tabs); lay->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        Config n; NetAudioCfg nn;
        connect(bb, &QDialogButtonBox::accepted, &d, [&] {
            n = cfg;
            n.sshHost = sshE->text().trimmed(); n.flrigHost = flrigE->text().trimmed(); n.flrigPort = flrigP->value();
            n.rigCfgDir = dirE->text().trimmed();
            n.scopeHost = shE->text().trimmed(); n.scopePort = spP->value();
            n.stationLabel = lblE->text().trimmed(); n.helpFile = hfE->text().trimmed();
            nn = naCfg;
            nn.mode = naMode->currentIndex() + 1;
            nn.compression = naComp->currentIndex() > 0 ? 1 : 0;
            if (naComp->currentIndex() > 0) nn.opusKbps = kb[naComp->currentIndex() - 1];
            nn.port = naPort->value(); nn.latencyMs = naLat->value(); nn.packetMs = naPkt->currentData().toInt();
            nn.fec = naFec->currentText(); nn.sourceNode = naSrc->text().trimmed(); nn.destAddr = naDest->text().trimmed();
            nn.keepOnExit = naKeep->isChecked();
            nn.micSource = txMic->currentIndex() >= 0 && txMic->currentText() == txMic->itemText(txMic->currentIndex())
                               ? txMic->currentData().toString() : txMic->currentText().trimmed();
            nn.txPort = txPort->value(); nn.txUnit = txUnit->text().trimmed();
            QString err = n.validate();
            if (err.isEmpty()) err = nn.validate();
            if (!err.isEmpty()) { QMessageBox::warning(&d, "Settings", err); return; }
            d.accept();
        });
        d.resize(560, d.sizeHint().height());
        if (d.exec() != QDialog::Accepted) return;
        const bool naWasActive = na->active();
        const QString naBefore = naCfg.describe() + naCfg.sourceNode + naCfg.destAddr + QString::number(naCfg.port);
        auto txKey = [] { return cfg.sshHost + "|" + cfg.flrigUrl() + "|" + naCfg.micSource + "|" + naCfg.txUnit + "|"
                                 + QString::number(naCfg.txPort) + "|" + naCfg.describe(); };
        const QString txBefore = txKey();
        const bool txWasActive = tx->active();
        cfg = n; cfg.save();
        naCfg = nn; naCfg.save();
        if (txWasActive && txKey() != txBefore) { stopTalk(true); tx->stop(false, false); tx->start(); }
        // Network audio: restart it when its settings changed.
        if (naWasActive && naBefore != naCfg.describe() + naCfg.sourceNode + naCfg.destAddr + QString::number(naCfg.port)) na->start();
        applyConfig();
    }
private:
    void showHelp() {
        QString md;
        if (!cfg.helpFile.isEmpty()) {
            QString path = cfg.helpFile; if (path.startsWith("~/")) path = QDir::home().filePath(path.mid(2));
            QFile f(path);
            md = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) + "\n\n---\n" : path + " not found.\n\n---\n";
        }
        md += "**FT-710 Remote 1.0**: Start (in the RX network row) starts the network receive audio (ROC or RTP from the rig host; the input device "
              "\"FT-710 RX (network)\" for WSJT-X/fldigi, and Listen plays it here) and the link to the ft710-txgate service on "
              "the rig host. Hold PTT or Space to talk: this computer's microphone goes straight to the TX gate, which keys the "
              "radio through flrig only while you hold PTT (key, ~70 ms, then audio; 0.7 s hang after release; 120 s timeout) "
              "and unkeys by itself if the link, heartbeat or audio stops. Releasing, switching windows or closing the app always "
              "stops talking. \"Enable TX keying\" arms the gate (tx_enabled on the rig host); \"TX level\" is applied by the gate. "
              "Stop (or closing the app) stops the stream and the gate. "
              "**CAT reset** power-cycles the FT-710 over its CAT port on the rig host, bypassing flrig (flrig is stopped "
              "and started again; refused while PTT is held). "
              "**Band scope**: spectrum + waterfall from the FT-710 itself (MENU SCU-LAN10 = ON; streamed by "
              "the ft710-scope service on the rig host). The shaded band is the approximate passband, the red line "
              "the VFO. With \"Click to tune\" on, a single click sets VFO-A there via flrig (only while receiving). "
              "The arrow keys step the dial anywhere in the window: Left/Right \u00b1 5 kHz, Up/Down \u00b1 500 Hz "
              "(also only while receiving; a widget that uses the arrows keeps them while it has focus, "
              "buttons and checkboxes do not, so they tune even when one is focused). "
              "**Span** and **Level** set the radio's own scope span (1 kHz-1 MHz) and scope level (-30 to +30 dB, 0.5 dB "
              "steps) through flrig (CAT SS05/SS04, display settings only). With \"Auto contrast\" off the colours are fixed, "
              "so Level works like the radio's knob; turn it on to let the app rescale colours itself. "
              "**Settings…** holds the rig host, flrig, network audio / TX gate and scope settings.";
        QDialog d(this); d.setWindowTitle("Help / About"); d.resize(640, 520);
        auto *l = new QVBoxLayout(&d); auto *tb = new QTextBrowser; tb->setMarkdown(md); l->addWidget(tb);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close); l->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        d.exec();
    }

    void poll() {
        if (pollProc) return;   // previous poll still running
        if (!cfg.configured()) { setOffline(); return; }
        updateTxUi();
        const int gen = ++pollGen;
        pollProc = new QProcess(this);
        QStringList args = sshArgs(log->isVisible() ? "bash -s log" : "bash -s");
        connect(pollProc, &QProcess::finished, this, [this, gen](int code, QProcess::ExitStatus st) {
            QString out = QString::fromUtf8(pollProc->readAllStandardOutput());
            pollProc->deleteLater(); pollProc = nullptr;
            if (st != QProcess::NormalExit || code == 255) { setOffline(); return; }
            parse(out, gen);
        });
        connect(pollProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (pollProc && pollProc->state() == QProcess::NotRunning) { pollProc->deleteLater(); pollProc = nullptr; setOffline(); }
        });
        pollProc->start("ssh", args);
        pollProc->write(pollScript());
        pollProc->closeWriteChannel();
        QTimer::singleShot(8000, pollProc, [p = pollProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
        updateUi();
    }

    static void colored(QLabel *l, const QString &t, const char *color) {
        l->setText(t); l->setStyleSheet(QString("color:%1;").arg(color));
    }
    void setOffline() {
        for (auto *l : {txEnLbl, pttLbl, vfoLbl}) colored(l, cfg.configured() ? cfg.rigName() + " unreachable" : "not configured (Settings…)", "gray");
        txState = "unknown"; radioRx = false; vfoHz = 0; tuneTargetHz = tuneSentHz = 0; setTxEnabled(false); updateUi();
    }
    void parse(const QString &out, int gen) {
        QString logText;
        QMap<QString, QString> kv;
        int li = out.indexOf("---LOG---\n");
        QString head = li >= 0 ? out.left(li) : out;
        if (li >= 0) logText = out.mid(li + 10);
        for (const QString &line : head.split('\n')) {
            int eq = line.indexOf('=');
            if (eq > 0) kv[line.left(eq)] = line.mid(eq + 1);
        }
        gateSvc = kv.value("txgate");

        txState = kv["tx"];
        if (txState == "on") colored(txEnLbl, "ENABLED — PTT here keys the radio (TX gate)", "#c00");
        else colored(txEnLbl, "disabled (receive-only)", "green");

        if (!tx->ready()) showPtt(kv["ptt"].trimmed());   // with the gate link up, its 0.25 s flrig readout is used instead

        bool okv; double hz = kv["vfo"].trimmed().toDouble(&okv);
        parseTx(kv["txgain"]);
        // Stale-poll guard (see tuneTo/sendTune): while a dial change is unconfirmed, a poll that
        // started before the last set_vfo finished still shows the pre-tune frequency — keep the
        // commanded value instead of flickering back. Confirmed by a later-generation poll, or at
        // once when a poll already reports the commanded frequency (then the radio agrees: done).
        if (tuneTargetHz > 0 && okv && hz > 0 &&
            (tuneProc || (gen <= tuneDoneGen && std::fabs(hz - tuneTargetHz) >= 0.5)))
            hz = vfoHz;
        else if (tuneTargetHz > 0 && okv && hz > 0)
            tuneTargetHz = 0;   // confirmed
        vfoHz = okv && hz > 0 ? hz : 0;
        vfoLbl->setText(okv && hz > 0 ? QString::number(hz / 1e6, 'f', 6) + " MHz" : "?");

        if (log->isVisible() && li >= 0)
            log->setPlainText((catLog.isEmpty() ? QString() : "--- CAT reset (this app) ---\n" + catLog.join('\n') + "\n--- TX gate log ---\n") + logText.trimmed());
        updateUi();
    }
    void updateUi() {
        if (!(noteAge.isValid() && noteAge.elapsed() < 8000))
            statusLbl->setText(tx->active() ? "TX gate: " + tx->statusText() : "Not connected — Start (RX network row) starts the network audio and the TX gate");
        netStartBtn->setText(na->active() || tx->active() ? "Stop" : "Start");
        ptt->setEnabled(tx->ready() || talking);
        txBtn->setEnabled(txState == "on" || txState == "off");
        txBtn->setText(txState == "on" ? "Disable TX keying (go receive-only)"
                     : txState == "off" ? "Enable TX keying" : "TX keying: unknown");
    }
};

// First run of this build: copy settings from an older install that stored them under another
// organization directory (~/.config/<org>/ft710-remote.conf). Keys already set here win.
static void migrateLegacySettings() {
    QSettings now;
    if (now.value("settings/migrated", false).toBool()) return;
    const QString mine = QFileInfo(now.fileName()).absoluteFilePath();
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QFileInfo best;
    for (const QFileInfo &dir : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QFileInfo f(QDir(dir.absoluteFilePath()).filePath("ft710-remote.conf"));
        if (f.isFile() && f.absoluteFilePath() != mine && (!best.exists() || f.lastModified() > best.lastModified())) best = f;
    }
    if (best.exists()) {
        QSettings old(best.absoluteFilePath(), QSettings::IniFormat);
        int n = 0;
        for (const QString &k : old.allKeys()) if (!now.contains(k)) { now.setValue(k, old.value(k)); ++n; }
        now.setValue("settings/migratedFrom", best.absoluteFilePath());
        QTextStream(stderr) << "settings: migrated " << n << " keys from " << best.absoluteFilePath() << Qt::endl;
    }
    now.setValue("settings/migrated", true);
    now.sync();
}

// Headless network-audio control (--netaudio start|stop|status|watch SECONDS). Overrides for a test
// run without touching the saved settings: --na-mode roc|rtp, --na-compression none|opus,
// --na-bitrate 24|32|48|64, --na-port N, --na-latency MS, --na-packet MS,
// --na-rx-gain X (linear 0.5-10, or dB like 9.5dB; with "start" it goes into the receiver config,
// with status/watch it's set live on the running receiver; never saved).
static int netaudioCli(QApplication &app, const QStringList &args, int i) {
    QTextStream out(stdout);
    auto opt = [&](const QString &k) { int j = args.indexOf(k); return j >= 0 && j + 1 < args.size() ? args[j + 1] : QString(); };
    if (QString m = opt("--na-mode"); !m.isEmpty()) naCfg.mode = m == "rtp" ? 2 : 1;
    if (QString c = opt("--na-compression"); !c.isEmpty()) naCfg.compression = c == "opus" ? 1 : 0;
    if (QString b = opt("--na-bitrate"); !b.isEmpty()) naCfg.opusKbps = b.toInt();
    if (QString p = opt("--na-port"); !p.isEmpty()) naCfg.port = p.toInt();
    if (QString l = opt("--na-latency"); !l.isEmpty()) naCfg.latencyMs = l.toInt();
    if (QString p = opt("--na-packet"); !p.isEmpty()) naCfg.packetMs = p.toInt();
    const QString gainOpt = opt("--na-rx-gain");
    if (!gainOpt.isEmpty()) naCfg.rxGain = NetAudioCfg::parseGain(gainOpt);   // validate() rejects out-of-range / garbage
    else if (naCfg.effCompression() == 1 && naCfg.packetMs < 20 && !args.contains("--na-packet")) naCfg.packetMs = 20;
    const QString cmd = i + 1 < args.size() ? args[i + 1] : "status";
    if (QString e = naCfg.validate(); !e.isEmpty()) { out << "netaudio: invalid settings: " << e << Qt::endl; return 2; }
    NetAudio na;
    na.logLine = [&out](const QString &m) { out << QTime::currentTime().toString("HH:mm:ss.zzz") << " " << m << Qt::endl; };
    if (cmd == "stop") { na.stop(true); out << "netaudio: stopped" << Qt::endl; return 0; }
    const int secs = cmd == "watch" && i + 2 < args.size() ? args[i + 2].toInt() : cmd == "start" ? 20 : 6;
    const bool adopt = cmd != "start" && NetAudio::localRunning();
    if (cmd != "start" && !adopt) { out << "netaudio: not running (local unit inactive)" << Qt::endl; return 1; }
    out << "netaudio: " << naCfg.describe() << (adopt ? " (monitoring the running stream)" : "") << Qt::endl;
    na.start(adopt);
    if (adopt && !gainOpt.isEmpty()) na.setRxGain(naCfg.rxGain, false);
    QElapsedTimer t; t.start(); int lastSec = -1; bool wasUp = false;
    QTimer tick; tick.setInterval(250);
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        const int s = int(t.elapsed() / 1000);
        if (s != lastSec) {
            lastSec = s;
            out << QString("t=%1s level=%2 dBFS up=%3 local=%4 remote=%5 | %6").arg(s, 3).arg(na.levelDb(), 6, 'f', 1).arg(na.linkUp())
                       .arg(na.localState(), na.remoteState(), na.statusText()) << Qt::endl;
        }
        if (cmd == "start" && na.linkUp() && !wasUp) { wasUp = true; QTimer::singleShot(3000, &app, [&] { app.exit(0); }); }
        if (t.elapsed() > secs * 1000) app.exit(cmd == "start" ? (na.linkUp() ? 0 : 1) : 0);
    });
    tick.start();
    const int rc = app.exec();
    out << "netaudio: source=" << na.source() << " dest=" << na.destination() << " restarts=" << na.restartCount()
        << " final level=" << QString::number(na.levelDb(), 'f', 1) << " dBFS up=" << na.linkUp() << Qt::endl;
    return rc;
}

// Hidden test hook: --txgate-test HOST PORT TOKENFILE HOLD_MS [--opus KBPS] [--mic] [--mic-source NAME] [--wait-ms N]
// Connects straight to a TX gate (no ssh, nothing started), holds PTT for HOLD_MS with a 700 Hz
// -20 dBFS test tone (or the configured mic with --mic), releases, prints what the gate reports.
// Only ever point this at a test gate (fake flrig + null sink): it keys whatever flrig the gate uses.
static int txgateCli(QApplication &app, const QStringList &args, int i) {
    QTextStream out(stdout);
    if (i + 4 >= args.size()) { out << "usage: --txgate-test HOST PORT TOKENFILE HOLD_MS [--opus KBPS] [--mic]" << Qt::endl; return 2; }
    const QString host = args[i + 1]; const quint16 port = quint16(args[i + 2].toUInt()); const int holdMs = args[i + 4].toInt();
    QFile tf(args[i + 3]);
    if (!tf.open(QIODevice::ReadOnly)) { out << "txgate-test: cannot read token file" << Qt::endl; return 2; }
    const QByteArray tok = tf.readAll().trimmed();
    naCfg.mode = 2; naCfg.compression = 0;
    if (int j = args.indexOf("--mic-source"); j >= 0 && j + 1 < args.size()) naCfg.micSource = args[j + 1];
    if (int j = args.indexOf("--opus"); j >= 0 && j + 1 < args.size()) { naCfg.compression = 1; naCfg.opusKbps = args[j + 1].toInt(); }
    const int waitMs = args.indexOf("--wait-ms") >= 0 ? args[args.indexOf("--wait-ms") + 1].toInt() : 2500;
    TxLink tx;
    QElapsedTimer t; t.start();
    auto ts = [&] { return QString("%1").arg(t.elapsed() / 1000.0, 7, 'f', 3); };
    tx.logLine = [&](const QString &m) { out << ts() << " log " << m << Qt::endl; };
    QString lastStatus;
    tx.changed = [&] { const QString s = tx.statusText(); if (s != lastStatus) { lastStatus = s; out << ts() << " status " << s << Qt::endl; } };
    tx.pttLost = [&](const QString &w) { out << ts() << " pttLost " << w << Qt::endl; };
    tx.startDirect(host, port, tok, !args.contains("--mic"));
    enum { Wait, Held, Released } ph = Wait; qint64 relAt = 0; int rc = 1;
    QTimer tick; tick.setInterval(20);
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (ph == Wait && tx.ready() && !tx.micUp() && t.elapsed() > 8000) { out << ts() << " mic never came up: " << tx.micProblem() << Qt::endl; app.exit(3); }
        if (ph == Wait && tx.ready() && tx.micUp()) { ph = Held; out << ts() << " PTT down (" << tx.codec() << ")" << Qt::endl; tx.setPtt(true); }
        if (ph == Held && !tx.pttHeld() && relAt == 0) { relAt = t.elapsed(); ph = Released; out << ts() << " PTT dropped by the link" << Qt::endl; }
        static qint64 downAt = -1; if (ph == Held && downAt < 0) downAt = t.elapsed();
        if (ph == Held && downAt >= 0 && t.elapsed() - downAt >= holdMs) { tx.setPtt(false); ph = Released; relAt = t.elapsed(); out << ts() << " PTT up" << Qt::endl; }
        if (ph == Released && t.elapsed() - relAt > waitMs) { rc = 0; app.exit(0); }
        if (ph == Wait && t.elapsed() > 15000) { out << ts() << " never ready" << Qt::endl; app.exit(1); }
    });
    tick.start();
    app.exec();
    out << ts() << " final " << tx.statusText() << Qt::endl;
    tx.stop(false, true);
    return rc;
}

// --scope-selftest: drive a real ScopeWidget with synthetic frames that carry the radio's scope centre
// (meta[140:144]) and synthetic mouse clicks. For LSB and USB, snap 0.5 kHz / 1 kHz / off, checks that
// (1) the hover preview stripe before the click == the grey stripe after the rig tunes there (in Hz and,
// with the scope re-centred, against the axis), (2) with snap off the preview is centred on the cursor,
// (3) the axis uses the radio's centre (dial 3.821000 LSB -> centre 3.819500), not the dial.
static int scopeSelfTest() {
    int fails = 0, n = 0;
    QTextStream o(stdout);
    auto check = [&](bool ok, const QString &what) { ++n; if (!ok) ++fails; o << (ok ? "PASS " : "FAIL ") << what << "\n"; };
    const qint64 span = 100000; const int W = 850;
    auto feed = [&](ScopeWidget &w, qint64 dial, int mode) {   // a frame as the FT-710 sends it in centre mode
        const qint64 centre = dial + tuneSide(mode) * 1500;    // measured: LSB centre = dial - 1500 (meta[130:132] = 1500); USB assumed mirrored
        QByteArray meta(150, 0);
        for (int k = 0; k < 4; ++k) meta[140 + k] = char((centre >> (24 - 8 * k)) & 0xff);
        const qint64 st = scopeStartHz(reinterpret_cast<const uchar *>(meta.constData()), meta.size(), dial, span, dial - span / 2, false);
        w.frame(dial, st, span, mode, false, QByteArray(W, char(30)));
        return st;
    };
    auto hz = [&](const ScopeWidget &w, double x) { return w.freqAt(x); };
    {   // axis: LSB dial 3.821000 -> scope centre 3.819500
        ScopeWidget w; w.resize(W, 200);
        const qint64 st = feed(w, 3821000, 0);
        check(st == 3819500 - span / 2, QString("LSB axis start %1 (centre 3.819500, not the dial)").arg(st));
        check(std::abs(hz(w, W / 2.0) - 3819500) < 1, "LSB scope middle pixel = 3.819500 MHz");
    }
    for (int mode : {0, 1, 8, 9}) for (int step : {500, 1000, 0}) for (double x : {101.3, 333.0, 425.0, 612.7, 777.0}) {
        ScopeWidget w; w.resize(W, 200); w.snap = step > 0; w.snapStepHz = step > 0 ? step : 500; w.ssbOffsetHz = 1500;
        double tuned = 0; w.onTune = [&](double h) { tuned = h; };
        const qint64 dial0 = 3821000; feed(w, dial0, mode);
        const QRect area = w.rect();
        const double cursor = hz(w, x), preDial = w.dialForX(x);
        const QRectF pre = w.passRect(preDial, area);
        const double preLo = hz(w, pre.left()), preHi = hz(w, pre.right());
        QMouseEvent pr(QEvent::MouseButtonPress, QPointF(x, 50), QPointF(x, 50), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent rl(QEvent::MouseButtonRelease, QPointF(x, 50), QPointF(x, 50), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&w, &pr); QCoreApplication::sendEvent(&w, &rl);
        const QString tag = QString("%1 snap %2 x=%3 cursor %4 -> dial %5:").arg(tuneSideName(mode)).arg(step ? QString::number(step) : "off")
                                .arg(x).arg(cursor / 1e6, 0, 'f', 4).arg(tuned / 1e6, 0, 'f', 4);
        check(tuned == preDial, tag + " click tunes to the previewed dial");
        feed(w, qint64(tuned), mode);   // rig is now there; scope re-centres
        const QRectF post = w.passRect(w.vfo, area);
        const double postLo = hz(w, post.left()), postHi = hz(w, post.right());
        check(std::abs(postLo - preLo) < 1 && std::abs(postHi - preHi) < 1,
              tag + QString(" preview %1-%2 == stripe after click %3-%4").arg(preLo, 0, 'f', 0).arg(preHi, 0, 'f', 0).arg(postLo, 0, 'f', 0).arg(postHi, 0, 'f', 0));
        const double mid = (preLo + preHi) / 2, tol = step > 0 ? step / 2.0 + 1 : 51;   // no snap: 100 Hz step at 100 kHz span
        check(std::abs(mid - cursor) <= tol, tag + QString(" preview centred on the cursor (off by %1 Hz, allowed %2)").arg(mid - cursor, 0, 'f', 0).arg(tol));
    }
    o << "\n" << (n - fails) << "/" << n << " passed\n";
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    {   // command-line-only modes work without a display
        bool cli = false;
        for (int k = 1; k < argc; ++k) if (!strcmp(argv[k], "--print-config") || !strcmp(argv[k], "--netaudio") || !strcmp(argv[k], "--txgate-test") || !strcmp(argv[k], "--print-receiver-conf") || !strcmp(argv[k], "--scope-selftest") || !strcmp(argv[k], "--tune-keys-selftest")) cli = true;
        if (cli && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (app.arguments().contains("--scope-selftest")) return scopeSelfTest();   // no settings, no network, no rig
    if (app.arguments().contains("--tune-keys-selftest")) return Main::tuneKeysSelfTest();   // idem: static key-routing logic only
    app.setApplicationName("ft710-remote");
    app.setOrganizationName("FT-710_remote");
    app.setDesktopFileName("ft710-remote");
    migrateLegacySettings();
    cfg.load();
    naCfg.load();
    if (app.arguments().contains("--print-config")) {   // non-interactive check: effective settings (no password)
        QTextStream o(stdout);
        o << "file=" << QSettings().fileName() << "\nssh=" << cfg.sshHost << "\nflrig=" << cfg.flrigUrl()
          << "\nconfigDir=" << cfg.rigCfgDir << "\nscope=" << cfg.effScopeHost() << ":" << cfg.scopePort
          << "\nlabel=" << cfg.stationLabel << " help=" << cfg.helpFile
          << "\nnetaudio=" << naCfg.describe() << " port=" << naCfg.port << " source=" << (naCfg.sourceNode.isEmpty() ? "RB_SOURCE" : naCfg.sourceNode)
          << " dest=" << (naCfg.destAddr.isEmpty() ? "auto" : naCfg.destAddr) << " keepOnExit=" << naCfg.keepOnExit
          << " listen=" << (naCfg.listenMute ? "muted" : QString::number(naCfg.listenVol) + "%")
          << " rxGain=" << QString::number(naCfg.rxGain, 'f', 2) << "x (" << QString::number(NetAudioCfg::gainDb(naCfg.rxGain), 'f', 1) << " dB)"
          << " clickTune=" << QSettings().value("scopeClickTune", true).toBool() << " snap=" << QSettings().value("scopeSnapKHz", true).toBool()
          << " snapStep=" << QSettings().value("scopeSnapStep", 500).toInt() << " ssbOffset=" << QSettings().value("scopeSsbOffset", 1500).toInt()
          << "\ntx=tx-gate gatePort=" << naCfg.txPort << " unit=" << naCfg.txUnit
          << " mic=" << (naCfg.micSource.isEmpty() ? "default" : naCfg.micSource)
          << "\nvalid=" << (cfg.validate().isEmpty() ? "yes" : cfg.validate())
          << (naCfg.validate().isEmpty() ? "" : " netaudio: " + naCfg.validate()) << Qt::endl;
        return 0;
    }
    if (app.arguments().contains("--print-receiver-conf")) {   // debugging: the PipeWire receiver config the app would write
        QTextStream(stdout) << NetAudio::receiverConfText();
        return 0;
    }
    if (int i = app.arguments().indexOf("--netaudio"); i >= 0) return netaudioCli(app, app.arguments(), i);
    if (int i = app.arguments().indexOf("--txgate-test"); i >= 0) return txgateCli(app, app.arguments(), i);
    Main w;
    if (!w.restoreSavedGeometry())
        w.resize(QSettings().value("showScope", true).toBool() ? QSize(760, 880) : QSize(520, 560));
    w.show();
    QTimer::singleShot(500, &w, [&w] { w.enableGeometrySaving(); });
    const QStringList args = app.arguments();
    if (args.contains("--connect"))   // hidden test hook: what the Connect button does
        QTimer::singleShot(500, &w, [&w] { w.doConnect(); });
    if (int i = args.indexOf("--tx-test"); i >= 0) {   // hidden test hook: read back, optionally set
        QTimer::singleShot(4000, &w, [&w] {
            QTextStream(stderr) << "tx-read: enabled=" << w.txEnabledForTest() << " value=" << w.txForTest() << Qt::endl; });
        if (i + 1 < args.size() && !args[i + 1].startsWith("-"))
            QTimer::singleShot(4500, &w, [&w, pct = args[i + 1].toInt()] {
                w.setTxForTest(pct); QTextStream(stderr) << "tx-set: " << pct << Qt::endl; });
    }
    if (int i = args.indexOf("--txgate-gui-test"); i >= 0 && i + 4 < args.size()) {   // hidden test hook (test gate only)
        QFile tf(args[i + 3]); if (!tf.open(QIODevice::ReadOnly)) qWarning("cannot read the token file");
        QTimer::singleShot(300, &w, [&w, h = args[i + 1], p = quint16(args[i + 2].toUInt()), t = tf.readAll().trimmed(), ms = args[i + 4].toInt()] {
            w.txGuiTest(h, p, t, ms); });
    }
    if (qEnvironmentVariableIsSet("FT710_SMOKE_MS"))   // headless smoke test: auto-quit
        QTimer::singleShot(qEnvironmentVariable("FT710_SMOKE_MS").toInt(), &app, [&] {
            QTextStream out(stdout);
            for (auto *l : w.findChildren<QLabel *>()) out << "label: " << l->text() << "\n";
            for (auto *b : w.findChildren<QPushButton *>()) out << "button: " << b->text().replace('\n', ' ') << (b->isEnabled() ? "" : " [disabled]") << "\n";
            for (auto *b : w.findChildren<QToolButton *>()) out << "toolbutton: " << b->text() << (b->isEnabled() ? "" : " [disabled]") << (b->isVisible() ? "" : " [hidden]") << "\n";
            for (auto *sl : w.findChildren<QSlider *>()) out << "slider: " << sl->toolTip().left(40) << " value=" << sl->value()
                                                            << (sl->isEnabled() ? "" : " [disabled]") << (sl->isVisible() ? "" : " [hidden]") << "\n";
            for (auto *c : w.findChildren<QComboBox *>()) out << "combo: " << c->toolTip().left(40) << " current=" << c->currentText()
                                                              << (c->isEnabled() ? "" : " [disabled]") << (c->isVisible() ? "" : " [hidden]") << "\n";
            for (auto *sb : w.findChildren<QSpinBox *>()) out << "spinbox: " << sb->toolTip().left(40) << " value=" << sb->value()
                                                              << (sb->isEnabled() ? "" : " [disabled]") << (sb->isVisible() ? "" : " [hidden]") << "\n";
            for (auto *c : w.findChildren<QCheckBox *>()) out << "checkbox: " << c->text() << (c->isChecked() ? " [checked]" : "")
                                                              << (c->isEnabled() ? "" : " [disabled]") << (c->isVisible() ? "" : " [hidden]") << "\n";
            app.quit();
        });
    return app.exec();
}
