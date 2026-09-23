#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <atomic>
#include <sys/types.h>
#include "types.h"

namespace ytui {

class Pornhub {
public:
    Pornhub();
    ~Pornhub();

    // Search Pornhub, returns list of video results. `start` skips that many
    // leading results (paging): the batch is results [start, start+max_results).
    std::vector<Video> search(const std::string& query, int max_results = 20,
                              const std::string& cookie_args = "", int start = 0);

    // Browse trending/popular
    std::vector<Video> get_trending(int max_results = 20);

    // Get videos from specific category
    std::vector<Video> get_category(const std::string& category, int max_results = 20);

    // Get the direct stream URL for a video
    std::string get_stream_url(const std::string& video_url, const std::string& quality = "best");

    // Get video info (single video details)
    std::optional<Video> get_video_info(const std::string& video_url);

    // ── Metadata enrichment ──────────────────────────────────────────────────
    // search() runs yt-dlp with --flat-playlist, which for this extractor emits
    // only {id, title, url} per entry — no thumbnail, duration, uploader or view
    // count. Recovering those needs a second pass that actually loads each video
    // page, which costs roughly a second apiece.
    //
    // Fetches full metadata for `urls` in ONE yt-dlp process (its ~0.4s Python
    // startup is paid once, not per video) and calls `on_item` for each result
    // the moment its JSON line arrives — so callers can fill the UI in
    // progressively rather than waiting for the whole slice.
    //
    // Blocking: call from a worker thread. `on_spawn` receives the child pid so
    // the caller can SIGKILL it to cancel; without that, a cancel would still
    // have to wait out every remaining page fetch. Safe to call concurrently
    // (this class holds no state).
    void enrich(const std::vector<std::string>& urls,
                const std::string& cookie_args,
                const std::function<void(Video)>& on_item,
                const std::function<void(pid_t)>& on_spawn,
                const std::atomic<bool>& cancel);

    // Check if yt-dlp is available
    static bool is_available();

private:
    // Execute yt-dlp and capture stdout
    std::string exec_ytdlp(const std::vector<std::string>& args);

    // Parse a JSON object into a Video struct
    Video parse_video_json(const std::string& json_str);

    // Format view count (e.g., 1234567 -> "1.23M")
    static std::string format_views(long long views);

    // Format duration seconds to "H:MM:SS" or "M:SS"
    static std::string format_duration(int seconds);

    // Sanitize UTF-8 text
    static std::string sanitize_utf8(const std::string& s);
};

} // namespace ytui
