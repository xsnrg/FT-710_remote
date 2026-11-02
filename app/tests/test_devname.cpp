// Unit test for audioNodeLabel (devname.h): display label for a PipeWire node, raw name as fallback.
#include "../devname.h"
#include <cstdio>
static int fails = 0;
static void eq(const char *what, const QString &got, const QString &want) {
    const bool ok = got == want; if (!ok) fails++;
    std::printf("%s %-44s got \"%s\"\n", ok ? "PASS" : "FAIL", what, qPrintable(got));
}
int main() {
    const QString anker = "alsa_input.usb-ANKER_Anker_PowerConf_S330_ACCUDP1E45301169-00.analog-stereo";
    const QJsonArray a = QJsonDocument::fromJson(R"([
      {"name":"alsa_output.usb-ANKER_Anker_PowerConf_S330_ACCUDP1E45301169-00.analog-stereo.monitor",
       "description":"Monitor of Anker PowerConf S330 Analog Stereo","properties":{}},
      {"name":"alsa_input.usb-ANKER_Anker_PowerConf_S330_ACCUDP1E45301169-00.analog-stereo",
       "description":"Anker PowerConf S330 Analog Stereo",
       "properties":{"node.nick":"Anker PowerConf S330","device.description":"Anker PowerConf S330"}},
      {"name":"nick_only","description":"","properties":{"node.nick":"Nick Mic"}},
      {"name":"devdesc_only","properties":{"device.description":"Dev Desc"}},
      {"name":"prop_desc","description":"Top","properties":{"node.description":"Prop Desc"}},
      {"name":"blank","description":"  ","properties":{}}
    ])").array();
    eq("description wins", audioNodeLabel(a, anker), "Anker PowerConf S330 Analog Stereo");
    eq("node.description before top-level", audioNodeLabel(a, "prop_desc"), "Prop Desc");
    eq("nick when no description", audioNodeLabel(a, "nick_only"), "Nick Mic");
    eq("device.description last", audioNodeLabel(a, "devdesc_only"), "Dev Desc");
    eq("blank text -> raw name", audioNodeLabel(a, "blank"), "blank");
    eq("unknown node -> raw name", audioNodeLabel(a, "not_there"), "not_there");
    eq("empty list (pactl failed) -> raw name", audioNodeLabel(QJsonArray(), anker), anker);
    std::printf("%s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
