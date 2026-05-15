#include "GitHubRelease.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace {

// JSON から `"key":"value"` の value 部分を取り出す。文字列フィールド限定。
// 入れ子・エスケープには対応しないが、GitHub API のレスポンス上は値に "
// が含まれない (URL は \u エスケープされない) ため十分。
String extractStr(const String& json, const String& key) {
  String pat = "\"" + key + "\":\"";
  int p = json.indexOf(pat);
  if (p < 0) return String();
  int start = p + pat.length();
  int end   = json.indexOf("\"", start);
  if (end < 0) return String();
  return json.substring(start, end);
}

}  // namespace

GitHubReleaseInfo GitHubRelease::fetchLatest(const String& ownerRepo,
                                             uint32_t timeoutMs) {
  GitHubReleaseInfo info;

  if (WiFi.status() != WL_CONNECTED) {
    info.error = "WiFi not connected";
    return info;
  }

  WiFiClientSecure client;
  client.setInsecure();           // 証明書検証は省略 (READ-ONLY 公開 API)
  client.setTimeout(timeoutMs / 1000);

  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setUserAgent("NTRIP-client-for-Arduino");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = "https://api.github.com/repos/" + ownerRepo + "/releases/latest";
  if (!http.begin(client, url)) {
    info.error = "http.begin failed";
    return info;
  }

  // application/vnd.github+json を明示するのが GitHub の推奨
  http.addHeader("Accept", "application/vnd.github+json");

  int code = http.GET();
  info.httpCode = code;
  if (code != 200) {
    info.error = "HTTP " + String(code);
    http.end();
    return info;
  }

  String body = http.getString();
  http.end();

  info.tagName     = extractStr(body, "tag_name");
  info.name        = extractStr(body, "name");
  info.publishedAt = extractStr(body, "published_at");
  info.htmlUrl     = extractStr(body, "html_url");

  if (info.tagName.length() == 0) {
    info.error = "tag_name not found";
    return info;
  }
  info.ok = true;
  return info;
}
