// Short, human-readable names for PipeWire/PulseAudio audio nodes, for display only.
// The raw node name stays the stored/selected value; this just maps it to a label.
#pragma once
#include <QString>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QProcess>

// Pick a label for node `name` from `pactl -f json list sources` (or sinks) output.
// Order: node.description, top-level "description", node.nick, device.description, raw name.
// Returns the raw name if the node is not in the list or has no usable text.
inline QString audioNodeLabel(const QJsonArray &nodes, const QString &name) {
    for (const QJsonValue &v : nodes) {
        const QJsonObject o = v.toObject();
        if (o["name"].toString() != name) continue;
        const QJsonObject p = o["properties"].toObject();
        for (const QString &s : {p["node.description"].toString(), o["description"].toString(),
                                 p["node.nick"].toString(), p["device.description"].toString()}) {
            const QString t = s.trimmed();
            if (!t.isEmpty() && !t.startsWith("Monitor of ")) return t;
        }
        break;
    }
    return name;
}

// Cached lookup for the main window: one `pactl` call per unseen node name (never per UI tick).
// A miss (node absent / pactl failed) is cached too, and retried at most every 30 s.
class AudioNodeNames {
public:
    QString label(const QString &name) {
        if (name.isEmpty()) return name;
        auto it = cache.constFind(name);
        if (it != cache.constEnd() && (it->found || it->age.elapsed() < 30000)) return it->label;
        Entry e; e.age.start();
        QProcess p; p.start("pactl", {"-f", "json", "list", "sources"});
        if (p.waitForFinished(1500) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            e.label = audioNodeLabel(QJsonDocument::fromJson(p.readAllStandardOutput()).array(), name);
            e.found = e.label != name;
        } else { p.kill(); p.waitForFinished(200); }
        if (e.label.isEmpty()) e.label = name;
        cache.insert(name, e);
        return e.label;
    }
private:
    struct Entry { QString label; bool found = false; QElapsedTimer age; };
    QHash<QString, Entry> cache;
};
