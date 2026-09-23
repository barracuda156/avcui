#pragma once
// ─── Native MissAV provider ───────────────────────────────────────────────────
// No Python, no yt-dlp. MissAV is the one site in this class where that is
// tractable, because its search is not scraping at all:
//
//   SEARCH  POST to MissAV's Recombee recommendation backend — a real JSON API,
//           HMAC-SHA1 signed. Returns item ids. No HTML anywhere.
//   DETAIL  One page fetch; everything we need is in <meta> tags, plus the
//           playlist URL which is packed into a JS blob (see unpack_m3u8).
//   PLAY    The extracted URL is an HLS manifest, which mpv parses natively.
//           No stream pre-resolution, so nothing goes stale (unlike the
//           yt-dlp -g path we had to remove for Pornhub).
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

    // Full metadata + the HLS manifest URL for one video page.
    // Pass an existing Http to reuse its connection across a batch — that
    // keep-alive is the main reason this is worth doing natively, and creating
    // a fresh handle per video throws it away.
    std::optional<Video> get_video(const std::string& url, class Http* http = nullptr);

    // Playable URL for mpv. Empty until get_video() has run for this entry.
    static std::string playlist_url(const Video& v) { return v.stream_url; }

    // The CDN serving the manifest and its segments (surrit.com) hotlink-
    // protects them: a bare request gets HTTP 403, even though the URL is
    // correct. mpv must send the same Referer/Origin/User-Agent we used to
    // extract it, so these come back as ready-made mpv arguments.
    //
    // Also needed for the thumbnail host (fourhoi.com), which is protected the
    // same way — see Thumbnails::set_referer().
    static std::vector<std::string> mpv_header_args();

    // Raw "Name: value" headers the stream CDN requires (Referer, Origin,
    // User-Agent). mpv_header_args() is derived from this list.
    static std::vector<std::string> http_headers();

    // Referer/User-Agent this provider's image CDN expects.
    static const char* referer();
    static const char* user_agent();

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
    // Returns "" if the blob is missing or too short — callers must treat an
    // empty result as "not playable" rather than assuming success.
    static std::string unpack_m3u8(const std::string& html);

    // Pull <meta property="..."> / <meta name="..."> content by attribute value.
    static std::string meta_content(const std::string& html, const std::string& key);

private:
    std::vector<Video> parse_recomms(const std::string& json_body);
};

} // namespace ytui
