#pragma once
// ─── Background metadata enrichment ──────────────────────────────────────────
// search() is fast because it uses yt-dlp --flat-playlist, but for this
// extractor that returns only {id, title, url} — no thumbnail, duration,
// uploader or view count. Recovering those means loading each video page, about
// a second apiece, which is far too slow to block a search on.
//
// This runs that second pass in the background: the results list appears
// immediately with titles, and rows fill in as metadata arrives.
//
// DESIGN: N worker threads, each driving one yt-dlp process over a slice of the
// URLs (so yt-dlp's ~0.4s startup is paid N times, not once per video). Workers
// push completed Videos into a mutex-guarded queue; the UI thread calls drain()
// once per frame. Nothing here ever blocks the UI loop.
//
// Threads are owned and joined, never detached — a detached worker outliving
// the App is exactly the use-after-free class upstream spent 3.1.1 fixing.

#include "types.h"
#include "provider.h"
#include "log.h"
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <signal.h>

namespace ytui {

class Enricher {
public:
    ~Enricher() { stop(); }

    // Begin enriching `videos` (only entries with a URL are fetched).
    // Restarts cleanly if called while a previous run is in flight.
    // `provider` must outlive the run — App owns it for the process lifetime.
    void start(Provider* provider, const std::vector<Video>& videos,
               const std::string& cookie_args, int workers = 4) {
        stop();
        if (!provider) return;
        provider_ = provider;

        std::vector<std::string> urls;
        for (const auto& v : videos)
            if (!v.url.empty()) urls.push_back(v.url);
        if (urls.empty()) return;

        cancel_ = false;
        total_  = (int)urls.size();
        done_   = 0;
        { std::lock_guard<std::mutex> lk(mu_); ready_.clear(); pids_.clear(); }

        int n = std::max(1, std::min(workers, (int)urls.size()));
        // Deal round-robin, not in contiguous blocks: the first rows on screen
        // are the ones the user is looking at, so spreading them across workers
        // fills the visible window first instead of finishing row 1-4 while
        // rows 5-15 wait on a single process.
        std::vector<std::vector<std::string>> slices((size_t)n);
        for (size_t i = 0; i < urls.size(); i++)
            slices[i % (size_t)n].push_back(urls[i]);

        for (int w = 0; w < n; w++) {
            if (slices[(size_t)w].empty()) continue;
            running_++;
            workers_.emplace_back([this, slice = slices[(size_t)w], cookie_args]() {
                provider_->enrich(
                    slice, cookie_args,
                    [this](Video v) {
                        std::lock_guard<std::mutex> lk(mu_);
                        ready_.push_back(std::move(v));
                        done_++;
                    },
                    [this](pid_t p) {
                        std::lock_guard<std::mutex> lk(mu_);
                        pids_.push_back(p);
                    },
                    cancel_);
                running_--;
            });
        }
        Log::write("[enrich] started: %d urls across %zu workers",
                   total_, workers_.size());
    }

    // Hand back everything completed since the last call. Call once per frame.
    std::vector<Video> drain() {
        std::vector<Video> out;
        std::lock_guard<std::mutex> lk(mu_);
        out.swap(ready_);
        return out;
    }

    // Cancel and join. Kills the yt-dlp children first: a worker blocked in
    // fgets() on a page fetch would otherwise keep us here for seconds.
    void stop() {
        if (workers_.empty()) { cancel_ = false; return; }
        cancel_ = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (pid_t p : pids_) if (p > 0) kill(p, SIGKILL);
        }
        for (auto& t : workers_) if (t.joinable()) t.join();
        workers_.clear();
        { std::lock_guard<std::mutex> lk(mu_); pids_.clear(); }
        cancel_ = false;
    }

    // Keyed on live workers, not done_ < total_: yt-dlp skips entries it fails
    // to extract, so done_ can legitimately stop short of total_ and the caller
    // would show a progress line that never completes.
    bool active() const { return running_.load() > 0; }
    int  done()   const { return done_.load(); }
    int  total()  const { return total_; }

private:
    Provider*                provider_ = nullptr;   // owned by App, outlives us
    std::vector<std::thread> workers_;
    std::mutex               mu_;
    std::vector<Video>       ready_;   // guarded by mu_
    std::vector<pid_t>       pids_;    // guarded by mu_
    std::atomic<bool>        cancel_{false};
    std::atomic<int>         done_{0};
    std::atomic<int>         running_{0};   // workers still alive
    int                      total_ = 0;
};

} // namespace ytui
