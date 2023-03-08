# NTRIP-client-for-Arduino
fork from:
GLAY-AK2/NTRIP-client-for-Arduino

# Add
examples/M5Atomic_ntrip
examples/M5Stack_W5500
examples/M5Atomic_ntrip-WPS

https://shop.m5stack.com/collections/m5-atom/products/atom-rs232-kit

![IMG_7711_01](https://user-images.githubusercontent.com/6777579/127084970-9d954f52-c155-42cb-a9d0-1e72e5324804.png)

#M5Stack_W5500
Serial2.begin(115200, SERIAL_8N1,16,17);

TX=17
RX=16

#M5Stamp Pico

Looks like it's working.

#Misc

-kubota WRH1200A
WRH1200A must be GPS, Glonass only data.
If you input other RTCM3 data, it will not be fixed.

-M5Stack RS232 Module
https://twitter.com/M5Stack/status/1626045499437645824

#References

esp32 WPS

https://qiita.com/coppercele/items/6789deea453826916725
