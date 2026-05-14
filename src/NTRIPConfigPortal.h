#ifndef NTRIP_CONFIG_PORTAL_H
#define NTRIP_CONFIG_PORTAL_H

// 設定ポータルは ESPAsyncWebServer + AsyncTCP に依存する。
// これらは Bridge アプリだけが必要なので、ライブラリ全体の必須依存にはせず
// __has_include で gating する。Basic example のように portal を使わない
// 利用者は、これらを lib_deps に入れなくてもライブラリ本体は使える。
#if __has_include(<ESPAsyncWebServer.h>)
#define NTRIP_CONFIG_PORTAL_AVAILABLE 1
#else
#define NTRIP_CONFIG_PORTAL_AVAILABLE 0
#endif

#if NTRIP_CONFIG_PORTAL_AVAILABLE

#include <Arduino.h>
#include "NTRIPConfig.h"

/**
 * Captive-portal config flow:
 *   1. STA を切って AP モードに切替
 *   2. DNSServer で全 DNS クエリを自分の IP にすり替え (キャプティブポータル)
 *   3. ESPAsyncWebServer で `/` に設定フォーム、`/save` で POST 受信
 *   4. 保存に成功したら true で返る (呼び出し側で reboot or 再接続する)
 *   5. timeoutMs を超えると false で返る (操作なし扱い)
 *
 * 呼び出し中は loop() を止めて待機する想定 (Bridge は通信中断、設定中の
 * 数十秒は許容)。AP+STA 同時運用はしない。
 */
class NTRIPConfigPortal {
 public:
  NTRIPConfigPortal();
  ~NTRIPConfigPortal();

  // apSsid/apPassword: 設定中に立てる AP の SSID/PW (8 文字以上を推奨)。
  // timeoutMs == 0 で無制限。
  // 関数は中で AsyncWebServer を立て、保存が押されるか timeout まで block する。
  bool run(NTRIPConfig& cfg,
           const String& apSsid,
           const String& apPassword,
           uint32_t timeoutMs = 0);

  // 上で run() を呼ぶ前に WiFi がどんな状態でも、関数を抜けるときは
  // 元の STA モードに戻して切断状態にする (呼び出し側が WiFi.begin() し直す前提)。
};

#endif  // NTRIP_CONFIG_PORTAL_AVAILABLE
#endif  // NTRIP_CONFIG_PORTAL_H
