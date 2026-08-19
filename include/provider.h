#pragma once
// ─── Content provider abstraction ─────────────────────────────────────────────
// Two backends with genuinely different shapes, which is what this interface
// has to absorb:
//
//   Pornhub — yt-dlp. Search is `--flat-playlist` and returns only id/title/url,
//             so metadata needs a second pass. mpv resolves the stream itself
//             (--ytdl=yes) because the site serves HLS whose segment URLs go
//             stale if pre-resolved.
//   MissAV  — native (src/missav.cpp). Search is a signed JSON API; detail is
//             one page fetch. Yields a direct HLS manifest URL, so mpv gets it
//             with --ytdl=no and no yt-dlp is involved at all. Both its CDNs
//             hotlink-protect, so playback and thumbnails need site headers.

#include "types.h"
#include "pornhub.h"
#include "missav.h"
#include "http.h"
#include "log.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

namespace ytui {

class Provider {
public:
    virtual ~Provider() = default;

    virtual const char* name() const = 0;
    virtual std::vector<Video> search(const std::string& query, int max_results,
                                      const std::string& cookie_args) = 0;

    // True when search() already returns thumbnails/durations, making the
    // background enrichment pass unnecessary.
    virtual bool search_is_complete() const = 0;

    // Background metadata pass. Blocking — call from a worker thread.
    // on_spawn receives a child pid when the implementation forks one, so the
    // caller can kill it to cancel; implementations with no child never call it.
    virtual void enrich(const std::vector<std::string>& urls,
                        const std::string& cookie_args,
                        const std::function<void(Video)>& on_item,
                        const std::function<void(pid_t)>& on_spawn,
                        const std::atomic<bool>& cancel) = 0;

    // Make `v` playable, fetching if needed. Blocking, called at play time.
    // Returns false if no playable URL could be produced.
    virtual bool resolve_playback(Video& v) = 0;

    // What mpv should be handed, and how.
    virtual std::string play_url(const Video& v) const = 0;
    virtual bool direct_stream() const = 0;              // true -> --ytdl=no
    virtual std::vector<std::string> mpv_args() const = 0;

    // Referer this provider's image CDN expects.
    virtual const char* thumb_referer() const = 0;

    // Uploader/channel page, or "" if the provider has no such concept.
    virtual std::string channel_url(const Video& v) const = 0;

    static std::unique_ptr<Provider> make(const std::string& name);
    static bool valid(const std::string& name) {
        return name == "pornhub" || name == "missav";
    }
};

// ─── Pornhub (yt-dlp) ─────────────────────────────────────────────────────────
class PornhubProvider : public Provider {
public:
    const char* name() const override { return "pornhub"; }

    std::vector<Video> search(const std::string& q, int n,
                              const std::string& cookies) override {
        return backend_.search(q, n, cookies);
    }
    bool search_is_complete() const override { return false; }   // flat stubs only

    void enrich(const std::vector<std::string>& urls, const std::string& cookies,
                const std::function<void(Video)>& on_item,
                const std::function<void(pid_t)>& on_spawn,
                const std::atomic<bool>& cancel) override {
        backend_.enrich(urls, cookies, on_item, on_spawn, cancel);
    }

    // Nothing to do: mpv drives yt-dlp just-in-time.
    bool resolve_playback(Video& v) override { return !v.url.empty(); }
    std::string play_url(const Video& v) const override { return v.url; }
    bool direct_stream() const override { return false; }
    std::vector<std::string> mpv_args() const override { return {}; }
    const char* thumb_referer() const override { return "https://www.pornhub.com/"; }

    std::string channel_url(const Video& v) const override {
        // The extractor gives an uploader name, not an opaque channel id.
        return v.channel.empty() ? "" : "https://www.pornhub.com/users/" + v.channel;
    }

private:
    Pornhub backend_;
};

// ─── MissAV (native) ──────────────────────────────────────────────────────────
class MissavProvider : public Provider {
public:
    const char* name() const override { return "missav"; }

    std::vector<Video> search(const std::string& q, int n,
                              const std::string&) override {
        auto r = backend_.search(q, n);
        // Recombee is asked for item properties; whether it returns them is its
        // decision, so this is measured per search rather than assumed.
        complete_ = !r.empty();
        for (const auto& v : r)
            if (v.thumbnail_url.empty()) { complete_ = false; break; }
        return r;
    }
    bool search_is_complete() const override { return complete_; }

    // No child process: each item is one HTTPS GET on a keep-alive handle.
    // Cancellation is checked between items, bounded by the 30s HTTP timeout.
    void enrich(const std::vector<std::string>& urls, const std::string&,
                const std::function<void(Video)>& on_item,
                const std::function<void(pid_t)>&,
                const std::atomic<bool>& cancel) override {
        // One Http for the whole slice: the connection to missav.ws is reused
        // across every item, which is the entire point of not shelling out.
        Http http;
        http.impersonate("chrome131");
        for (const auto& u : urls) {
            if (cancel.load()) return;
            auto v = backend_.get_video(u, &http);
            if (v && on_item) on_item(*v);
        }
    }

    bool resolve_playback(Video& v) override {
        if (!v.stream_url.empty()) return true;   // enrichment already got it

        // Only reached when the entry was never enriched — played straight from
        // a fresh search, or replayed from history/a playlist (which store the
        // page URL only).
        //
        // Retried, because a single attempt is not reliable here: missav rate
        // limits, and the enrichment workers may be mid-sweep against the same
        // host. A transient 403 must not surface to the user as a hard failure.
        // Runs on the UI thread, so the timeout is shortened and the attempts
        // bounded — worst case is ~25s, and the common failure (an immediate
        // 403) retries in well under a second.
        if (!resolve_http_) {
            resolve_http_ = std::make_unique<Http>();
            resolve_http_->impersonate("chrome131");
            resolve_http_->set_timeout(10);
        }
        for (int attempt = 1; attempt <= 3; attempt++) {
            if (attempt > 1) usleep(300000 * (useconds_t)(attempt - 1));
            auto full = backend_.get_video(v.url, resolve_http_.get());
            if (full && !full->stream_url.empty()) {
                v.stream_url = full->stream_url;
                if (v.thumbnail_url.empty()) v.thumbnail_url = full->thumbnail_url;
                if (v.duration.empty())      v.duration      = full->duration;
                if (attempt > 1)
                    Log::write("[missav] resolve succeeded on attempt %d", attempt);
                return true;
            }
            Log::write("[missav] resolve attempt %d/3 failed for %s",
                       attempt, v.url.c_str());
        }
        return false;
    }

    std::string play_url(const Video& v) const override { return v.stream_url; }
    bool direct_stream() const override { return true; }
    std::vector<std::string> mpv_args() const override { return MissAV::mpv_header_args(); }
    const char* thumb_referer() const override { return MissAV::referer(); }
    std::string channel_url(const Video&) const override { return ""; }

private:
    MissAV backend_;
    bool   complete_ = false;
    // Keep-alive handle for the interactive resolve path. UI-thread only —
    // enrich() runs on workers and builds its own, so this is never shared.
    std::unique_ptr<Http> resolve_http_;
};

inline std::unique_ptr<Provider> Provider::make(const std::string& name) {
    if (name == "missav") return std::make_unique<MissavProvider>();
    return std::make_unique<PornhubProvider>();
}

} // namespace ytui
