#ifndef GITHUB_RELEASE_H
#define GITHUB_RELEASE_H

#include <Arduino.h>

/**
 * GitHub Releases の最新版情報を取得する小ヘルパー。
 *
 * 単体での比較用途 (現バージョンとの照合) を想定し、JSON はフィールド名を
 * 直接 substring 検索する軽量実装にしてある (ArduinoJson 等の依存なし)。
 *
 * GitHub API は無認証で 60 req/hour/IP の制限。ボタン手動トリガー想定なら
 * 余裕。証明書検証は WiFiClientSecure の setInsecure() で省略 (公開
 * READ-ONLY API なので MITM リスクは情報の上書きに限定される)。
 */
struct GitHubReleaseInfo {
  bool   ok            = false;     // false なら以下フィールドは未保証
  int    httpCode      = 0;
  String tagName;                   // 例: "v0.4.0"
  String name;                      // リリースタイトル (タグ自体と同じことが多い)
  String publishedAt;               // ISO8601 (例: "2026-05-15T12:34:56Z")
  String htmlUrl;                   // ブラウザ閲覧URL
  String body;                      // release notes (\n改行に正規化済み)
  String error;                     // ok == false のとき原因メモ
};

class GitHubRelease {
 public:
  // owner/repo は "owner/repo" 形式 (例: "yasunorioi/NTRIP-client-for-Arduino")
  // timeoutMs は HTTP の総 timeout
  static GitHubReleaseInfo fetchLatest(const String& ownerRepo,
                                       uint32_t timeoutMs = 8000);
};

#endif  // GITHUB_RELEASE_H
