#pragma once
// ─── Search sort and filters ──────────────────────────────────────────────────
// What a front end may ask of a provider's search, and what each provider can
// actually do. The two sites differ a lot:
//
//   Pornhub  URL parameters on its search page, which yt-dlp keeps while it
//            pages: o=mr|lg|mv|tr (sort), hd=1, min_duration / max_duration
//            (minutes, in steps of 10/20/30 only).
//   MissAV   Recombee search takes a ReQL filter over item properties
//            (is_uncensored_leak, duration, has_english_subtitle, ...). Paging
//            ("next items") keeps it. It has no sort: results are ranked by
//            relevance, and its booster did not visibly change that.
//
// The default-constructed options mean "as before": relevance, no filters.

#include <string>

namespace ytui {

struct SearchOptions {
    enum class Sort { Relevance, Newest, Longest, MostViewed, TopRated };
    enum class Uncensored { Any, Only, Exclude };

    Sort       sort = Sort::Relevance;
    Uncensored uncensored = Uncensored::Any;
    int  min_minutes = 0;          // 0 = no lower bound
    int  max_minutes = 0;          // 0 = no upper bound
    bool english_subtitles = false;
    bool hd_only = false;

    bool is_default() const {
        return sort == Sort::Relevance && uncensored == Uncensored::Any &&
               min_minutes == 0 && max_minutes == 0 && !english_subtitles && !hd_only;
    }
};

// Which of the options a provider honours; a UI disables the rest.
struct SearchCaps {
    bool sort_newest = false, sort_longest = false;
    bool sort_most_viewed = false, sort_top_rated = false;
    bool uncensored = false;
    bool duration = false;
    // Largest duration bound the site can express, in minutes (0 = any).
    // Pornhub stops at 30: "over 30 min" is its longest filter.
    int  duration_limit = 0;
    bool english_subtitles = false;
    bool hd_only = false;
};

} // namespace ytui
