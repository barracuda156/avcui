#pragma once
// ─── Loopback HLS proxy ───────────────────────────────────────────────────────
// Some stream CDNs (MissAV's surrit.com) sit behind Cloudflare rules that
// reject any client whose TLS fingerprint is not a browser's. Headers do not
// help: mpv and ffmpeg get HTTP 403 however faithfully they copy Referer and
// User-Agent, because the check happens in the TLS handshake.
//
// So the player is pointed at this proxy instead. It listens on 127.0.0.1 only,
// fetches each request upstream through an Http that presents a browser
// fingerprint (curl-impersonate), and hands the bytes back. The player needs no
// headers and no special options; the same URL works for mpv and for avgui's
// ffmpeg source.
//
// Playlists are rewritten on the way through:
//   - absolute segment/variant URLs are pointed back at the proxy, so nothing
//     escapes to a direct fetch the CDN would refuse;
//   - segment names with a non-media extension (surrit serves MPEG-TS as
//     "video0.jpeg") get a "~.ts" suffix, because newer ffmpeg HLS demuxers
//     refuse segments whose extension is not a known media type. The proxy
//     strips the suffix again before fetching.
//
// Only hosts registered through wrap() are proxied; anything else is refused,
// so local processes cannot use it as an open relay.

#include <string>
#include <vector>

namespace ytui {

class HlsProxy {
public:
    // Return a loopback URL that serves `url` (and every playlist/segment it
    // references on the same host) fetched with `headers`, impersonating
    // `impersonate` ("chrome131"). Starts the proxy on first use.
    // Returns `url` unchanged if the proxy cannot be started.
    static std::string wrap(const std::string& url,
                            const std::vector<std::string>& headers,
                            const std::string& impersonate);

    // Given a master playlist URL (typically one wrap() returned), fetch it and
    // return the absolute URL of the rendition with the greatest height not
    // above `max_height` — or the smallest one if all are above it. Returns
    // `master_url` itself if it is not a master playlist or cannot be read.
    // Blocking: one HTTP round trip.
    //
    // Handing a player one rendition rather than the master both honours a
    // resolution setting and avoids ffmpeg demuxing every rendition at once.
    static std::string pick_variant(const std::string& master_url, int max_height);
};

} // namespace ytui
