// FT-710 Remote: small Qt6 front end for a remote Yaesu FT-710 behind a Mumble audio bridge
// (radio-bridge) and flrig on the "rig host". All station settings come from the Settings dialog.
// PTT goes to the local Mumble client over its RPC socket ($XDG_RUNTIME_DIR/MumbleSocket),
// the same channel `mumble rpc starttalking` uses, so the server password never hits argv.
#include <QtWidgets>
#include <QLocalSocket>
#include <QTcpSocket>
#include <functional>
#include <memory>
#include <cmath>
#include "tune.h"
#include <QDomDocument>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>

// Minimal X11 (XWayland) helpers. Mumble 1.4 has no "start minimized" option, and on native
// Wayland no other program may minimize its window. So when FT-710 Remote launches Mumble with
// "Start Mumble minimized" on, it runs Mumble under XWayland (QT_QPA_PLATFORM=xcb) and asks the
// window manager to iconify its main window (ICCCM WM_CHANGE_STATE). Mumble then hides itself in
// its top-bar tray icon (its default "hide in tray when minimized" behaviour).
namespace x11 {
struct Conn {
    xcb_connection_t *c = nullptr; xcb_window_t root = 0;
    Conn() {
        int scr = 0; c = xcb_connect(nullptr, &scr);
        if (xcb_connection_has_error(c)) { xcb_disconnect(c); c = nullptr; return; }
        auto it = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (int i = 0; i < scr && it.rem; ++i) xcb_screen_next(&it);
        root = it.data->root;
    }
    ~Conn() { if (c) xcb_disconnect(c); }
    xcb_atom_t atom(const char *n) {
        auto *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(strlen(n)), n), nullptr);
        xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE); free(r); return a;
    }
    QByteArray prop(xcb_window_t w, xcb_atom_t a, xcb_atom_t type) {
        auto *r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, a, type, 0, 4096), nullptr);
        QByteArray out;
        if (r && r->format) out = QByteArray((const char *)xcb_get_property_value(r), xcb_get_property_value_length(r) * (r->format / 8));
        free(r); return out;
    }
    void clientMessage(xcb_window_t w, xcb_atom_t type, uint32_t d0, uint32_t d1 = 0, uint32_t d2 = 0) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE; ev.format = 32; ev.window = w; ev.type = type;
        ev.data.data32[0] = d0; ev.data.data32[1] = d1; ev.data.data32[2] = d2;
        xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char *)&ev);
        xcb_flush(c);
    }
    // Managed, non-transient top-level windows of Mumble: by PID if given, else by WM_CLASS.
    QList<xcb_window_t> mumbleWindows(qint64 pid, bool viewableOnly) {
        QList<xcb_window_t> res;
        if (!c) return res;
        QByteArray list = prop(root, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
        const xcb_atom_t aPid = atom("_NET_WM_PID");
        for (int i = 0; i + 4 <= list.size(); i += 4) {
            xcb_window_t w; memcpy(&w, list.constData() + i, 4);
            if (!prop(w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW).isEmpty()) continue;
            if (pid > 0) {
                QByteArray p = prop(w, aPid, XCB_ATOM_CARDINAL);
                uint32_t v = 0; if (p.size() >= 4) memcpy(&v, p.constData(), 4);
                if (qint64(v) != pid) continue;
            } else if (!prop(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING).toLower().contains("mumble")) continue;
            if (viewableOnly) {
                auto *a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, w), nullptr);
                bool vis = a && a->map_state == XCB_MAP_STATE_VIEWABLE; free(a);
                if (!vis) continue;
            }
            res.append(w);
        }
        return res;
    }
    // All top-level windows (managed or not, e.g. hidden in the tray) of a process (pid > 0) or
    // of Mumble (WM_CLASS), that accept WM_DELETE_WINDOW and carry a WM_STATE (real client windows,
    // not Qt's helper/group-leader windows).
    QList<xcb_window_t> closableWindows(qint64 pid) {
        QList<xcb_window_t> res, fallback;
        if (!c) return res;
        auto *t = xcb_query_tree_reply(c, xcb_query_tree(c, root), nullptr);
        if (!t) return res;
        const xcb_atom_t aPid = atom("_NET_WM_PID"), aProt = atom("WM_PROTOCOLS"), aDel = atom("WM_DELETE_WINDOW"),
                         aState = atom("WM_STATE");
        xcb_window_t *kids = xcb_query_tree_children(t);
        for (int i = 0; i < xcb_query_tree_children_length(t); ++i) {
            const xcb_window_t w = kids[i];
            if (pid > 0) {
                QByteArray p = prop(w, aPid, XCB_ATOM_CARDINAL);
                uint32_t v = 0; if (p.size() >= 4) memcpy(&v, p.constData(), 4);
                if (qint64(v) != pid) continue;
            } else if (!prop(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING).toLower().contains("mumble")) continue;
            if (!prop(w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW).isEmpty()) continue;
            QByteArray pr = prop(w, aProt, XCB_ATOM_ATOM); bool del = false;
            for (int k = 0; k + 4 <= pr.size(); k += 4) { xcb_atom_t a; memcpy(&a, pr.constData() + k, 4); if (a == aDel) del = true; }
            if (!del) continue;
            if (!prop(w, aState, aState).isEmpty()) res.append(w);     // has been managed by the WM
            else {                                                    // never managed: skip tiny helper windows
                auto *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr);
                if (g && g->width > 50 && g->height > 50) fallback.append(w);
                free(g);
            }
        }
        free(t);
        return res.isEmpty() ? fallback : res;
    }
    // ICCCM WM_DELETE_WINDOW sent straight to the window: exactly what the title-bar close button
    // does, so the program runs its normal close path (Mumble saves its settings, may ask first).
    void requestClose(xcb_window_t w) {
        xcb_client_message_event_t ev{};
        ev.response_type = XCB_CLIENT_MESSAGE; ev.format = 32; ev.window = w; ev.type = atom("WM_PROTOCOLS");
        ev.data.data32[0] = atom("WM_DELETE_WINDOW"); ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(c, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
        xcb_flush(c);
    }
    void iconify(xcb_window_t w) { clientMessage(w, atom("WM_CHANGE_STATE"), 3 /*IconicState*/); }
    void activate(xcb_window_t w) { clientMessage(w, atom("_NET_ACTIVE_WINDOW"), 2 /*pager*/, XCB_CURRENT_TIME); }
};
}

// ---------------------------------------------------------------------------------------------
// Station configuration (QSettings, edited in the Settings dialog). Nothing station-specific is
// compiled in. The "rig host" is the machine with the FT-710, flrig, radio-bridge and ft710-scope;
// this app reaches it with ssh (key-based login, BatchMode) for status, TX enable and CAT via flrig.
static QString shq(const QString &s) { QString r = s; r.replace("'", "'\\''"); return "'" + r + "'"; }
static QString hostPart(const QString &dest) { const int at = dest.lastIndexOf('@'); return at >= 0 ? dest.mid(at + 1) : dest; }

struct Config {
    QString sshHost;                                  // ssh destination: alias, host or user@host
    QString flrigHost = "127.0.0.1"; int flrigPort = 12345;   // flrig XML-RPC as seen from the rig host
    QString bridgeService = "radio-bridge";           // systemd --user unit on the rig host
    QString murmurService = "murmur";                 // system unit of the Mumble server ("" = don't check)
    QString bridgeCfgDir = "~/.config/radio-bridge";  // tx_enabled, tx_gain, server.pw on the rig host
    QString bridgeUser = "FT710";                     // Mumble name the bridge uses (status display)
    QString mumbleHost; int mumblePort = 64738;       // "" = same host as the rig host
    QString mumbleUser, mumbleChannel;
    int pwSource = 0;   // 0 = read <bridge dir>/server.pw over ssh, 1 = none / Mumble's saved login, 2 = stored locally
    QString mumblePassword;                           // only used with pwSource 2 (plain text in the local config)
    bool startMinimized = true;
    QString scopeHost; int scopePort = 7355;          // "" = same host as the rig host
    QString stationLabel;                             // shown in the window title (e.g. your callsign)
    QString helpFile;                                 // optional local Markdown notes shown in Help

    bool configured() const { return !sshHost.isEmpty(); }
    QString rigName() const { return configured() ? hostPart(sshHost) : QString("rig host"); }
    QString effMumbleHost() const { return mumbleHost.isEmpty() ? hostPart(sshHost) : mumbleHost; }
    QString effScopeHost() const { return scopeHost.isEmpty() ? hostPart(sshHost) : scopeHost; }
    QString flrigUrl() const {
        return QString("http://%1:%2/RPC2").arg(flrigHost.contains(':') ? "[" + flrigHost + "]" : flrigHost).arg(flrigPort);
    }
    // Shell expression for a file in the bridge's config dir on the rig host ("~/" stays expandable).
    QString remoteFile(const QString &name) const {
        QString d = bridgeCfgDir; while (d.endsWith('/') && d.size() > 1) d.chop(1);
        if (d == "~") return "\"$HOME\"/" + shq(name);
        if (d.startsWith("~/")) return "\"$HOME\"/" + shq(d.mid(2) + "/" + name);
        return shq(d + "/" + name);
    }
    void load() {
        QSettings st; Config def;
        sshHost = st.value("rig/sshHost").toString().trimmed();
        flrigHost = st.value("rig/flrigHost", def.flrigHost).toString().trimmed();
        flrigPort = st.value("rig/flrigPort", def.flrigPort).toInt();
        bridgeService = st.value("rig/bridgeService", def.bridgeService).toString().trimmed();
        murmurService = st.value("rig/murmurService", def.murmurService).toString().trimmed();
        bridgeCfgDir = st.value("rig/bridgeConfigDir", def.bridgeCfgDir).toString().trimmed();
        bridgeUser = st.value("rig/bridgeMumbleUser", def.bridgeUser).toString().trimmed();
        mumbleHost = st.value("mumble/host").toString().trimmed();
        mumblePort = st.value("mumble/port", def.mumblePort).toInt();
        mumbleUser = st.value("mumble/user").toString().trimmed();
        mumbleChannel = st.value("mumble/channel").toString().trimmed();
        pwSource = st.value("mumble/passwordSource", 0).toInt();
        mumblePassword = st.value("mumble/password").toString();
        startMinimized = st.value("startMumbleMinimized", true).toBool();
        scopeHost = st.value("scopeHost").toString().trimmed();
        scopePort = st.value("scopePort", def.scopePort).toInt();
        stationLabel = st.value("station/label").toString().trimmed();
        helpFile = st.value("station/helpFile").toString().trimmed();
    }
    void save() const {
        QSettings st;
        st.setValue("rig/sshHost", sshHost); st.setValue("rig/flrigHost", flrigHost); st.setValue("rig/flrigPort", flrigPort);
        st.setValue("rig/bridgeService", bridgeService); st.setValue("rig/murmurService", murmurService);
        st.setValue("rig/bridgeConfigDir", bridgeCfgDir); st.setValue("rig/bridgeMumbleUser", bridgeUser);
        st.setValue("mumble/host", mumbleHost); st.setValue("mumble/port", mumblePort);
        st.setValue("mumble/user", mumbleUser); st.setValue("mumble/channel", mumbleChannel);
        st.setValue("mumble/passwordSource", pwSource);
        if (pwSource == 2) st.setValue("mumble/password", mumblePassword); else st.remove("mumble/password");
        st.setValue("startMumbleMinimized", startMinimized);
        st.setValue("scopeHost", scopeHost); st.setValue("scopePort", scopePort);
        st.setValue("station/label", stationLabel); st.setValue("station/helpFile", helpFile);
        st.sync();
        // The file may hold a Mumble password: keep it owner-only.
        QFile::setPermissions(st.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    // Returns "" if valid, else a message. Strict on anything that reaches a shell or ssh's argv.
    QString validate() const {
        static const QRegularExpression dest("^([A-Za-z0-9._-]+@)?[A-Za-z0-9_][A-Za-z0-9._-]*$"),
            host("^([A-Za-z0-9_][A-Za-z0-9._-]*|\\[?[0-9A-Fa-f:.]+\\]?)$"),
            unit("^[A-Za-z0-9_][A-Za-z0-9@._-]*$"), path("^(~|~/|/)?[A-Za-z0-9._/ +-]*$"),
            user("^[^/@:#?\\s][^/@:#?]{0,63}$");
        if (sshHost.isEmpty()) return "Rig host (ssh) is required.";
        if (!dest.match(sshHost).hasMatch()) return "Rig host (ssh) must look like host, alias or user@host.";
        if (!host.match(flrigHost).hasMatch()) return "flrig host is not a valid host name or address.";
        if (!unit.match(bridgeService).hasMatch()) return "Bridge service name is not a valid systemd unit name.";
        if (!murmurService.isEmpty() && !unit.match(murmurService).hasMatch()) return "Mumble server service name is not a valid unit name.";
        if (bridgeCfgDir.isEmpty() || !path.match(bridgeCfgDir).hasMatch()) return "Bridge config dir must be a plain path (~/... or /...).";
        if (!mumbleHost.isEmpty() && !host.match(mumbleHost).hasMatch()) return "Mumble server is not a valid host name or address.";
        if (mumbleUser.isEmpty() || !user.match(mumbleUser).hasMatch()) return "Mumble user name is required (no / @ : # ?).";
        if (!bridgeUser.isEmpty() && !user.match(bridgeUser).hasMatch()) return "Bridge Mumble name contains invalid characters.";
        if (mumbleChannel.contains("//") || mumbleChannel.contains('?') || mumbleChannel.contains('#')) return "Mumble channel: use Parent/Child, no ? or #.";
        if (!scopeHost.isEmpty() && !host.match(scopeHost).hasMatch()) return "Scope host is not a valid host name or address.";
        if (pwSource == 2 && mumblePassword.isEmpty()) return "Enter the Mumble password, or pick another password source.";
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

static QString mumbleSocketPath() {
    QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!rt.isEmpty() && QDir(rt).exists()) return QDir(rt).filePath("MumbleSocket");
    return QDir::home().filePath(".MumbleSocket");
}

// Send one request to Mumble's RPC socket. elem = "self"/"url"; params as child elements.
static bool mumbleRpc(const QString &elem, const QList<QPair<QString, QString>> &params) {
    QLocalSocket s;
    s.connectToServer(mumbleSocketPath());
    if (!s.waitForConnected(500)) return false;
    QDomDocument d;
    QDomElement r = d.createElement(elem);
    for (auto &kv : params) {
        QDomElement e = d.createElement(kv.first);
        e.appendChild(d.createTextNode(kv.second));
        r.appendChild(e);
    }
    d.appendChild(r);
    s.write(d.toByteArray());
    s.flush();
    if (!s.waitForReadyRead(1500)) return false;
    QDomDocument rep;
    rep.setContent(s.readAll());
    return rep.firstChildElement("reply").firstChildElement("succeeded").text() == "true";
}
static bool mumbleRunning() {
    QLocalSocket s;
    s.connectToServer(mumbleSocketPath());
    return s.waitForConnected(300);
}

#include "netaudio.h"   // network audio (LAN stream) mode; uses cfg, shq() and sshArgs() above
#include "txgate.h"     // TX without Mumble (mic -> ft710-txgate on the rig host); uses naCfg

// One ssh round trip per poll. Prints key=value lines, then the log if asked.
// Settings arrive as shell variables (BRIDGE, MURMUR, TXEN, TXGAIN, FLRIG) prepended by pollScript().
static const char *POLL_SCRIPT = R"SH(
if [ -n "$MURMUR" ]; then echo "murmur=$(systemctl is-active "$MURMUR" 2>/dev/null)"; else echo "murmur=not checked"; fi
echo "bridge=$(systemctl --user is-active "$BRIDGE" 2>/dev/null)"
[ -n "$TXUNIT" ] && echo "txgate=$(systemctl --user is-active "$TXUNIT" 2>/dev/null)"
[ -e "$TXEN" ] && echo tx=on || echo tx=off
INV=$(systemctl --user show -p InvocationID --value "$BRIDGE" 2>/dev/null)
if [ -n "$INV" ]; then J=$(journalctl --user _SYSTEMD_INVOCATION_ID=$INV --no-pager -o cat 2>/dev/null | tail -n 400); fi
echo "conn=$(printf '%s\n' "$J" | grep -E 'Mumble (connected|disconnected)|status connected=' | tail -1)"
echo "status=$(printf '%s\n' "$J" | grep 'status connected=' | tail -1)"
rpc() { curl -s -m 2 -H 'Content-Type: text/xml' -d "<?xml version=\"1.0\"?><methodCall><methodName>$1</methodName></methodCall>" "$FLRIG" | tr -d '\n\t ' | sed -n 's/.*<value>\(<[a-z0-9]*>\)\{0,1\}\([^<]*\).*/\2/p'; }
echo "ptt=$(rpc rig.get_ptt)"
echo "vfo=$(rpc rig.get_vfo)"
echo "txgain=$(cat "$TXGAIN" 2>/dev/null || echo none)"
if [ "$1" = log ]; then echo ---LOG---; journalctl --user -u "$BRIDGE" -n 20 --no-pager 2>/dev/null; fi
)SH";
static QByteArray pollScript() {
    const QString vars = QString("BRIDGE=%1\nMURMUR=%2\nTXEN=%3\nTXGAIN=%4\nFLRIG=%5\nTXUNIT=%6\n")
        .arg(shq(cfg.bridgeService), shq(cfg.murmurService), cfg.remoteFile("tx_enabled"), cfg.remoteFile("tx_gain"), shq(cfg.flrigUrl()),
             naCfg.enabled() && naCfg.txGate ? shq(naCfg.txUnit) : QString());
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

        // Connection row
        auto *row = new QHBoxLayout;
        connectBtn = new QPushButton("Connect");
        disconnectBtn = new QPushButton("Disconnect…");
        disconnectBtn->setToolTip("Stops talking and offers to close Mumble the normal way (like its own close/Quit, "
                                  "so it saves its settings), even when it is hidden in the tray.");
        mumbleLbl = new QLabel;
        row->addWidget(connectBtn); row->addWidget(disconnectBtn); row->addWidget(mumbleLbl, 1);
        v->addLayout(row);
        startMinChk = new QCheckBox("Start Mumble minimized");
        startMinChk->setToolTip("When Connect has to start Mumble, start it minimized to its top-bar tray icon "
                                "(runs Mumble via XWayland so it can be minimized). Click the tray icon, or "
                                "Disconnect…, to show it. Has no effect if Mumble is already running or you start it yourself.");
        startMinChk->setChecked(cfg.startMinimized);
        connect(startMinChk, &QCheckBox::toggled, this, [](bool on) { cfg.startMinimized = on; QSettings().setValue("startMumbleMinimized", on); });
        v->addWidget(startMinChk);

        // PTT
        ptt = new QPushButton("PTT\n(hold, or hold Space)");
        ptt->setMinimumHeight(140);
        QFont f = ptt->font(); f.setPointSize(f.pointSize() + 10); f.setBold(true); ptt->setFont(f);
        v->addWidget(ptt);

        // RX volume: Mumble's playback stream on this computer (PipeWire/Pulse sink-input), same as the
        // per-app slider in GNOME sound settings. Never touches the radio or the master volume.
        // Hidden in direct-TX mode (Mumble is not used there; the "RX network" row has the volume).
        volRowW = new QWidget; auto *volRow = new QHBoxLayout(volRowW); volRow->setContentsMargins(0, 0, 0, 0);
        volRow->addWidget(new QLabel("RX volume (Mumble):"));
        volSlider = new QSlider(Qt::Horizontal);
        volSlider->setRange(0, 150); volSlider->setPageStep(5); volSlider->setSingleStep(1);
        volSlider->setTickPosition(QSlider::TicksBelow); volSlider->setTickInterval(25);
        volLbl = new QLabel("—"); volLbl->setMinimumWidth(volLbl->fontMetrics().horizontalAdvance("150%  "));
        muteBtn = new QToolButton; muteBtn->setText("Mute"); muteBtn->setCheckable(true);
        volRow->addWidget(volSlider, 1); volRow->addWidget(volLbl); volRow->addWidget(muteBtn);
        v->addWidget(volRowW);
        setVolEnabled(false);

        // TX level: gain the bridge on the rig host applies to Mumble audio before it plays it into the FT-710
        // USB input (~/.config/radio-bridge/tx_gain, linear factor = (slider%/100)^3, same curve as a
        // PulseAudio volume). Works whether or not the bridge's TX stream exists, and survives restarts.
        auto *txRow = new QHBoxLayout;
        txRow->addWidget(new QLabel("TX level (to radio):"));
        txSlider = new QSlider(Qt::Horizontal);
        txSlider->setRange(0, 150); txSlider->setPageStep(5); txSlider->setSingleStep(1);
        txSlider->setTickPosition(QSlider::TicksBelow); txSlider->setTickInterval(25);
        txLbl = new QLabel("—"); txLbl->setMinimumWidth(txLbl->fontMetrics().horizontalAdvance("150%  "));
        txResetBtn = new QToolButton; txResetBtn->setText("100%");
        txRow->addWidget(txSlider, 1); txRow->addWidget(txLbl); txRow->addWidget(txResetBtn);
        v->addLayout(txRow);
        // Keep both rows' labels the same width so the sliders line up.
        { QLabel *a = qobject_cast<QLabel *>(volRow->itemAt(0)->widget()), *b = qobject_cast<QLabel *>(txRow->itemAt(0)->widget());
          int w = qMax(a->sizeHint().width(), b->sizeHint().width()); a->setMinimumWidth(w); b->setMinimumWidth(w);
          int bw = qMax(muteBtn->sizeHint().width(), txResetBtn->sizeHint().width()); muteBtn->setMinimumWidth(bw); txResetBtn->setMinimumWidth(bw); }
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
        netGain->setToolTip("RX gain of the network audio on this computer (-6 to +20 dB, default +9.5 dB = 3x like Mumble). "
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
        netMuteBtn->setToolTip("Listen to the network stream on this computer's speakers. Muted by default because Mumble "
                               "usually plays the radio audio already (mute one of them to avoid hearing it twice).");
        netStartBtn = new QPushButton("Start");
        netStartBtn->setToolTip("Start/stop the network audio stream (also started by Connect and stopped by Disconnect).");
        nr->addWidget(netName); nr->addWidget(netLevel); nr->addWidget(gainName); nr->addWidget(netGain, 1); nr->addWidget(netGainLbl); nr->addWidget(netVol, 1); nr->addWidget(netVolLbl); nr->addWidget(netMuteBtn); nr->addWidget(netStartBtn);
        v->addWidget(netRow);
        // Direct TX (network mode, no Mumble): mic level of what PTT would send to the TX gate.
        micRow = new QWidget; auto *mr = new QHBoxLayout(micRow); mr->setContentsMargins(0, 0, 0, 0);
        auto *micName = new QLabel("TX mic:");
        micBar = new QProgressBar; micBar->setRange(0, 60); micBar->setValue(0); micBar->setTextVisible(true); micBar->setFormat("—");
        micBar->setToolTip("Level of the microphone the app sends to the TX gate while PTT is held (dBFS). "
                           "Aim for peaks around -12 to -6 dBFS; set the mic level in your desktop sound settings "
                           "and the level into the radio with \"TX level\".");
        micDev = new QLabel; micDev->setStyleSheet("color:gray;");
        mr->addWidget(micName); mr->addWidget(micBar, 1); mr->addWidget(micDev);
        v->addWidget(micRow);
        { QLabel *a = qobject_cast<QLabel *>(nr->itemAt(0)->widget()); int w = qMax(a->sizeHint().width(), micName->sizeHint().width());
          a->setMinimumWidth(w); micName->setMinimumWidth(w); }
        tx = new TxLink(this);
        tx->changed = [this] { updateTxUi(); };
        tx->logLine = [](const QString &m) { QTextStream(stderr) << "txgate: " << m << Qt::endl; };
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
        connect(netStartBtn, &QPushButton::clicked, this, [this] { if (na->active()) na->stop(false); else na->start(); });
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
        for (auto *l : {&murmurLbl, &bridgeLbl, &connLbl, &rxLbl, &txEnLbl, &pttLbl, &vfoLbl}) *l = new QLabel("…");
        QFont big = pttLbl->font(); big.setBold(true); big.setPointSize(big.pointSize() + 4);
        pttLbl->setFont(big); vfoLbl->setFont(big);
        g->addRow("Mumble server (murmur):", murmurLbl);
        g->addRow("radio-bridge service:", bridgeLbl);
        g->addRow("Bridge ↔ Mumble:", connLbl);
        g->addRow("RX frames sent:", rxLbl);
        g->addRow("TX keying:", txEnLbl);
        g->addRow("Radio PTT (flrig):", pttLbl);
        g->addRow("VFO:", vfoLbl);
        netLbl = new QLabel("…"); netLbl->setWordWrap(true);
        g->addRow("Network audio:", netLbl);
        txgLbl = new QLabel("…"); txgLbl->setWordWrap(true);
        g->addRow("TX gate (direct TX):", txgLbl);
        statusForm = g;
        v->addWidget(box);

        auto *row2 = new QHBoxLayout;
        txBtn = new QPushButton("…");
        restartBtn = new QPushButton("Restart bridge");
        auto *helpBtn = new QPushButton("Help / About");
        settingsBtn = new QPushButton("Settings…");
        settingsBtn->setToolTip("Rig host, flrig, Mumble server and band-scope settings.");
        logBtn = new QToolButton; logBtn->setText("Show log"); logBtn->setCheckable(true);
        row2->addWidget(txBtn, 1); row2->addWidget(restartBtn); row2->addWidget(logBtn); row2->addWidget(settingsBtn); row2->addWidget(helpBtn);
        v->addLayout(row2);

        log = new QPlainTextEdit; log->setReadOnly(true); log->setVisible(false);
        log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        log->setMinimumHeight(220);
        v->addWidget(log);

        for (auto *b : findChildren<QAbstractButton *>()) b->setFocusPolicy(Qt::NoFocus);
        log->setFocusPolicy(Qt::NoFocus);

        connect(connectBtn, &QPushButton::clicked, this, [this] { doConnect(); });
        connect(disconnectBtn, &QPushButton::clicked, this, [this] { doDisconnect(); });
        connect(ptt, &QPushButton::pressed, this, [this] { pressPtt(); });
        connect(ptt, &QPushButton::released, this, [this] { stopTalk(); });
        connect(txBtn, &QPushButton::clicked, this, [this] { toggleTx(); });
        connect(restartBtn, &QPushButton::clicked, this, [this] {
            stopTalk();
            if (QMessageBox::question(this, "Restart bridge", QString("Restart %1 on %2?").arg(cfg.bridgeService, cfg.rigName())) == QMessageBox::Yes) {
                bool ok; sshRun("systemctl --user restart " + shq(cfg.bridgeService), &ok);
                if (!ok) QMessageBox::warning(this, "Restart bridge", "Restart failed.");
                poll();
            }
        });
        connect(logBtn, &QToolButton::toggled, this, [this](bool on) {
            log->setVisible(on); logBtn->setText(on ? "Hide log" : "Show log");
            if (on) poll();
            else if (!restoreSavedGeometry()) adjustSize();   // back to the remembered size
        });
        connect(helpBtn, &QPushButton::clicked, this, [this] { showHelp(); });
        connect(settingsBtn, &QPushButton::clicked, this, [this] { editSettings(); });

        volApply.setSingleShot(true); volApply.setInterval(60);   // coalesce slider drags
        connect(&volApply, &QTimer::timeout, this, [this] { applyVolume(volSlider->value()); });
        connect(volSlider, &QSlider::valueChanged, this, [this](int val) {
            volLbl->setText(QString("%1%").arg(val));
            if (volSync) return;           // programmatic update from polling
            volTouched.start();
            volApply.start();
        });
        connect(muteBtn, &QToolButton::toggled, this, [this](bool on) {
            muteBtn->setText(on ? "Muted" : "Mute");
            muteBtn->setStyleSheet(on ? "background:#c00; color:white;" : "");
            if (volSync) return;
            volTouched.start();
            for (int idx : volStreams) runPactl({"set-sink-input-mute", QString::number(idx), on ? "1" : "0"});
        });
        volTimer.setInterval(2000);
        connect(&volTimer, &QTimer::timeout, this, [this] { pollVolume(); });
        volTimer.start();
        pollVolume();

        qApp->installEventFilter(this);
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            stopTalk(true);
            if (tx->active()) tx->stop(naCfg.restoreBridge, naCfg.restoreBridge, true);   // gate unkeys; bridge back if configured
            if (na->active() && !naCfg.keepOnExit) na->stop(true);   // leave nothing streaming behind
        });

        timer.setInterval(POLL_IDLE_MS);
        connect(&timer, &QTimer::timeout, this, [this] { poll(); adjustPollRate(); });
        // PTT debounce: only start talking if PTT is still held after PTT_MIN_HOLD_MS.
        holdTimer.setSingleShot(true); holdTimer.setInterval(PTT_MIN_HOLD_MS);
        connect(&holdTimer, &QTimer::timeout, this, [this] { beginTalk(); });
        // Retry stoptalking until Mumble acknowledges it (extra stops are harmless: Mumble
        // only decrements its PTT count while it is > 0).
        stopRetry.setInterval(250);
        connect(&stopRetry, &QTimer::timeout, this, [this] {
            if (mumbleRpc("self", {{"stoptalking", "stoptalking"}})) { stopRetry.stop(); stopTries = 0; updatePttWarn(false); return; }
            if (++stopTries >= 8) { stopRetry.stop(); stopTries = 0; updatePttWarn(true); }
        });
        timer.start();
        applyConfig();
        // App restarted while the network stream kept running (Settings: keep running): just monitor it.
        if (naCfg.enabled() && cfg.configured() && NetAudio::localRunning()) na->start(true);
        if (!cfg.configured()) QTimer::singleShot(300, this, [this] { editSettings(); });
    }

    ~Main() override {
        qApp->removeEventFilter(this);
        timer.stop();
        if (pollProc) { pollProc->disconnect(); pollProc->kill(); pollProc->waitForFinished(1000); }
        volTimer.stop();
        if (volProc) { volProc->disconnect(); volProc->kill(); volProc->waitForFinished(1000); }
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
    QPushButton *connectBtn, *disconnectBtn, *ptt, *txBtn, *restartBtn;
    // --- band scope ---
    QCheckBox *scopeChk, *scopeTuneChk, *scopeSnapChk; QWidget *scopeTuneOpts; QComboBox *scopeSnapStep; QSpinBox *scopeSsbOff;
    ScopeWidget *scope;
    bool radioRx = false;          // flrig last reported PTT=0 (tuning by click is only allowed then)
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
    // Click-to-tune: flrig rig.set_vfo on the rig host (frequency only; never PTT, never menu/EX settings).
    void tuneTo(double hz) {
        if (tuneProc || !radioRx || talking || hz < 30e3 || hz > 75e6) return;
        const QString body = QString("<?xml version=\"1.0\"?><methodCall><methodName>rig.set_vfo</methodName><params><param>"
                                     "<value><double>%1</double></value></param></params></methodCall>").arg(hz, 0, 'f', 0);
        tuneProc = new QProcess(this);
        connect(tuneProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            if (st != QProcess::NormalExit || code != 0) QTextStream(stderr) << "rig.set_vfo failed" << Qt::endl;
            tuneProc->deleteLater(); tuneProc = nullptr; poll();
        });
        connect(tuneProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && tuneProc) { tuneProc->deleteLater(); tuneProc = nullptr; }
        });
        tuneProc->start("ssh", sshArgs("curl -s -m 3 -H 'Content-Type: text/xml' --data-binary @- " + shq(cfg.flrigUrl()) + " >/dev/null"));
        tuneProc->write(body.toUtf8()); tuneProc->closeWriteChannel();
        QTimer::singleShot(8000, tuneProc, [p = tuneProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    QCheckBox *startMinChk;
    QTimer *minTimer = nullptr;

    // After launching Mumble (under XWayland) minimize each of its main windows once, as soon as it maps.
    void minimizeMumbleWhenShown(qint64 pid) {
        if (minTimer) { minTimer->stop(); minTimer->deleteLater(); }
        minTimer = new QTimer(this); minTimer->setInterval(100);
        auto done = std::make_shared<QSet<xcb_window_t>>();
        auto clock = std::make_shared<QElapsedTimer>(); clock->start();
        auto firstHit = std::make_shared<qint64>(-1);
        connect(minTimer, &QTimer::timeout, this, [this, pid, done, clock, firstHit] {
            x11::Conn x;
            for (xcb_window_t w : x.mumbleWindows(pid, true))
                if (!done->contains(w)) {
                    done->insert(w); x.iconify(w);
                    if (*firstHit < 0) *firstHit = clock->elapsed();
                    QTextStream(stderr) << "start-minimized: iconified window 0x" << Qt::hex << w << Qt::dec
                                        << " at " << clock->elapsed() << " ms" << Qt::endl;
                }
            // Watch 15 s at most, or 3 s after the main window was minimized (catches late extra windows).
            if (clock->elapsed() > 15000 || (*firstHit >= 0 && clock->elapsed() > *firstHit + 3000)) {
                minTimer->stop(); minTimer->deleteLater(); minTimer = nullptr;
            }
        });
        minTimer->start();
    }
    // Bring Mumble to the front: RPC focus (show+raise), plus un-minimize it if it's an XWayland window.
    void raiseMumble() {
        mumbleRpc("focus", {});
        QDBusInterface("net.sourceforge.mumble.mumble", "/", "net.sourceforge.mumble.Mumble").call(QDBus::NoBlock, "focus");
        QTimer::singleShot(150, this, [] { x11::Conn x; for (xcb_window_t w : x.mumbleWindows(0, false)) x.activate(w); });
    }
    QToolButton *logBtn; QPushButton *settingsBtn; QGroupBox *statusBox;
    QLabel *mumbleLbl, *murmurLbl, *bridgeLbl, *connLbl, *rxLbl, *txEnLbl, *pttLbl, *vfoLbl;
    QPlainTextEdit *log;
    QTimer timer;
    QProcess *pollProc = nullptr;
    bool talking = false;          // a starttalking was sent (acked or not) and no stop yet
    static constexpr int POLL_IDLE_MS = 3000, POLL_TALK_MS = 500, PTT_MIN_HOLD_MS = 150;
    QTimer holdTimer, stopRetry;
    int stopTries = 0;
    QElapsedTimer fastPollSince;   // keep fast polling briefly after release so the readout shows the unkey
    QString txState = "unknown";

    // --- RX volume (Mumble sink-input) ---
    // --- TX level (bridge's playback stream on the rig host) ---
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
        const QString tip = on ? "Level of the TX audio (Mumble via radio-bridge, or your mic via the TX gate) played into the FT-710's USB input. "
                                 "Lower it if you're driving the radio too hard; 100% = unchanged. Saved on the rig host "
                                 "(~/.config/radio-bridge/tx_gain), so it survives bridge restarts. "
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
        // Atomic replace, so the bridge never reads a half-written file.
        const QString g = QString::number(pctToGain(pct), 'f', 6);
        txProc->start("ssh", sshArgs(QString("f=%1; echo %2 > \"$f.tmp\" && mv -f \"$f.tmp\" \"$f\"").arg(cfg.remoteFile("tx_gain")).arg(g)));
        QTimer::singleShot(8000, txProc, [p = txProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void parseTx(const QString &val) {   // file contents, "none" if absent, empty if the poll failed
        const QString v = val.trimmed();
        if (v.isEmpty()) { setTxEnabled(false); return; }
        setTxEnabled(true);
        bool ok = false; double g = v.toDouble(&ok);
        const int pct = (v == "none" || !ok) ? 100 : qBound(0, gainToPct(g), 150);   // bridge default is 1.0
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

    // --- direct TX (TX gate) ---
    TxLink *tx; QWidget *micRow; QProgressBar *micBar; QLabel *micDev, *txgLbl; QTimer micUi;
    bool talkNet = false;          // the current over goes through the TX gate (not Mumble)
    QString gateSvc;               // last polled state of the gate unit
    QString noteText; QElapsedTimer noteAge;
    void note(const QString &t) { noteText = t; noteAge.start(); mumbleLbl->setText(t); }
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
        const bool on = naCfg.txNet();
        micRow->setVisible(on);
        if (statusForm) statusForm->setRowVisible(txgLbl, on);
        micDev->setText(naCfg.micSource.isEmpty() ? "default input" : naCfg.micSource);
        if (tx->active()) micUi.start(); else { micUi.stop(); updateMicUi(); }
        if (!on && !tx->active()) { updateUi(); return; }
        if (tx->active()) colored(txgLbl, tx->statusText(), tx->gateKeyed() ? "#c00" : tx->ready() ? "green" : "#c60");
        else colored(txgLbl, "not connected (Connect starts it)" + (gateSvc.isEmpty() ? QString() : " · service " + gateSvc), "gray");
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
        const bool on = naCfg.enabled();
        volRowW->setVisible(!naCfg.txNet());   // Mumble's playback slider means nothing without Mumble
        netRow->setVisible(on);
        if (statusForm) statusForm->setRowVisible(netLbl, on);
        if (!on) return;
        const bool act = na->active();
        netStartBtn->setText(act ? "Stop" : "Start");
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
    QWidget *volRowW; QSlider *volSlider; QLabel *volLbl; QToolButton *muteBtn;
    QTimer volTimer, volApply;
    QProcess *volProc = nullptr;
    QList<int> volStreams;       // current Mumble sink-input indexes
    QSet<int> volSeen;           // streams already handled (saved volume applied or adopted)
    bool volFirstPoll = true, volSync = false;
    QElapsedTimer volTouched;    // last user interaction; polling won't overwrite the slider just after

    void setVolEnabled(bool on) {
        volSlider->setEnabled(on); muteBtn->setEnabled(on);
        const QString tip = on ? "Volume of Mumble's playback on this computer (radio receive audio). "
                                 "Does not change the FT-710 USB level or the master volume."
                               : "Mumble's audio stream isn't present yet (Connect first). "
                                 "The last volume is reapplied when it appears.";
        volSlider->setToolTip(tip); muteBtn->setToolTip(tip); volLbl->setToolTip(tip);
        if (!on) volLbl->setText("—");
    }
    void runPactl(const QStringList &args) {   // fire-and-forget, non-blocking
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, p, &QObject::deleteLater);
        connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) p->deleteLater(); });
        p->start("pactl", args);
    }
    void applyVolume(int pct, int onlyIdx = -1) {
        QSettings().setValue("rxVolume", pct);
        for (int idx : volStreams)
            if (onlyIdx < 0 || idx == onlyIdx)
                runPactl({"set-sink-input-volume", QString::number(idx), QString("%1%").arg(pct)});
    }
    void pollVolume() {
        if (volProc) return;
        volProc = new QProcess(this);
        connect(volProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            QByteArray out = volProc->readAllStandardOutput();
            volProc->deleteLater(); volProc = nullptr;
            if (st == QProcess::NormalExit && code == 0) parseVolume(out);
        });
        connect(volProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (volProc && volProc->state() == QProcess::NotRunning) { volProc->deleteLater(); volProc = nullptr; setVolEnabled(false); }
        });
        volProc->start("pactl", {"-f", "json", "list", "sink-inputs"});
        QTimer::singleShot(3000, volProc, [p = volProc] { if (p->state() != QProcess::NotRunning) p->kill(); });
    }
    void parseVolume(const QByteArray &json) {
        struct S { int idx, pct; bool mute; };
        QList<S> found;
        for (const QJsonValue &it : QJsonDocument::fromJson(json).array()) {
            QJsonObject o = it.toObject(), pr = o["properties"].toObject();
            const QString name = pr["application.name"].toString(), bin = pr["application.process.binary"].toString();
            if (!name.contains("mumble", Qt::CaseInsensitive) && !bin.contains("mumble", Qt::CaseInsensitive)) continue;
            QJsonObject vol = o["volume"].toObject();
            double sum = 0; int n = 0;
            for (auto c = vol.begin(); c != vol.end(); ++c) { sum += c.value().toObject()["value"].toDouble(); ++n; }
            found.append({o["index"].toInt(), n ? int(std::lround(sum / n * 100.0 / 65536.0)) : 100, o["mute"].toBool()});
        }
        std::sort(found.begin(), found.end(), [](const S &a, const S &b) { return a.idx < b.idx; });
        volStreams.clear();
        for (auto &f : found) volStreams.append(f.idx);
        const bool startup = volFirstPoll; volFirstPoll = false;
        if (found.isEmpty()) { setVolEnabled(false); return; }
        setVolEnabled(true);
        QSettings st;
        for (auto &f : found) {
            if (volSeen.contains(f.idx)) continue;
            volSeen.insert(f.idx);
            // A stream that was already playing when the app started keeps its volume (adopted).
            // A stream that appears later (Mumble (re)connected) gets the last chosen volume.
            if (!startup && st.contains("rxVolume")) {
                int want = st.value("rxVolume").toInt();
                if (want != f.pct) { applyVolume(want, f.idx); f.pct = want; }
            }
        }
        if (volTouched.isValid() && volTouched.elapsed() < 1500) return;   // user is adjusting
        if (volSlider->isSliderDown()) return;
        const S &m = found.first();
        volSync = true;
        volSlider->setValue(qBound(0, m.pct, 150));
        volLbl->setText(QString("%1%").arg(m.pct));
        muteBtn->setChecked(m.mute);
        volSync = false;
        if (st.value("rxVolume", -1).toInt() != m.pct) st.setValue("rxVolume", m.pct);   // follow external changes
    }
public:
    void setVolumeForTest(int pct) { volSlider->setValue(pct); }   // same path as dragging the slider
private:

    // Press: arm the debounce timer; a release within PTT_MIN_HOLD_MS cancels without keying.
    void pressPtt() {
        if (talking || holdTimer.isActive()) return;
        if (naCfg.txNet()) {   // direct TX through the TX gate
            if (!tx->ready()) { ptt->setDown(false); note("TX link not ready — " + (tx->active() ? tx->statusText() : QString("Connect first"))); return; }
            if (txState != "on") { ptt->setDown(false); note("TX keying is disabled (Enable TX keying first)"); return; }
            if (!tx->micUp()) { ptt->setDown(false); note("Not keying — " + tx->micProblem()); return; }
        } else if (!mumbleRunning()) { ptt->setDown(false); mumbleLbl->setText("Mumble not running — Connect first"); return; }
        ptt->setStyleSheet("background:#e90; color:black;");
        ptt->setText("hold…");
        holdTimer.start();
    }
    void startTalk() { beginTalk(); }   // immediate (used by the --ptt-test hook)
    void beginTalk() {
        holdTimer.stop();
        if (talking) return;
        if (naCfg.txNet()) {
            if (!tx->setPtt(true)) { resetPttButton(); note(tx->ready() && !tx->micUp() ? "Not keying — " + tx->micProblem() : QString("TX link not ready — not keyed")); return; }
            talking = true; talkNet = true;
            ptt->setStyleSheet("background:#c00; color:white;");
            ptt->setText("TALKING?\n(waiting for the gate)");
            updateTxUi(); adjustPollRate(); poll();
            return;
        }
        if (!mumbleRunning()) { resetPttButton(); mumbleLbl->setText("Mumble not running — Connect first"); return; }
        if (stopRetry.isActive()) {   // an earlier stop is still unacknowledged: try once more, then stop retrying
            stopRetry.stop(); stopTries = 0;
            mumbleRpc("self", {{"stoptalking", "stoptalking"}});
        }
        // Mark talking before sending: even if Mumble does not ack, it may have acted on the
        // request, so the release must always send stoptalking.
        talking = true;
        bool ack = mumbleRpc("self", {{"starttalking", "starttalking"}});
        ptt->setStyleSheet("background:#c00; color:white;");
        ptt->setText(ack ? "TALKING" : "TALKING?\n(Mumble did not ack)");
        adjustPollRate();
        poll();
    }
    void stopTalk(bool force = false) {
        if (holdTimer.isActive()) { holdTimer.stop(); resetPttButton(); return; }   // short tap: never keyed
        if (talking && talkNet) {   // direct TX: the gate drains, hangs and unkeys (and unkeys by itself if this is lost)
            talking = false; talkNet = false;
            tx->setPtt(false);
            fastPollSince.start(); adjustPollRate();
        } else if (talking) {
            talking = false;
            fastPollSince.start();
            if (mumbleRpc("self", {{"stoptalking", "stoptalking"}})) updatePttWarn(false);
            else {
                if (force) {   // quitting: retry synchronously a few times
                    for (int i = 0; i < 3 && !mumbleRpc("self", {{"stoptalking", "stoptalking"}}); ++i) waitMs(200);
                } else { stopTries = 0; stopRetry.start(); }
            }
            adjustPollRate();
        }
        resetPttButton();
    }
    void resetPttButton() {
        ptt->setDown(false);
        ptt->setStyleSheet("");
        ptt->setText("PTT\n(hold, or hold Space)");
    }
    void updatePttWarn(bool on) {
        if (on) mumbleLbl->setText("WARNING: Mumble did not acknowledge stop-talking — check Mumble / radio PTT");
        else if (mumbleLbl->text().startsWith("WARNING")) { mumbleLbl->setText(""); updateUi(); }
    }
    // Poll the status (incl. flrig PTT) every 0.5 s while talking and for 4 s after release, else 3 s.
    void adjustPollRate() {
        bool fast = talking || holdTimer.isActive() || stopRetry.isActive()
                    || (fastPollSince.isValid() && fastPollSince.elapsed() < 4000);
        int want = fast ? POLL_TALK_MS : POLL_IDLE_MS;
        if (timer.interval() != want) timer.start(want);
    }

    // Push-to-Talk via Mumble's live D-Bus API (net.sourceforge.mumble.mumble), never by editing
    // Mumble.conf, so Mumble's own saves are never clobbered. 0=Continuous 1=VAD 2=PTT.
    void ensurePttMode() {
        QDBusInterface m("net.sourceforge.mumble.mumble", "/", "net.sourceforge.mumble.Mumble");
        QDBusReply<uint> mode;
        for (int i = 0; i < 20; ++i) {
            if (m.isValid()) { mode = m.call("getTransmitMode"); if (mode.isValid()) break; }
            waitMs(250);
        }
        if (!mode.isValid()) {
            QMessageBox::information(this, "Mumble transmit mode",
                "Couldn't read Mumble's transmit mode. Please make sure Mumble is set to Push-to-Talk "
                "(Settings > Audio Input > Transmission).");
            return;
        }
        if (mode.value() == 2) return;
        if (QMessageBox::question(this, "Mumble transmit mode",
                "Mumble is not set to Push-to-Talk. In Voice Activity or Continuous mode your mic "
                "could key the FT-710 whenever TX keying is on.\n\nSwitch Mumble to Push-to-Talk now?")
            == QMessageBox::Yes) {
            QDBusMessage r = m.call("setTransmitMode", uint(2));
            if (r.type() == QDBusMessage::ErrorMessage)
                QMessageBox::warning(this, "Mumble transmit mode",
                    "Couldn't change it. Set Push-to-Talk in Mumble: Settings > Audio Input > Transmission.");
        }
    }

    // Current Mumble server URL via D-Bus ("" when not connected / not reachable).
    static QString mumbleCurrentUrl() {
        QDBusInterface m("net.sourceforge.mumble.mumble", "/", "net.sourceforge.mumble.Mumble");
        if (!m.isValid()) return {};
        QDBusReply<QString> r = m.call("getCurrentUrl");
        return r.isValid() ? r.value() : QString();
    }
    static void waitMs(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 50); }

public:
    bool isConnectedToServer() {
        if (cfg.effMumbleHost().isEmpty() || cfg.mumbleUser.isEmpty()) return false;
        QUrl cur(mumbleCurrentUrl());
        return cur.host().compare(cfg.effMumbleHost(), Qt::CaseInsensitive) == 0 && cur.userName() == cfg.mumbleUser;
    }

    // Connect Mumble to the configured server as the configured user (Settings > Mumble).
    //  * Mumble not running: start it WITH the URL (no password in argv). That suppresses Mumble's
    //    startup "Connect to server" dialog (which otherwise pops up and, if a server is picked there,
    //    replaces our session). If you saved the server as a Mumble favorite with its password,
    //    Mumble fills the password in itself.
    //  * Additionally (and when Mumble is already running) send the full URL incl. password over
    //    Mumble's local RPC socket, which connects immediately even while first-run wizards are open.
    //    The password comes from the rig host (bridge's server.pw, over ssh) or the local settings,
    //    per Settings > Mumble > Password; it never appears on a command line.
    void doConnect() {
        // Network audio mode: start the LAN stream. With direct TX (default in network mode) PTT and
        // the mic go to the TX gate and Mumble is not used at all; otherwise PTT/TX still use Mumble.
        if (naCfg.enabled() && cfg.configured()) {
            if (!na->active()) na->start();
            if (naCfg.txGate) {
                if (!tx->active()) tx->start();
                note("Network mode: RX " + naCfg.modeName() + ", TX direct via the TX gate (Mumble not used)");
                updateTxUi(); return;
            }
            if (cfg.mumbleUser.isEmpty()) { mumbleLbl->setText("Network audio started (no Mumble user set: no PTT)"); return; }
        }
        if (!cfg.configured() || cfg.mumbleUser.isEmpty()) {
            QMessageBox::information(this, "Connect", "Set the rig host and your Mumble user name in Settings first.");
            editSettings(); return;
        }
        QUrl u;
        u.setScheme("mumble"); u.setHost(cfg.effMumbleHost()); u.setPort(cfg.mumblePort);
        u.setUserName(cfg.mumbleUser); u.setPath("/" + cfg.mumbleChannel); u.setQuery("version=1.2.0");
        if (mumbleRunning() && isConnectedToServer()) { mumbleLbl->setText(QString("Already connected as %1").arg(cfg.mumbleUser)); return; }
        if (!mumbleRunning()) {
            QProcess launcher;
            launcher.setProgram("mumble");
            launcher.setArguments({u.toString(QUrl::FullyEncoded)});
            // Only use XWayland if we can actually reach it (DISPLAY + XAUTHORITY OK); otherwise a
            // Qt xcb Mumble would fail to start, so fall back to a normal (Wayland) launch.
            const bool minimized = startMinChk->isChecked() && !qEnvironmentVariableIsEmpty("DISPLAY") && x11::Conn().c;
            if (minimized) {
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert("QT_QPA_PLATFORM", "xcb");   // XWayland, so the window can be minimized
                env.remove("GDK_BACKEND");              // let Mumble's GTK theme bits pick a working backend
                launcher.setProcessEnvironment(env);
            }
            qint64 pid = 0;
            if (!launcher.startDetached(&pid)) {
                QMessageBox::warning(this, "Connect", "Could not start mumble."); return;
            }
            if (minimized) minimizeMumbleWhenShown(pid);
            mumbleLbl->setText("Starting Mumble…");
            QElapsedTimer t; t.start();
            while (!mumbleRunning() && t.elapsed() < 20000) QApplication::processEvents(QEventLoop::AllEvents, 200);
            if (!mumbleRunning()) { QMessageBox::warning(this, "Connect", "Mumble did not start (no RPC socket)."); return; }
            // Give the launch URL a moment; if it already connected we're done.
            QElapsedTimer c; c.start();
            while (!isConnectedToServer() && c.elapsed() < 4000) waitMs(250);
        }
        if (!isConnectedToServer()) {
            QString pw;
            if (cfg.pwSource == 0) {
                bool ok;
                pw = QString::fromUtf8(sshRun("cat " + cfg.remoteFile("server.pw"), &ok)).trimmed();
                if (!ok || pw.isEmpty()) { QMessageBox::warning(this, "Connect", QString("Couldn't read the server password from %1.").arg(cfg.rigName())); return; }
            } else if (cfg.pwSource == 2) pw = cfg.mumblePassword;
            QUrl full(u); if (!pw.isEmpty()) full.setPassword(pw);
            bool sent = false;
            for (int i = 0; i < 10 && !sent; ++i) { sent = mumbleRpc("url", {{"href", full.toString(QUrl::FullyEncoded)}}); if (!sent) waitMs(500); }
            pw.fill(QChar(0)); full.clear();
            if (!sent) { mumbleLbl->setText("Mumble refused the connect request"); return; }
            QElapsedTimer c; c.start();
            while (!isConnectedToServer() && c.elapsed() < 8000) waitMs(250);
        }
        mumbleLbl->setText(isConnectedToServer() ? QString("Connected as %1 @ %2").arg(cfg.mumbleUser, cfg.effMumbleHost())
                                               : QString("Connect sent — check Mumble window"));
        ensurePttMode();
    }

    // Hidden test hook (--ptt-test): key via the same code path as the PTT button, then release.
    // Hidden test hook (--minimize-test <pid>): run the start-minimized watcher on an existing process.
    void minimizeTest(qint64 pid) { minimizeMumbleWhenShown(pid); }
    void raiseTest() { raiseMumble(); }
    // Hidden test hook (--txgate-gui-test HOST PORT TOKENFILE HOLD_MS): the real PTT button path
    // against a test gate (direct connection, 700 Hz test tone, settings not saved). Prints each step.
    void txGuiTest(const QString &host, quint16 port, const QByteArray &tok, int holdMs) {
        naCfg.mode = 2; naCfg.txGate = true;          // in memory only
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
    void pttTest(int ms) {
        QTextStream err(stderr);
        err << "ptt-test: connected=" << isConnectedToServer() << "\n"; err.flush();
        startTalk();
        err << "ptt-test: talking=" << talking << "\n"; err.flush();
        waitMs(ms);
        stopTalk();
        err << "ptt-test: released, talking=" << talking << "\n"; err.flush();
    }
private:

    // Disconnect: stop talking, then (if confirmed) close Mumble the normal way. Mumble 1.4 has
    // no remote disconnect/quit (RPC or D-Bus), and when it was started minimized it sits hidden in
    // the tray, where "bring to front" can't reach it. So we send WM_DELETE_WINDOW to its main window
    // (works hidden too, under XWayland): the same as its close button, so it saves its settings.
    // Never kills it. Falls back to showing Mumble when it isn't an X11 (XWayland) window.
    void doDisconnect() {
        stopTalk(true);
        if (na->active()) na->stop(true);   // stop both network-audio units cleanly
        if (tx->active()) {   // gate unkeys on stop; radio-bridge comes back only if configured (and PTT=0, no marker)
            tx->stop(naCfg.restoreBridge, naCfg.restoreBridge, false);
            note(naCfg.restoreBridge ? "Disconnected: TX gate stopping, radio-bridge being restored" : "Disconnected (TX gate left running, radio-bridge stays stopped)");
            updateTxUi();
        }
        if (!mumbleRunning()) { if (!naCfg.txNet()) mumbleLbl->setText("Mumble is not running"); return; }
        QMessageBox mb(QMessageBox::Question, "Disconnect",
            "Stopped talking. Close Mumble now?\n\nThis disconnects from the server and quits Mumble "
            "normally (as its own Quit does), so it keeps its settings. Mumble may ask to confirm.",
            QMessageBox::NoButton, this);
        QPushButton *closeB = mb.addButton("Close Mumble", QMessageBox::AcceptRole);
        QPushButton *showB = mb.addButton("Show Mumble", QMessageBox::ActionRole);
        mb.addButton(QMessageBox::Cancel);
        mb.setDefaultButton(closeB);
        mb.exec();
        if (mb.clickedButton() == showB) { raiseMumble(); return; }
        if (mb.clickedButton() != closeB) return;
        QList<xcb_window_t> wins;
        { x11::Conn x; wins = x.closableWindows(0); for (xcb_window_t w : wins) x.requestClose(w); }
        if (wins.isEmpty()) {
            raiseMumble();
            QMessageBox::information(this, "Disconnect",
                "Couldn't reach Mumble's window (it is not running under XWayland). "
                "Use Server > Disconnect or Quit in Mumble itself.");
            return;
        }
        mumbleLbl->setText("Closing Mumble…");
        QElapsedTimer t; t.start();
        while (mumbleRunning() && t.elapsed() < 4000) waitMs(250);
        if (!mumbleRunning()) { mumbleLbl->setText("Mumble closed"); updateUi(); return; }
        // Still running: it is probably asking "close or minimize?", or is set to minimize on close.
        raiseMumble();
        mumbleLbl->setText("Mumble is still running — answer its prompt or use Quit in Mumble");
    }

    void toggleTx() {
        stopTalk();
        bool enable = txState != "on";
        if (enable && QMessageBox::question(this, "TX keying",
                naCfg.txNet() ? "Enable TX keying? The PTT button / Space in this app will key the FT-710 through the TX gate."
                              : "Enable TX keying? Anyone talking on the Mumble server will key the FT-710.") != QMessageBox::Yes)
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
        startMinChk->setChecked(cfg.startMinimized);
        if (scopeChk->isChecked()) {
            scope->stop();
            if (cfg.configured() || !cfg.scopeHost.isEmpty()) scope->start(cfg.effScopeHost(), quint16(cfg.scopePort));
        }
        poll(); pollVolume(); updateUi(); updateNetUi(); updateTxUi();
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
        auto *sshE = mk(cfg.sshHost, "e.g. rigpi, pi@192.168.1.20", "ssh destination of the computer with the FT-710, flrig, radio-bridge "
                        "and ft710-scope (an ~/.ssh/config alias, host, or user@host). Needs key-based ssh login.");
        auto *flrigE = mk(cfg.flrigHost, "127.0.0.1", "flrig XML-RPC host as seen FROM the rig host (usually 127.0.0.1).");
        auto *flrigP = port(cfg.flrigPort, "flrig XML-RPC port (flrig default 12345).");
        auto *svcE = mk(cfg.bridgeService, "radio-bridge", "systemd --user service running radio_bridge.py on the rig host.");
        auto *murE = mk(cfg.murmurService, "murmur (blank = don't check)", "System service of the Mumble server on the rig host (murmur, mumble-server). Blank if it runs elsewhere.");
        auto *dirE = mk(cfg.bridgeCfgDir, "~/.config/radio-bridge", "Bridge config dir on the rig host (tx_enabled, tx_gain, server.pw).");
        auto *buE = mk(cfg.bridgeUser, "FT710", "Mumble user name the bridge uses (RB_MUMBLE_USER); shown in the status.");
        f1->addRow("Rig host (ssh):", sshE); f1->addRow("flrig host:", flrigE); f1->addRow("flrig port:", flrigP);
        f1->addRow("Bridge service:", svcE); f1->addRow("Bridge config dir:", dirE); f1->addRow("Bridge's Mumble name:", buE);
        f1->addRow("Mumble server service:", murE);
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
        // Mumble
        auto *w2 = new QWidget; auto *f2 = new QFormLayout(w2);
        auto *mhE = mk(cfg.mumbleHost, "blank = rig host", "Mumble server address you connect to from this computer.");
        auto *mpP = port(cfg.mumblePort, "Mumble server port (default 64738).");
        auto *muE = mk(cfg.mumbleUser, "your name or callsign", "User name you join the Mumble server with.");
        auto *mcE = mk(cfg.mumbleChannel, "blank = root channel", "Channel to join, e.g. Radio or Parent/Child.");
        auto *pwSrc = new QComboBox;
        pwSrc->addItems({"Read server.pw from the rig host (ssh)", "None / Mumble's saved login", "Store here (plain text in local config)"});
        pwSrc->setCurrentIndex(qBound(0, cfg.pwSource, 2));
        auto *pwE = new QLineEdit(cfg.mumblePassword); pwE->setEchoMode(QLineEdit::Password);
        pwE->setToolTip("Stored only in this computer's config file (" + QSettings().fileName() + "), never anywhere else.");
        auto syncPw = [pwSrc, pwE] { pwE->setEnabled(pwSrc->currentIndex() == 2); };
        connect(pwSrc, &QComboBox::currentIndexChanged, &d, syncPw); syncPw();
        auto *minC = new QCheckBox("Start Mumble minimized"); minC->setChecked(cfg.startMinimized);
        f2->addRow("Server:", mhE); f2->addRow("Port:", mpP); f2->addRow("User name:", muE); f2->addRow("Channel:", mcE);
        f2->addRow("Password:", pwSrc); f2->addRow("", pwE); f2->addRow("", minC);
        tabs->addTab(w2, "Mumble");
        // Scope
        auto *w3 = new QWidget; auto *f3 = new QFormLayout(w3);
        auto *shE = mk(cfg.scopeHost, "blank = rig host", "Host running ft710-scope (as reachable from this computer).");
        auto *spP = port(cfg.scopePort, "ft710-scope TCP port (default 7355).");
        f3->addRow("Scope streamer host:", shE); f3->addRow("Scope streamer port:", spP);
        tabs->addTab(w3, "Band scope");
        // Network audio
        auto *w5 = new QWidget; auto *f5 = new QFormLayout(w5);
        auto *naMode = new QComboBox; naMode->addItems({"Mumble (default)", "LAN stream: ROC (FEC, recommended on LAN)", "LAN stream: RTP"});
        naMode->setCurrentIndex(naCfg.mode);
        naMode->setToolTip("Mumble: radio audio comes through Mumble only. LAN stream: the rig host also sends the radio's "
                           "receive audio straight to this computer with PipeWire, as the capture device \"FT-710 RX (network)\" "
                           "for WSJT-X, fldigi etc. PTT/TX: see \"PTT / TX audio\" below.");
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
        auto *naSrc = mk(naCfg.sourceNode, "blank = RB_SOURCE from the bridge's bridge.env",
                         "PipeWire capture node of the FT-710's USB audio on the rig host (pactl list short sources).");
        auto *naDest = mk(naCfg.destAddr, "blank = automatic (this computer as the rig host sees it)",
                          "IP address the rig host sends the stream to. Automatic uses the ssh connection's source address.");
        auto *naKeep = new QCheckBox("Keep streaming when FT-710 Remote closes"); naKeep->setChecked(naCfg.keepOnExit);
        f5->addRow("Audio mode:", naMode); f5->addRow("Compression:", naComp); f5->addRow("Port:", naPort);
        f5->addRow("Buffer / latency:", naLat); f5->addRow("Packet size:", naPkt); f5->addRow("ROC FEC:", naFec);
        f5->addRow("Source node (rig host):", naSrc); f5->addRow("Send to (this computer):", naDest); f5->addRow("", naKeep);
        auto *naNote = new QLabel("Needs PipeWire with pipewire-module-roc (ROC) on both computers; Opus needs GStreamer "
                                  "(pipewire-gstreamer, opus plugins) on the rig host. Pick \"FT-710 RX (network)\" as the input in WSJT-X/fldigi.");
        naNote->setWordWrap(true); f5->addRow(naNote);
        // Direct TX (no Mumble)
        auto *txPath = new QComboBox; txPath->addItems({"Direct to the TX gate (no Mumble)", "Mumble + radio-bridge"});
        txPath->setCurrentIndex(naCfg.txGate ? 0 : 1);
        txPath->setToolTip("Direct: PTT/Space and this computer's microphone go straight to the ft710-txgate service on the rig "
                           "host, which keys flrig only while you hold PTT (radio-bridge is stopped while it runs). "
                           "Mumble: the previous behaviour.");
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
        auto *txRestore = new QCheckBox("Restore radio-bridge (Mumble path) on Disconnect"); txRestore->setChecked(naCfg.restoreBridge);
        txRestore->setToolTip("On Disconnect/close: stop the TX gate and start radio-bridge again (only if the radio is not "
                              "transmitting). Switching the audio mode back to Mumble always restores it.");
        f5->addRow("PTT / TX audio:", txPath); f5->addRow("Microphone:", txMic); f5->addRow("TX gate port:", txPort);
        f5->addRow("TX gate service:", txUnit); f5->addRow("", txRestore);
        auto syncNa = [naMode, naComp, naPkt, naFec, naPort, naLat, naSrc, naDest, txPath, txMic, txPort, txUnit, txRestore] {
            const int m = naMode->currentIndex(); const bool on = m != 0;
            if (m == 1) naComp->setCurrentIndex(0);
            naComp->setEnabled(m == 2); naPkt->setEnabled(m == 2); naFec->setEnabled(m == 1);
            for (QWidget *w : {static_cast<QWidget *>(naPort), static_cast<QWidget *>(naLat), static_cast<QWidget *>(naSrc), static_cast<QWidget *>(naDest)}) w->setEnabled(on);
            txPath->setEnabled(on);
            const bool g = on && txPath->currentIndex() == 0;
            for (QWidget *w : {static_cast<QWidget *>(txMic), static_cast<QWidget *>(txPort), static_cast<QWidget *>(txUnit), static_cast<QWidget *>(txRestore)}) w->setEnabled(g);
        };
        connect(txPath, &QComboBox::currentIndexChanged, &d, syncNa);
        connect(naMode, &QComboBox::currentIndexChanged, &d, syncNa);
        connect(naComp, &QComboBox::currentIndexChanged, &d, [naComp, naPkt] {
            const int ms = naPkt->currentData().toInt();
            if (naComp->currentIndex() > 0 && ms < 20) naPkt->setCurrentIndex(naPkt->findData(20));
            if (naComp->currentIndex() == 0 && ms > 10) naPkt->setCurrentIndex(naPkt->findData(10));
        });
        syncNa();
        tabs->addTab(w5, "Network audio");
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
            n.bridgeService = svcE->text().trimmed(); n.murmurService = murE->text().trimmed();
            n.bridgeCfgDir = dirE->text().trimmed(); n.bridgeUser = buE->text().trimmed();
            n.mumbleHost = mhE->text().trimmed(); n.mumblePort = mpP->value(); n.mumbleUser = muE->text().trimmed();
            n.mumbleChannel = mcE->text().trimmed(); while (n.mumbleChannel.startsWith('/')) n.mumbleChannel.remove(0, 1);
            n.pwSource = pwSrc->currentIndex(); n.mumblePassword = n.pwSource == 2 ? pwE->text() : QString();
            n.startMinimized = minC->isChecked();
            n.scopeHost = shE->text().trimmed(); n.scopePort = spP->value();
            n.stationLabel = lblE->text().trimmed(); n.helpFile = hfE->text().trimmed();
            nn = naCfg;
            nn.mode = naMode->currentIndex();
            nn.compression = naComp->currentIndex() > 0 ? 1 : 0;
            if (naComp->currentIndex() > 0) nn.opusKbps = kb[naComp->currentIndex() - 1];
            nn.port = naPort->value(); nn.latencyMs = naLat->value(); nn.packetMs = naPkt->currentData().toInt();
            nn.fec = naFec->currentText(); nn.sourceNode = naSrc->text().trimmed(); nn.destAddr = naDest->text().trimmed();
            nn.keepOnExit = naKeep->isChecked();
            nn.txGate = txPath->currentIndex() == 0;
            nn.micSource = txMic->currentIndex() >= 0 && txMic->currentText() == txMic->itemText(txMic->currentIndex())
                               ? txMic->currentData().toString() : txMic->currentText().trimmed();
            nn.txPort = txPort->value(); nn.txUnit = txUnit->text().trimmed(); nn.restoreBridge = txRestore->isChecked();
            QString err = n.validate();
            if (err.isEmpty()) err = nn.validate();
            if (!err.isEmpty()) { QMessageBox::warning(&d, "Settings", err); return; }
            d.accept();
        });
        d.resize(560, d.sizeHint().height());
        if (d.exec() != QDialog::Accepted) return;
        const bool naWasActive = na->active();
        const QString naBefore = naCfg.describe() + naCfg.sourceNode + naCfg.destAddr + QString::number(naCfg.port);
        const QString txBefore = cfg.sshHost + "|" + cfg.bridgeService + "|" + cfg.flrigUrl() + "|" + naCfg.micSource + "|" + naCfg.txUnit + "|"
                                 + QString::number(naCfg.txPort) + "|" + naCfg.describe();
        const bool txWasActive = tx->active();
        if (txWasActive && !nn.txNet()) {   // leaving direct TX: release, stop the gate, always bring radio-bridge back
            stopTalk(true);
            tx->stop(true, true, false);
            note("Direct TX off: TX gate stopping, radio-bridge being restored");
        }
        const bool wasTxNet = naCfg.txNet();
        cfg = n; cfg.save();
        naCfg = nn; naCfg.save();
        txModeSwitched(wasTxNet);
        if (txWasActive && naCfg.txNet()) {
            const QString txAfter = cfg.sshHost + "|" + cfg.bridgeService + "|" + cfg.flrigUrl() + "|" + naCfg.micSource + "|" + naCfg.txUnit + "|"
                                    + QString::number(naCfg.txPort) + "|" + naCfg.describe();
            if (txAfter != txBefore) { stopTalk(true); tx->stop(false, false, false); tx->start(); }
        }
        // Network audio: stop it when switched back to Mumble, restart it when its settings changed.
        if (naWasActive && !naCfg.enabled()) na->stop(false);
        else if (naWasActive && naBefore != naCfg.describe() + naCfg.sourceNode + naCfg.destAddr + QString::number(naCfg.port)) na->start();
        applyConfig();
    }

    // Without Mumble nothing else plays the radio here: switching to direct TX turns Listen on;
    // switching back to Mumble TX turns it off again (Mumble plays the radio; avoid hearing it twice).
    void txModeSwitched(bool wasTxNet) {
        if (wasTxNet != naCfg.txNet() && naCfg.enabled() && netMuteBtn->isChecked() == naCfg.txNet())
            netMuteBtn->setChecked(!naCfg.txNet());
    }
public:
    // Test hook (FT710_SMOKE_TXGATE=0/1): the same TX-mode switch the Settings dialog does, then refresh.
    void testSwitchTx(bool direct) {
        const bool was = naCfg.txNet(); naCfg.txGate = direct; naCfg.save(); txModeSwitched(was); applyConfig();
    }
private:
    void showHelp() {
        QString md;
        if (!cfg.helpFile.isEmpty()) {
            QString path = cfg.helpFile; if (path.startsWith("~/")) path = QDir::home().filePath(path.mid(2));
            QFile f(path);
            md = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) + "\n\n---\n" : path + " not found.\n\n---\n";
        }
        md += "**FT-710 Remote 1.0**: Connect launches Mumble and joins the configured server "
              "(the password, if any, is handed to Mumble via its local RPC socket). Hold PTT or Space to talk; "
              "releasing, switching windows or closing the app always stops talking. "
              "**Band scope**: spectrum + waterfall from the FT-710 itself (MENU SCU-LAN10 = ON; streamed by "
              "the ft710-scope service on the rig host). The shaded band is the approximate passband, the red line "
              "the VFO. With \"Click to tune\" on, a single click sets VFO-A there via flrig (only while receiving). "
              "**Span** and **Level** set the radio's own scope span (1 kHz-1 MHz) and scope level (-30 to +30 dB, 0.5 dB "
              "steps) through flrig (CAT SS05/SS04, display settings only). With \"Auto contrast\" off the colours are fixed, "
              "so Level works like the radio's knob; turn it on to let the app rescale colours itself. "
              "**Network audio** (Settings > Network audio): instead of only hearing the radio through Mumble, the rig host can "
              "stream its receive audio straight to this computer (ROC or RTP, optional Opus) as the input device "
              "\"FT-710 RX (network)\" for WSJT-X/fldigi; Connect starts it, Disconnect stops it, and it restarts itself if the "
              "link drops. **Direct TX** (network mode, \"PTT / TX audio\" = TX gate, the default there): PTT/Space send this computer's "
              "microphone straight to the ft710-txgate service on the rig host, which keys the radio through flrig only while "
              "you hold PTT (key, ~70 ms, then audio; 0.7 s hang after release; 120 s timeout) and unkeys by itself if the link, "
              "heartbeat or audio stops. Mumble is not used; radio-bridge is stopped while the gate runs and restored on "
              "Disconnect (if enabled) or when you switch back to Mumble. \"TX level\" is applied by the gate. "
              "**Settings…** holds the rig host, flrig, Mumble server and scope addresses.";
        QDialog d(this); d.setWindowTitle("Help / About"); d.resize(640, 520);
        auto *l = new QVBoxLayout(&d); auto *tb = new QTextBrowser; tb->setMarkdown(md); l->addWidget(tb);
        auto *bb = new QDialogButtonBox(QDialogButtonBox::Close); l->addWidget(bb);
        connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        d.exec();
    }

    void poll() {
        if (pollProc) return;   // previous poll still running
        if (!cfg.configured()) { setOffline(); return; }
        if (naCfg.txNet()) updateTxUi();
        pollProc = new QProcess(this);
        QStringList args = sshArgs(log->isVisible() ? "bash -s log" : "bash -s");
        connect(pollProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            QString out = QString::fromUtf8(pollProc->readAllStandardOutput());
            pollProc->deleteLater(); pollProc = nullptr;
            if (st != QProcess::NormalExit || code == 255) { setOffline(); return; }
            parse(out);
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
        for (auto *l : {murmurLbl, bridgeLbl, connLbl, rxLbl, txEnLbl, pttLbl, vfoLbl}) colored(l, cfg.configured() ? cfg.rigName() + " unreachable" : "not configured (Settings…)", "gray");
        txState = "unknown"; radioRx = false; setTxEnabled(false); updateUi();
    }
    void parse(const QString &out) {
        QString logText;
        QMap<QString, QString> kv;
        int li = out.indexOf("---LOG---\n");
        QString head = li >= 0 ? out.left(li) : out;
        if (li >= 0) logText = out.mid(li + 10);
        for (const QString &line : head.split('\n')) {
            int eq = line.indexOf('=');
            if (eq > 0) kv[line.left(eq)] = line.mid(eq + 1);
        }
        auto act = [](QLabel *l, const QString &s) { colored(l, s.isEmpty() ? "?" : s, s == "active" ? "green" : s == "not checked" ? "gray" : "#c00"); };
        act(murmurLbl, kv["murmur"]);
        act(bridgeLbl, kv["bridge"]);
        gateSvc = kv.value("txgate");
        if (naCfg.txNet() && kv["bridge"] != "active" && (gateSvc == "active" || tx->active()))
            colored(bridgeLbl, kv["bridge"] + " (stopped for direct TX)", "gray");

        const QString c = kv["conn"];
        bool connected = c.contains("Mumble connected") || c.contains("status connected=True");
        if (kv["bridge"] != "active") colored(connLbl, "—", "gray");
        else if (c.isEmpty()) colored(connLbl, "starting…", "gray");
        else colored(connLbl, connected ? "connected as " + cfg.bridgeUser : "NOT connected", connected ? "green" : "#c00");

        QRegularExpression rx("rx_frames_sent=(\\d+)");
        auto m = rx.match(kv["status"]);
        rxLbl->setText(m.hasMatch() ? m.captured(1) + " (as of last 60 s status line)" : "no status line yet");

        txState = kv["tx"];
        if (txState == "on") colored(txEnLbl, naCfg.txNet() ? "ENABLED — PTT here keys the radio (TX gate)" : "ENABLED — Mumble audio keys the radio", "#c00");
        else colored(txEnLbl, "disabled (receive-only)", "green");

        if (!tx->ready()) showPtt(kv["ptt"].trimmed());   // with the gate link up, its 0.25 s flrig readout is used instead

        bool okv; double hz = kv["vfo"].trimmed().toDouble(&okv);
        parseTx(kv["txgain"]);
        vfoLbl->setText(okv && hz > 0 ? QString::number(hz / 1e6, 'f', 6) + " MHz" : "?");

        if (log->isVisible() && li >= 0) log->setPlainText(logText.trimmed());
        updateUi();
    }
    void updateUi() {
        bool running = mumbleRunning();
        if (naCfg.txNet()) {   // Mumble is not part of this mode
            if (!(noteAge.isValid() && noteAge.elapsed() < 8000))
                mumbleLbl->setText(tx->active() ? "Direct TX: " + tx->statusText() : "Network mode (direct TX) — Connect to start");
            disconnectBtn->setEnabled(tx->active() || na->active() || running);
            ptt->setEnabled(tx->ready() || talking);
            restartBtn->setEnabled(!tx->active());
            restartBtn->setToolTip(tx->active() ? "Disabled while the TX gate is in use (radio-bridge would stop it)." : "");
        } else {
        if ((!mumbleLbl->text().startsWith("Connect request") && !mumbleLbl->text().startsWith("WARNING")) || !running)
            mumbleLbl->setText(running ? "Mumble running" : "Mumble not running");
        disconnectBtn->setEnabled(running);
        ptt->setEnabled(running || talking);
        restartBtn->setEnabled(true); restartBtn->setToolTip("");
        }
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
    if (QString m = opt("--na-mode"); !m.isEmpty()) naCfg.mode = m == "roc" ? 1 : m == "rtp" ? 2 : 0;
    if (QString c = opt("--na-compression"); !c.isEmpty()) naCfg.compression = c == "opus" ? 1 : 0;
    if (QString b = opt("--na-bitrate"); !b.isEmpty()) naCfg.opusKbps = b.toInt();
    if (QString p = opt("--na-port"); !p.isEmpty()) naCfg.port = p.toInt();
    if (QString l = opt("--na-latency"); !l.isEmpty()) naCfg.latencyMs = l.toInt();
    if (QString p = opt("--na-packet"); !p.isEmpty()) naCfg.packetMs = p.toInt();
    const QString gainOpt = opt("--na-rx-gain");
    if (!gainOpt.isEmpty()) naCfg.rxGain = NetAudioCfg::parseGain(gainOpt);   // validate() rejects out-of-range / garbage
    else if (naCfg.effCompression() == 1 && naCfg.packetMs < 20 && !args.contains("--na-packet")) naCfg.packetMs = 20;
    const QString cmd = i + 1 < args.size() ? args[i + 1] : "status";
    if (!naCfg.enabled() && cmd != "stop") { out << "netaudio: mode is Mumble (set Settings > Network audio, or --na-mode roc|rtp)" << Qt::endl; return 2; }
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
    tx.stop(false, false, true);
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
        for (int k = 1; k < argc; ++k) if (!strcmp(argv[k], "--print-config") || !strcmp(argv[k], "--netaudio") || !strcmp(argv[k], "--txgate-test") || !strcmp(argv[k], "--print-receiver-conf") || !strcmp(argv[k], "--scope-selftest")) cli = true;
        if (cli && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    if (app.arguments().contains("--scope-selftest")) return scopeSelfTest();   // no settings, no network, no rig
    app.setApplicationName("ft710-remote");
    app.setOrganizationName("FT-710_remote");
    app.setDesktopFileName("ft710-remote");
    migrateLegacySettings();
    cfg.load();
    naCfg.load();
    if (int i = app.arguments().indexOf("--close-windows-test"); i >= 0 && i + 1 < app.arguments().size()) {   // hidden test hook
        x11::Conn x; const auto ws = x.closableWindows(app.arguments().at(i + 1).toLongLong());
        for (xcb_window_t w : ws) x.requestClose(w);
        QTextStream(stdout) << "close requested for " << ws.size() << " window(s)\n";
        return ws.isEmpty() ? 1 : 0;
    }
    if (app.arguments().contains("--print-config")) {   // non-interactive check: effective settings (no password)
        QTextStream o(stdout);
        o << "file=" << QSettings().fileName() << "\nssh=" << cfg.sshHost << "\nflrig=" << cfg.flrigUrl()
          << "\nbridge=" << cfg.bridgeService << " dir=" << cfg.bridgeCfgDir << " murmur=" << cfg.murmurService
          << "\nmumble=" << cfg.mumbleUser << "@" << cfg.effMumbleHost() << ":" << cfg.mumblePort << "/" << cfg.mumbleChannel
          << " pwSource=" << cfg.pwSource << "\nscope=" << cfg.effScopeHost() << ":" << cfg.scopePort
          << "\nlabel=" << cfg.stationLabel << " help=" << cfg.helpFile << " startMin=" << cfg.startMinimized
          << "\nnetaudio=" << naCfg.describe() << " port=" << naCfg.port << " source=" << (naCfg.sourceNode.isEmpty() ? "RB_SOURCE" : naCfg.sourceNode)
          << " dest=" << (naCfg.destAddr.isEmpty() ? "auto" : naCfg.destAddr) << " keepOnExit=" << naCfg.keepOnExit
          << " listen=" << (naCfg.listenMute ? "muted" : QString::number(naCfg.listenVol) + "%")
          << " rxGain=" << QString::number(naCfg.rxGain, 'f', 2) << "x (" << QString::number(NetAudioCfg::gainDb(naCfg.rxGain), 'f', 1) << " dB)"
          << " clickTune=" << QSettings().value("scopeClickTune", true).toBool() << " snap=" << QSettings().value("scopeSnapKHz", true).toBool()
          << " snapStep=" << QSettings().value("scopeSnapStep", 500).toInt() << " ssbOffset=" << QSettings().value("scopeSsbOffset", 1500).toInt()
          << "\ntx=" << (naCfg.txNet() ? "tx-gate" : "mumble") << " gatePort=" << naCfg.txPort << " unit=" << naCfg.txUnit
          << " mic=" << (naCfg.micSource.isEmpty() ? "default" : naCfg.micSource) << " restoreBridge=" << naCfg.restoreBridge
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
    if (args.contains("--connect") || args.contains("--ptt-test"))   // hidden test hooks
        QTimer::singleShot(500, &w, [&w, args] {
            w.doConnect();
            QTextStream(stderr) << "connect: connected=" << w.isConnectedToServer() << Qt::endl;
            if (args.contains("--ptt-test")) w.pttTest(3000);
        });
    if (int i = args.indexOf("--set-volume"); i >= 0 && i + 1 < args.size())   // hidden test hook
        QTimer::singleShot(3000, &w, [&w, pct = args[i + 1].toInt()] {
            w.setVolumeForTest(pct); QTextStream(stderr) << "set-volume: " << pct << Qt::endl; });
    if (int i = args.indexOf("--tx-test"); i >= 0) {   // hidden test hook: read back, optionally set
        QTimer::singleShot(4000, &w, [&w] {
            QTextStream(stderr) << "tx-read: enabled=" << w.txEnabledForTest() << " value=" << w.txForTest() << Qt::endl; });
        if (i + 1 < args.size() && !args[i + 1].startsWith("-"))
            QTimer::singleShot(4500, &w, [&w, pct = args[i + 1].toInt()] {
                w.setTxForTest(pct); QTextStream(stderr) << "tx-set: " << pct << Qt::endl; });
    }
    if (int i = args.indexOf("--minimize-test"); i >= 0 && i + 1 < args.size())   // hidden test hook
        QTimer::singleShot(200, &w, [&w, pid = args[i + 1].toLongLong()] { w.minimizeTest(pid); });
    if (int i = args.indexOf("--txgate-gui-test"); i >= 0 && i + 4 < args.size()) {   // hidden test hook (test gate only)
        QFile tf(args[i + 3]); if (!tf.open(QIODevice::ReadOnly)) qWarning("cannot read the token file");
        QTimer::singleShot(300, &w, [&w, h = args[i + 1], p = quint16(args[i + 2].toUInt()), t = tf.readAll().trimmed(), ms = args[i + 4].toInt()] {
            w.txGuiTest(h, p, t, ms); });
    }
    if (args.contains("--raise-test"))   // hidden test hook
        QTimer::singleShot(200, &w, [&w] { w.raiseTest(); });
    if (qEnvironmentVariableIsSet("FT710_SMOKE_TXGATE"))   // test hook: switch the TX mode like Settings does
        QTimer::singleShot(1000, &w, [&w] { w.testSwitchTx(qEnvironmentVariable("FT710_SMOKE_TXGATE") == "1"); });
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
