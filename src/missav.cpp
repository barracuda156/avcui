#include "missav.h"
#include "http.h"
#include "log.h"
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <algorithm>

using json = nlohmann::json;

namespace ytui {

// ─── Recombee backend ─────────────────────────────────────────────────────────
// MissAV's site search is served by Recombee, a hosted recommendation service.
// These are the public frontend credentials, lifted from the site's own JS and
// visible to any browser that loads the page — but they can be rotated, and if
// search suddenly 401s or 403s this constant is the first place to look.
static constexpr const char* kRecombeeHost = "client-rapi-missav.recombee.com";
static constexpr const char* kDatabaseId   = "missav-default";
static constexpr const char* kPublicToken  =
    "Ikkg568nlM51RHvldlPvc2GzZPE9R4XGzaH9Qj4zK9npbbbTly1gj9K4mgRn0QlV";

static constexpr const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 0xF]; }
    return s;
}

// Reproduces the site's _signUrl(): HMAC-SHA1 over "/{db}{path}?frontend_timestamp=N"
// keyed with the public token, appended as &frontend_sign=<hex>.
static std::string sign_path(const std::string& path) {
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(nullptr));

    std::string unsigned_path = std::string("/") + kDatabaseId + path;
    unsigned_path += (unsigned_path.find('?') != std::string::npos ? "&" : "?");
    unsigned_path += std::string("frontend_timestamp=") + ts;

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int  mac_len = 0;
    HMAC(EVP_sha1(),
         kPublicToken, (int)strlen(kPublicToken),
         (const unsigned char*)unsigned_path.data(), unsigned_path.size(),
         mac, &mac_len);

    return unsigned_path + "&frontend_sign=" + hex(mac, mac_len);
}

// Recombee wants a per-caller user id; the site generates a throwaway one.
static std::string anon_user_id() {
    static const char* d = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string s = "anon_";
    for (int i = 0; i < 16; i++) s += d[dist(rd)];
    return s;
}

// Encode a Unicode code point as UTF-8.
static void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// HTML entity decode: named plus NUMERIC forms.
//
// Numeric matters here. Titles come back with &#039; — zero-padded decimal —
// which a fixed table of "&#39;" misses entirely, and the raw entity then shows
// up in the UI. Any &#N; / &#xN; is handled rather than enumerated.
static std::string unescape(const std::string& in) {
    struct Ent { const char* name; const char* rep; };
    static const Ent ents[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&apos;", "'"}, {"&nbsp;", " "},
    };

    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        if (in[i] != '&') { out += in[i++]; continue; }

        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += in[i++]; continue; }
        std::string ent = in.substr(i, semi - i + 1);

        if (ent.size() > 3 && ent[1] == '#') {
            unsigned cp = 0;
            bool ok = false;
            if (ent[2] == 'x' || ent[2] == 'X')
                ok = sscanf(ent.c_str() + 3, "%x", &cp) == 1;
            else
                ok = sscanf(ent.c_str() + 2, "%u", &cp) == 1;
            if (ok && cp) { append_utf8(out, cp); i = semi + 1; continue; }
        }

        bool matched = false;
        for (const auto& e : ents) {
            if (ent == e.name) { out += e.rep; i = semi + 1; matched = true; break; }
        }
        // &amp; last: decoding it early would let "&amp;lt;" become "<".
        if (!matched && ent == "&amp;") { out += '&'; i = semi + 1; matched = true; }
        if (!matched) out += in[i++];
    }
    return out;
}

static std::string fmt_duration(int secs) {
    if (secs <= 0) return "";
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// Cache-key-safe id: our thumbnail cache builds a filename from this, and MissAV
// ids can carry slashes.
static std::string safe_id(const std::string& raw) {
    std::string s = raw;
    for (auto& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
    return s;
}

const char* MissAV::referer()    { return "https://missav.ws/"; }
const char* MissAV::user_agent() { return kUserAgent; }

std::vector<std::string> MissAV::mpv_header_args() {
    // One --http-header-fields-append per header: the plain --http-header-fields
    // form is a comma-separated list, and a User-Agent containing commas would
    // be split into garbage.
    return {
        std::string("--http-header-fields-append=Referer: ") + referer(),
        std::string("--http-header-fields-append=Origin: ") + kSite,
        std::string("--http-header-fields-append=User-Agent: ") + kUserAgent,
    };
}

// ─── meta tag extraction ──────────────────────────────────────────────────────
// Locate the <meta> element whose property/name is `key`, then read its
// content="". Done by bounding the tag rather than with one big regex, because
// attribute order is not guaranteed and content may itself contain '>' escapes.
std::string MissAV::meta_content(const std::string& html, const std::string& key) {
    size_t search_from = 0;
    while (true) {
        size_t k = html.find(key, search_from);
        if (k == std::string::npos) return "";
        search_from = k + key.size();

        size_t open = html.rfind("<meta", k);
        if (open == std::string::npos) continue;
        size_t close = html.find('>', k);
        if (close == std::string::npos) return "";
        // The key must belong to THIS tag, not an earlier one.
        if (close < k || open > k) continue;

        std::string tag = html.substr(open, close - open);
        size_t c = tag.find("content=");
        if (c == std::string::npos) continue;
        c += 8;
        if (c >= tag.size()) continue;
        char q = tag[c];
        if (q != '"' && q != '\'') continue;
        size_t end = tag.find(q, c + 1);
        if (end == std::string::npos) continue;
        return unescape(tag.substr(c + 1, end - c - 1));
    }
}

// ─── m3u8 unpacking ───────────────────────────────────────────────────────────
std::string MissAV::unpack_m3u8(const std::string& html) {
    // The player config is emitted as a packed JS dictionary. Everything between
    // the literal 'm3u8 and the next "video" is a pipe-separated token list
    // which, reversed, spells the manifest URL.
    size_t a = html.find("'m3u8");
    if (a == std::string::npos) return "";
    a += 5;
    size_t b = html.find("video", a);
    if (b == std::string::npos) return "";

    std::string blob = html.substr(a, b - a);

    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t p = blob.find('|', start);
        if (p == std::string::npos) { parts.push_back(blob.substr(start)); break; }
        parts.push_back(blob.substr(start, p - start));
        start = p + 1;
    }
    std::reverse(parts.begin(), parts.end());

    // [1]=scheme [2]=subdomain [3]=domain [4..8]=uuid segments.
    if (parts.size() < 9) {
        Log::write("[missav] m3u8 blob had %zu tokens, need >=9 — page layout changed?",
                   parts.size());
        return "";
    }
    for (size_t i = 1; i <= 8; i++)
        if (parts[i].empty()) {
            Log::write("[missav] m3u8 token %zu empty — page layout changed?", i);
            return "";
        }

    return parts[1] + "://" + parts[2] + "." + parts[3] + "/" +
           parts[4] + "-" + parts[5] + "-" + parts[6] + "-" +
           parts[7] + "-" + parts[8] + "/playlist.m3u8";
}

// ─── search ───────────────────────────────────────────────────────────────────
std::vector<Video> MissAV::parse_recomms(const std::string& body) {
    std::vector<Video> out;
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& e) {
        Log::write("[missav] search JSON parse failed: %s", e.what());
        return out;
    }
    if (!j.contains("recomms") || !j["recomms"].is_array()) {
        Log::write("[missav] response has no 'recomms' array");
        return out;
    }

    for (const auto& item : j["recomms"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        Video v;
        std::string id = item["id"].get<std::string>();
        v.id  = safe_id(id);
        v.url = std::string(kSite) + "/en/" + id;

        // We ask for returnProperties, so the backend MAY hand back metadata
        // here. When it does we skip the per-video fetch entirely; when it does
        // not, the fields stay empty and get_video() fills them in.
        if (item.contains("values") && item["values"].is_object()) {
            const auto& p = item["values"];
            auto str = [&](const char* k) -> std::string {
                return (p.contains(k) && p[k].is_string()) ? p[k].get<std::string>()
                                                           : std::string();
            };
            v.title         = unescape(str("title"));
            v.thumbnail_url = str("thumbnail");
            if (v.thumbnail_url.empty()) v.thumbnail_url = str("image");
            if (p.contains("duration") && p["duration"].is_number()) {
                v.duration_seconds = p["duration"].get<int>();
                v.duration = fmt_duration(v.duration_seconds);
            }
        }
        if (v.title.empty()) v.title = id;   // placeholder until detail arrives
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<Video> MissAV::search(const std::string& query, int max_results) {
    std::vector<Video> out;
    if (query.empty()) return out;

    std::string path = "/search/users/" + Http::url_encode(anon_user_id()) + "/items/";
    std::string url  = std::string("https://") + kRecombeeHost + sign_path(path);

    json body = {
        {"searchQuery",      query},
        {"count",            max_results},
        {"cascadeCreate",    true},
        {"returnProperties", true},
    };

    Http http;
    http.impersonate("chrome131");
    auto r = http.post_json(url, body.dump(), {
        "Accept: application/json",
        "Content-Type: application/json",
        std::string("Origin: ")     + kSite,
        std::string("Referer: ")    + kSite + "/",
        std::string("User-Agent: ") + kUserAgent,
    });

    Log::write("[missav] search '%s' -> HTTP %ld, %zu bytes",
               query.c_str(), r.status, r.body.size());
    if (!r.ok()) {
        if (r.status == 401 || r.status == 403)
            Log::write("[missav] auth rejected — the public Recombee token has "
                       "probably been rotated (see kPublicToken)");
        return out;
    }

    out = parse_recomms(r.body);
    Log::write("[missav] search returned %zu results", out.size());
    return out;
}

// ─── detail ───────────────────────────────────────────────────────────────────
std::optional<Video> MissAV::get_video(const std::string& url, Http* shared) {
    if (url.empty()) return std::nullopt;

    Http own;
    Http& http = shared ? *shared : own;
    if (!shared) http.impersonate("chrome131");
    auto r = http.get(url, {
        std::string("User-Agent: ") + kUserAgent,
        std::string("Referer: ")    + kSite + "/",
        "Accept: text/html,application/xhtml+xml",
    });

    if (!r.ok()) {
        Log::write("[missav] detail %s -> HTTP %ld", url.c_str(), r.status);
        return std::nullopt;
    }

    Video v;
    v.url           = url;
    v.title         = meta_content(r.body, "og:title");
    v.thumbnail_url = meta_content(r.body, "og:image");
    v.upload_date   = meta_content(r.body, "og:video:release_date");
    v.tags          = meta_content(r.body, "keywords");
    v.stream_url    = unpack_m3u8(r.body);

    std::string dur = meta_content(r.body, "og:video:duration");
    if (!dur.empty()) {
        v.duration_seconds = atoi(dur.c_str());
        v.duration = fmt_duration(v.duration_seconds);
    }

    // id from the last path segment, so thumbnail caching has a stable key.
    size_t slash = url.find_last_of('/');
    v.id = safe_id(slash == std::string::npos ? url : url.substr(slash + 1));

    if (v.title.empty()) {
        Log::write("[missav] no og:title for %s — page layout changed?", url.c_str());
        return std::nullopt;
    }
    Log::write("[missav] detail ok: '%s' dur=%s stream=%s",
               v.title.c_str(), v.duration.c_str(),
               v.stream_url.empty() ? "MISSING" : "ok");
    return v;
}

} // namespace ytui
