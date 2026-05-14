#include "NTRIPConfig.h"
#include <Preferences.h>

namespace {
constexpr const char* NS = "ntrip";
}

bool NTRIPConfig::isComplete() const {
  // host と mountpoint だけ必須。user/passwd は anonymous mountpoint なら空でよい。
  return host.length() > 0 && mountpoint.length() > 0 && port > 0;
}

bool NTRIPConfig::load() {
  Preferences prefs;
  if (!prefs.begin(NS, true)) {
    return false;
  }
  // 何も書かれていない名前空間の検出: 主要フィールドが全て isKey 偽なら新規。
  bool any = prefs.isKey("host") || prefs.isKey("mntpnt");

  host            = prefs.getString("host", "");
  port            = prefs.getUShort("port", 2101);
  mountpoint      = prefs.getString("mntpnt", "");
  user            = prefs.getString("user", "");
  passwd          = prefs.getString("passwd", "");

  vrsEnabled      = prefs.getBool("vrs", false);
  ggaIntervalSec  = prefs.getUShort("ggaSec", 10);
  useReceiverGGA  = prefs.getBool("ggaRx", true);
  manualLat       = prefs.getDouble("lat", 0.0);
  manualLon       = prefs.getDouble("lon", 0.0);
  manualAltMeters = prefs.getFloat("alt", 0.0f);

  prefs.end();
  return any;
}

bool NTRIPConfig::save() const {
  Preferences prefs;
  if (!prefs.begin(NS, false)) {
    return false;
  }
  prefs.putString("host",   host);
  prefs.putUShort("port",   port);
  prefs.putString("mntpnt", mountpoint);
  prefs.putString("user",   user);
  prefs.putString("passwd", passwd);

  prefs.putBool  ("vrs",    vrsEnabled);
  prefs.putUShort("ggaSec", ggaIntervalSec);
  prefs.putBool  ("ggaRx",  useReceiverGGA);
  prefs.putDouble("lat",    manualLat);
  prefs.putDouble("lon",    manualLon);
  prefs.putFloat ("alt",    manualAltMeters);

  prefs.end();
  return true;
}

void NTRIPConfig::clear() {
  Preferences prefs;
  if (prefs.begin(NS, false)) {
    prefs.clear();
    prefs.end();
  }
}
