#ifndef NTRIP_CONFIG_H
#define NTRIP_CONFIG_H

#include <Arduino.h>

// 各文字列フィールドの最大長。NVS の "string" タイプに格納する。
// Preferences は最大値が大きいので余裕を持たせている。
constexpr size_t NTRIP_CFG_MAX_HOST       = 64;
constexpr size_t NTRIP_CFG_MAX_MOUNTPOINT = 32;
constexpr size_t NTRIP_CFG_MAX_USER       = 32;
constexpr size_t NTRIP_CFG_MAX_PASSWD     = 32;

/**
 * Runtime-configurable NTRIP client settings.
 *
 * Stored under NVS namespace "ntrip". Values are loaded once at boot and
 * mutated only via Web UI → save() → reboot/reconnect.
 */
struct NTRIPConfig {
  String   host;             // NTRIP caster hostname (no scheme)
  uint16_t port             = 2101;
  String   mountpoint;
  String   user;             // empty if anonymous
  String   passwd;

  // VRS (Virtual Reference Station) options. Used only by Bridge variants
  // and only if the underlying NTRIPClient is wired to send GGA upstream.
  bool     vrsEnabled       = false;
  uint16_t ggaIntervalSec   = 10;     // GGA uplink interval
  bool     useReceiverGGA   = true;   // true: forward GGA from connected
                                      //   GNSS receiver; false: synthesize
                                      //   from manualLat/Lon below.
  double   manualLat        = 0.0;    // used when !useReceiverGGA
  double   manualLon        = 0.0;
  float    manualAltMeters  = 0.0f;

  // Returns false if any "required" field is empty / unusable. Used by
  // setup() to decide whether to force the config portal on first boot.
  bool isComplete() const;

  // Load from NVS. Returns true if any value was found, false if the
  // namespace was empty (fresh device).
  bool load();

  // Persist to NVS. Returns false on write failure.
  bool save() const;

  // Wipe NVS namespace. Used by the "factory reset" button on the portal.
  static void clear();
};

#endif  // NTRIP_CONFIG_H
