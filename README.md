# NTRIP-client-for-Arduino

Fork from: [GLAY-AK2/NTRIP-client-for-Arduino](https://github.com/GLAY-AK2/NTRIP-client-for-Arduino)

NTRIP client library for ESP32 (Arduino). Receives RTCM3 correction data from NTRIP Caster and outputs via RS232 to GNSS receivers.

## Examples

4 combinations of hardware and WiFi configuration method:

| Example | Hardware | WiFi Config | RS232 Port |
|---------|----------|-------------|------------|
| `M5Stack_WPS` | M5Stack + RS232 Kit | WPS | Serial2 (TX=17, RX=16) |
| `M5Stack_WiFiManager` | M5Stack + RS232 Kit | WiFiManager (captive portal) | Serial2 (TX=17, RX=16) |
| `M5Atom_WPS` | M5Atom + Atomic RS232 Base | WPS | Serial2 (TX=19, RX=22) |
| `M5Atom_WiFiManager` | M5Atom + Atomic RS232 Base | WiFiManager (captive portal) | Serial2 (TX=19, RX=22) |

### Hardware

- **M5Stack + RS232 Kit**: [M5Stack RS232 Module](https://shop.m5stack.com/products/rs232-module-13-2) via I2C port. LCD display shows NTRIP status and data rate.
- **M5Atom + Atomic RS232 Base**: [Atomic RS232 Kit](https://shop.m5stack.com/collections/m5-atom/products/atom-rs232-kit). LED color indicates active NTRIP server.

![M5Atom + Atomic RS232 Base](https://user-images.githubusercontent.com/6777579/127084970-9d954f52-c155-42cb-a9d0-1e72e5324804.png)

### WiFi Configuration

- **WPS**: Press WPS button on your WiFi router. No PC/smartphone required.
- **WiFiManager**: Connect to AP "M5stack" (password: "m5stackpass") from smartphone/PC, then configure WiFi via captive portal at http://192.168.4.1/

### NTRIP Server Configuration

Edit `eniwa-agriICT.h` (WPS/M5Stack examples) or the inline config in the .ino file (M5Atom_WiFiManager) to set your NTRIP caster host, port, mount point, user, and password. Up to 4 servers can be configured; press the button to cycle between them.

## Installation

### Arduino IDE
1. Download this repository as ZIP
2. Arduino IDE → Sketch → Include Library → Add .ZIP Library
3. Open examples from File → Examples → NTRIPClient

### arduino-cli
```bash
arduino-cli lib install --git-url https://github.com/yasunorioi/NTRIP-client-for-Arduino.git
```

### Dependencies

| Library | Required by |
|---------|-------------|
| [M5Stack](https://github.com/m5stack/M5Stack) | M5Stack examples |
| [M5Atom](https://github.com/m5stack/M5Atom) | M5Atom examples |
| [WiFiManager](https://github.com/tzapu/WiFiManager) | WiFiManager examples |

## Misc

### kubota WRH1200A
WRH1200A must receive GPS + GLONASS only data. If you input other RTCM3 data (e.g. Galileo, BeiDou), it will not achieve RTK fix.

## References

- [ESP32 WPS (Qiita)](https://qiita.com/coppercele/items/6789deea453826916725)
- [M5Stack RS232 Module](https://twitter.com/M5Stack/status/1626045499437645824)

## License

LGPL-3.0 (inherited from original)
