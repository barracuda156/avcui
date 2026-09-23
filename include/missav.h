#pragma once
// ─── Native MissAV provider ───────────────────────────────────────────────────
// No Python, no yt-dlp. MissAV is the one site in this class where that is
// tractable, because its search is not scraping at all:
//
//   SEARCH  POST to MissAV's Recombee recommendation backend — a real JSON API,
//           HMAC-SHA1 signed. Returns item ids. No HTML anywhere.
//           Cover art is at a fixed CDN path derived from the id, so search
//           results carry thumbnails without any page fetch.
//   DETAIL  One page fetch; everything we need is in <meta> tags, plus the
//           playlist URL which is packed into a JS blob (see unpack_m3u8).
//           missav.ws itself Cloudflare-challenges non-browsers, so pages are
//           fetched from mirror domains when it refuses (see kMirrors).
//   PLAY    The extracted URL is an HLS manifest. Its CDN checks the TLS
//           fingerprint, so players read it through HlsProxy.
//
// Ported from EchterAlsFake's unofficial-api-for-missav (AGPL-3.0) — the
// protocol details, not the code. Deliberately reimplemented so avcui gains no
// AGPL obligation and no Python dependency chain.
//
// BUILD: needs libcurl (or curl-impersonate) and OpenSSL libcrypto for HMAC.

#include "types.h"
#include <string>
#include <vector>
#include <optional>

namespace ytui {

class MissAV {
public:
    // Search. One HTTPS round-trip; no per-video fetch unless the backend
    // omitted properties (see `complete` on the result).
    std::vector<Video> search(const std::string& query, int max_results = 20);

    // The next `count` results of the last search(). Recombee pages through a
    // result set by its recommId ("recommend next items"), not by offset, so
    // this only works as a continuation. Empty when exhausted.
    std::vector<Video> search_more(int count);

    // Full metadata + the HLS manifest URL for one video page.
    // Pass an existing Http to reuse its connection across a batch — that
    // keep-alive is the main reason this is worth doing natively, and creating
    // a fresh handle per video throws it away.
    std::optional<Video> get_video(const std::string& url, class Http* http = nullptr);

    // Playable URL for mpv. Empty until get_video() has run for this entry.
    static std::string playlist_url(const Video& v) { return v.stream_url; }

    // The CDN serving the manifest and its segments (surrit.com) wants the
    // Referer/Origin/User-Agent we used to extract it AND a browser TLS
    // fingerprint; a request missing either gets HTTP 403. Headers alone are
    // not enough for mpv, which is why playback goes through HlsProxy — these
    // args remain for the --missav-test hint.
    static std::vector<std::string> mpv_header_args();

    // Raw "Name: value" headers the stream CDN requires (Referer, Origin,
    // User-Agent). mpv_header_args() is derived from this list.
    static std::vector<std::string> http_headers();

    // Referer/User-Agent this provider's image CDN expects.
    static const char* referer();
    static const char* user_agent();

    // curl-impersonate target every MissAV request presents. Without
    // curl-impersonate linked, page fetches and the stream CDN both 403.
    static const char* impersonate_target();

    // No external binary is required, unlike the yt-dlp providers.
    static bool is_available() { return true; }

    // Site root; also the Referer/Origin the CDN expects.
    static constexpr const char* kSite = "https://missav.ws";

    // ── exposed for the --missav-test harness ────────────────────────────────
    // Rebuild the HLS manifest URL from the packed JS blob on a video page.
    //
    // The page carries an obfuscated dictionary whose pipe-separated tokens,
    // REVERSED, spell out the URL: [1]=scheme, [2]=subdomain, [3]=domain,
    // [4..8]=the five uuid segments. Yields:
    //     <scheme>://<sub>.<domain>/<a>-<b>-<c>-<d>-<e>/playlist.m3u8
    // Falls back to a literal playlist URL or a bare surrit uuid on the page.
    // Returns "" if none is found — callers must treat an empty result as
    // "not playable" rather than assuming success.
    static std::string unpack_m3u8(const std::string& html);

    // Pull <meta property="..."> / <meta name="..."> content by attribute value.
    static std::string meta_content(const std::string& html, const std::string& key);

private:
    static std::string unpack_packed_m3u8(const std::string& html);
    std::vector<Video> parse_recomms(const std::string& json_body);
    // Signed POST to Recombee; returns the parsed results and records the
    // recommId for search_more().
    std::vector<Video> recombee(const std::string& path, const std::string& body,
                                const std::string& what);

    std::string last_recomm_id_;
};

} // namespace ytui
