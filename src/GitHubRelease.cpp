#include "GitHubRelease.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace {

// JSON から `"key":"value"` の value 部分を取り出す。escape 対応版。
// release notes (body) は \n や \" を含むため、ナイーブな indexOf("\"") では
// 終了位置を取り違える。ここではエスケープを正しくスキップし、最後に \\n
// → \n、\\r → \r、\\" → "、\\\\ → \\ にデコードする。\u はサポート外
// (出てきたらそのまま残す)。
String extractStr(const String& json, const String& key) {
  String pat = "\"" + key + "\":\"";
  int p = json.indexOf(pat);
  if (p < 0) return String();
  int i = p + pat.length();

  String out;
  out.reserve(64);
  while (i < (int)json.length()) {
    char c = json.charAt(i);
    if (c == '\\') {
      if (i + 1 >= (int)json.length()) break;
      char esc = json.charAt(i + 1);
      switch (esc) {
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        default:
          // 未対応: \uXXXX 等はそのまま落とす (簡易実装)
          break;
      }
      i += 2;
    } else if (c == '"') {
      // 終端
      return out;
    } else {
      out += c;
      i++;
    }
  }
  return out;  // 終端 " 見つからずに終わった場合 (壊れた JSON)
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
  info.body        = extractStr(body, "body");

  if (info.tagName.length() == 0) {
    info.error = "tag_name not found";
    return info;
  }
  info.ok = true;
  return info;
}
