#include "app.h"
#include "log.h"
#include "player.h"
#include "pornhub.h"
#include "types.h"
#include "theme.h"
#include "config.h"
#include "compat.h"
#include "thumbs.h"
#include "termcaps.h"
#include "missav.h"
#include "http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <locale.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

static ytui::App* g_app = nullptr;
static std::atomic<bool> g_update_available{false};
static std::string g_remote_version;

// ─── ANSI helpers (only when stdout is a TTY) ─────────────────────────────────
struct Ansi {
    const char* BOLD;
    const char* CYAN;
    const char* GREEN;
    const char* YELLOW;
    const char* RED;
    const char* PURPLE;
    const char* DIM;
    const char* RESET;

    explicit Ansi(bool use_color) {
        if (use_color) {
            BOLD   = "\033[1m";
            CYAN   = "\033[36m";
            GREEN  = "\033[32m";
            YELLOW = "\033[33m";
            RED    = "\033[31m";
            PURPLE = "\033[35m";
            DIM    = "\033[2m";
            RESET  = "\033[0m";
        } else {
            BOLD = CYAN = GREEN = YELLOW = RED = PURPLE = DIM = RESET = "";
        }
    }
};

// ─── Background update check ──────────────────────────────────────────────────

[[maybe_unused]] static int compare_versions(const std::string& v1, const std::string& v2) {
    int maj1=0, min1=0, pat1=0, maj2=0, min2=0, pat2=0;
    sscanf(v1.c_str(), "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2.c_str(), "%d.%d.%d", &maj2, &min2, &pat2);
    if (maj1 != maj2) return (maj1 > maj2) ? 1 : -1;
    if (min1 != min2) return (min1 > min2) ? 1 : -1;
    if (pat1 != pat2) return (pat1 > pat2) ? 1 : -1;
    return 0;
}

// Release-feed URL for the update check. avcui has no published release feed —
// it is packaged locally — and pointing this at upstream ytcui would compare our
// version against theirs and nag on every launch. Define AVCUI_VERSION_URL at
// build time to enable the check against a real feed.
#ifdef AVCUI_VERSION_URL
static void check_for_updates_async() {
    FILE* pipe = popen("curl -fsSL --max-time 2 "
                       AVCUI_VERSION_URL
                       " 2>/dev/null | tr -d '[:space:]'", "r");
    if (!pipe) return;
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), pipe)) {
        std::string remote(buf);
        if (!remote.empty() && remote[0] >= '0' && remote[0] <= '9') {
            if (compare_versions(remote, ytui::VERSION) > 0) {
                g_remote_version = remote;
                g_update_available = true;
            }
        }
    }
    pclose(pipe);
}
#else
static void check_for_updates_async() {}
#endif

// ─── Signal handler ───────────────────────────────────────────────────────────

static void signal_handler(int sig) {
    if (g_app) g_app->force_cleanup();
    fprintf(stderr, "\nCaught signal %d, cleaning up...\n", sig);
    // _exit, not exit: terminate immediately without running static destructors
    // (curl_global_cleanup / singleton teardown) while prefetch/update threads
    // may still be alive. exit() is also not async-signal-safe; _exit() is.
    _exit(1);
}

// ─── --diag: deep system diagnostic dump ──────────────────────────────────────
//
// Prints everything we need to debug macOS/Linux/BSD compat issues.
// Does NOT start the TUI. Exits after printing.

static void run_diag(const Ansi& C) {
    printf("\n%s%s╭──────────────────────────────────────────────╮%s\n",
           C.BOLD, C.CYAN, C.RESET);
    printf("%s%s│  avcui v%s — System Diagnostic              │%s\n",
           C.BOLD, C.CYAN, ytui::VERSION, C.RESET);
    printf("%s%s╰──────────────────────────────────────────────╯%s\n\n",
           C.BOLD, C.CYAN, C.RESET);

    // ── Platform / compat layer ───────────────────────────────────────────
    printf("%s%s[COMPAT LAYER]%s\n", C.BOLD, C.YELLOW, C.RESET);
    ytui::compat::dump_platform_info(STDOUT_FILENO);
    printf("\n");

    // ── Terminal / graphics ───────────────────────────────────────────────
    printf("%s%s[TERMINAL / GRAPHICS]%s\n", C.BOLD, C.YELLOW, C.RESET);
    {
        ytui::TermCaps::detect_for_diag();
        printf("%s\n", ytui::TermCaps::get().summary().c_str());
        const auto& tc = ytui::TermCaps::get();
        int bg = tc.best_graphics();
        const char* bgn = bg == 3 ? "kitty" : bg == 2 ? "sixel" : bg == 4 ? "iterm" : "none (block art)";
        printf("  best raster  : %s   (enable explicitly with --gfx)\n", bgn);
        printf("  chafa        : %s\n",
               ytui::Thumbnails::renderer_available() ? "found" : "MISSING (thumbnails off)");
        printf("\n");
    }

    // ── macOS version (if applicable) ─────────────────────────────────────
#if defined(YTUI_MACOS)
    {
        int mv = ytui::compat::macos_major_version();
        printf("%s%s[macOS VERSION]%s\n", C.BOLD, C.YELLOW, C.RESET);
        printf("  kern.osproductversion major : %d\n", mv);
        if (mv < 11) {
            printf("  %s%sWARNING%s: macOS < 11 — EVFILT_PROC would fail (using pipe watchdog)\n",
                   C.BOLD, C.RED, C.RESET);
        } else {
            printf("  %sOK%s: macOS %d (pipe watchdog active; kqueue option exists but unused)\n",
                   C.GREEN, C.RESET, mv);
        }
        printf("\n");
    }
#endif

    // ── Binary dependencies ───────────────────────────────────────────────
    printf("%s%s[DEPENDENCIES]%s\n", C.BOLD, C.YELLOW, C.RESET);

    struct Dep { const char* name; const char* check_cmd; bool required; };
    Dep deps[] = {
        { "yt-dlp",   "which yt-dlp",       true  },
        { "mpv",      "which mpv",           true  },
        { "curl",     "which curl",          false },
        { "chafa",    "which chafa",         false },
#if defined(YTUI_MACOS)
        { "pbcopy",   "which pbcopy",        true  },
        { "open",     "which open",          true  },
#else
        { "xdg-open", "which xdg-open",     false },
        { "wl-copy",  "which wl-copy",      false },
        { "xclip",    "which xclip",        false },
#endif
    };
    printf("  %sBackend:%s yt-dlp\n\n", C.DIM, C.RESET);

    for (auto& d : deps) {
        std::string cmd = std::string(d.check_cmd) + " > /dev/null 2>&1";
        bool found = (system(cmd.c_str()) == 0);
        if (found) {
            printf("  %s✓%s  %-12s", C.GREEN, C.RESET, d.name);
            // Print version if available
            std::string vcmd = std::string(d.check_cmd) +
                               " | xargs -I{} {} --version 2>/dev/null | head -1";
#if defined(YTUI_MACOS)
            if (strcmp(d.name, "yt-dlp") == 0)
                vcmd = "yt-dlp --version 2>/dev/null";
            else if (strcmp(d.name, "mpv") == 0)
                vcmd = "mpv --version 2>/dev/null | head -1";
#else
            if (strcmp(d.name, "yt-dlp") == 0)
                vcmd = "yt-dlp --version 2>/dev/null";
            else if (strcmp(d.name, "mpv") == 0)
                vcmd = "mpv --version 2>/dev/null | head -1";
#endif
            FILE* vp = popen(vcmd.c_str(), "r");
            char vbuf[128] = {0};
            if (vp) { char* _r = fgets(vbuf, sizeof(vbuf), vp); (void)_r; pclose(vp); }
            // Trim newline
            size_t vlen = strlen(vbuf);
            if (vlen > 0 && vbuf[vlen-1] == '\n') vbuf[vlen-1] = '\0';
            if (strlen(vbuf) > 0) printf(" %s%s%s", C.DIM, vbuf, C.RESET);
            printf("\n");
        } else {
            if (d.required)
                printf("  %s✗%s  %-12s %s[MISSING — REQUIRED]%s\n",
                       C.RED, C.RESET, d.name, C.RED, C.RESET);
            else
                printf("  %s-%s  %-12s %s[not found — optional]%s\n",
                       C.DIM, C.RESET, d.name, C.DIM, C.RESET);
        }
    }
    printf("\n");

    // ── Config / paths ────────────────────────────────────────────────────
    printf("%s%s[PATHS]%s\n", C.BOLD, C.YELLOW, C.RESET);
    std::string config_dir = ytui::Config::config_dir();
    std::string config_file = config_dir + "/config.json";
    std::string log_dir = ytui::Log::get_log_dir();
    std::string log_file = ytui::Log::get_log_path();

    auto path_status = [&](const char* label, const std::string& path, bool is_file) {
        struct stat st;
        bool exists = (stat(path.c_str(), &st) == 0);
        bool is_right_type = exists && (is_file ? S_ISREG(st.st_mode) : S_ISDIR(st.st_mode));
        printf("  %-20s %s%s%s%s\n",
               label,
               is_right_type ? C.GREEN : C.DIM,
               path.c_str(),
               exists ? (is_right_type ? " [exists]" : " [wrong type]") : " [not found]",
               C.RESET);
    };

    path_status("config dir",   config_dir,  false);
    path_status("config.json",  config_file, true);
    path_status("log dir",      log_dir,     false);
    path_status("debug.log",    log_file,    true);
    printf("\n");

    // ── Config dump (if exists) ────────────────────────────────────────────
    {
        std::ifstream cf(config_file);
        if (cf.is_open()) {
            printf("%s%s[CONFIG CONTENTS]%s\n", C.BOLD, C.YELLOW, C.RESET);
            std::string line;
            while (std::getline(cf, line))
                printf("  %s\n", line.c_str());
            printf("\n");
        }
    }

    // ── yt-dlp self-test ──────────────────────────────────────────────────
    printf("%s%s[YT-DLP SELF-TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    printf("  Testing yt-dlp extraction...\n");
    fflush(stdout);
    {
        FILE* tp = popen("yt-dlp --flat-playlist -j --no-warnings --ignore-errors "
                         "--playlist-end 1 "
                         "'https://pornhub.com/video/search?search=test' 2>&1 | head -3", "r");
        if (tp) {
            char tbuf[512];
            bool got_output = false;
            while (fgets(tbuf, sizeof(tbuf), tp)) {
                printf("  %s%s%s", C.DIM, tbuf, C.RESET);
                got_output = true;
            }
            pclose(tp);
            if (!got_output)
                printf("  %s[no output — yt-dlp may be broken or rate-limited]%s\n",
                       C.RED, C.RESET);
        }
    }
    printf("\n");

    // ── Thumbnail pipeline self-test ──────────────────────────────────────
    // Walks the real path a thumbnail takes — extract -> download -> render —
    // through the same code the TUI uses, so a "[loading thumbnail...]" that
    // never resolves can be pinned to one stage instead of guessed at.
    printf("%s%s[THUMBNAIL PIPELINE]%s\n", C.BOLD, C.YELLOW, C.RESET);
    fflush(stdout);
    {
        printf("  cache dir    : %s\n", ytui::Thumbnails::cache_dir().c_str());
        printf("  chafa        : %s\n",
               ytui::Thumbnails::renderer_available() ? "found" : "MISSING");

        printf("  1. extract   : running a real search...\n");
        fflush(stdout);
        ytui::Pornhub ph;
        auto results = ph.search("test", 3, "");
        printf("     results   : %zu\n", results.size());

        const ytui::Video* pick = nullptr;
        for (const auto& v : results) {
            if (v.thumbnail_url.empty())
                printf("     [%s] thumb=%s(EMPTY — extractor gave no thumbnail URL)%s\n",
                       v.id.empty() ? "NO-ID" : v.id.c_str(), C.RED, C.RESET);
            else
                printf("     [%s] thumb=%s\n",
                       v.id.empty() ? "NO-ID" : v.id.c_str(),
                       v.thumbnail_url.substr(0, 78).c_str());
            if (!pick && !v.id.empty() && !v.thumbnail_url.empty()) pick = &v;
        }

        if (results.empty()) {
            printf("  %s→ search returned nothing; thumbnails can't be tested%s\n",
                   C.RED, C.RESET);
        } else if (!pick) {
            printf("  %s→ FAIL at stage 1: no result carried a thumbnail URL.%s\n",
                   C.RED, C.RESET);
            printf("     The extractor output above has no usable 'thumbnail' or\n"
                   "     'thumbnails' field, so nothing is ever queued to download.\n");
        } else {
            printf("  2. download  : fetching %s.jpg ...\n", pick->id.c_str());
            fflush(stdout);
            std::string path = ytui::Thumbnails::thumb_path(pick->id);
            unlink(path.c_str());                    // force a real fetch
            ytui::Thumbnails::download_async(pick->id, pick->thumbnail_url);

            struct stat st{};
            long long size = -1;
            for (int i = 0; i < 100; i++) {          // up to ~10s
                usleep(100000);
                if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
                    size = (long long)st.st_size;
                    if (size > 1024) break;          // plausible image
                }
            }

            if (size < 0) {
                printf("  %s→ FAIL at stage 2: no file was written.%s\n", C.RED, C.RESET);
                printf("     curl could not fetch the URL (blocked, DNS, or timeout).\n");
                printf("     Reproduce:  curl -sfL -A '<ua>' -e '%s' -o /tmp/t.jpg '%s'\n",
                       ytui::Thumbnails::kReferer, pick->thumbnail_url.c_str());
            } else if (size <= 1024) {
                printf("  %s→ FAIL at stage 2: only %lld bytes — an error page, not an image.%s\n",
                       C.RED, size, C.RESET);
                printf("     The CDN is most likely rejecting the request (hotlink protection).\n");
            } else {
                printf("     %sok%s — %lld bytes at %s\n", C.GREEN, C.RESET, size, path.c_str());
                printf("  3. render    : chafa block art...\n");
                fflush(stdout);
                std::string art = ytui::Thumbnails::render(pick->id, 40, 12);
                if (art.empty())
                    printf("  %s→ FAIL at stage 3: chafa produced no output.%s\n", C.RED, C.RESET);
                else
                    printf("     %sok%s — chafa returned %zu bytes of block art\n",
                           C.GREEN, C.RESET, art.size());
            }
        }
    }
    printf("\n");

    // ── mpv self-test ─────────────────────────────────────────────────────
    printf("%s%s[MPV SELF-TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    printf("  Testing mpv --version...\n");
    {
        FILE* mp = popen("mpv --version 2>&1 | head -2", "r");
        if (mp) {
            char mbuf[256];
            while (fgets(mbuf, sizeof(mbuf), mp))
                printf("  %s%s%s", C.DIM, mbuf, C.RESET);
            pclose(mp);
        }
    }
    printf("\n");

    // ── Clipboard test ────────────────────────────────────────────────────
    printf("%s%s[CLIPBOARD]%s\n", C.BOLD, C.YELLOW, C.RESET);
#if defined(YTUI_MACOS)
    {
        // pbcopy ships with macOS — if it's missing something is very wrong
        bool found = (access("/usr/bin/pbcopy", X_OK) == 0);
        printf("  pbcopy   : %s%s%s  %s(built into macOS)%s\n",
               found ? C.GREEN : C.RED,
               found ? "OK" : "NOT FOUND — this should never happen",
               C.RESET, C.DIM, C.RESET);
    }
#else
    {
        bool wl = (system("which wl-copy > /dev/null 2>&1") == 0);
        bool xc = (system("which xclip  > /dev/null 2>&1") == 0);
        bool xs = (system("which xsel   > /dev/null 2>&1") == 0);
        bool any = wl || xc || xs;

        printf("  wl-copy  : %s%s%s\n",
               wl ? C.GREEN : C.DIM, wl ? "found" : "not found", C.RESET);
        printf("  xclip    : %s%s%s\n",
               xc ? C.GREEN : C.DIM, xc ? "found" : "not found", C.RESET);
        printf("  xsel     : %s%s%s\n",
               xs ? C.GREEN : C.DIM, xs ? "found" : "not found", C.RESET);

        if (any) {
            const char* which = wl ? "wl-copy" : (xc ? "xclip" : "xsel");
            printf("  %sURL copy will use: %s%s\n", C.GREEN, which, C.RESET);
        } else {
            printf("  %sWARNING: no clipboard tool found — URL copy will not work%s\n",
                   C.RED, C.RESET);
            // Detect session type to give the right install command
            bool wayland = (getenv("WAYLAND_DISPLAY") != nullptr)
                        || (getenv("XDG_SESSION_TYPE") != nullptr &&
                            std::string(getenv("XDG_SESSION_TYPE")) == "wayland");
            if (wayland) {
                printf("  Install fix (Wayland): %ssudo apt install wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo pacman -S wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo dnf install wl-clipboard%s\n",
                       C.YELLOW, C.RESET);
            } else {
                printf("  Install fix (X11):     %ssudo apt install xclip%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo pacman -S xclip%s\n",
                       C.YELLOW, C.RESET);
                printf("                         %ssudo dnf install xclip%s\n",
                       C.YELLOW, C.RESET);
            }
        }
    }
#endif
    printf("\n");

    // ── Process/pipe compat test ──────────────────────────────────────────
    printf("%s%s[PROCESS COMPAT TEST]%s\n", C.BOLD, C.YELLOW, C.RESET);
    {
        int pfd[2];
        int rc = ytui::compat::pipe_cloexec(pfd);
        if (rc == 0) {
            printf("  %spipe_cloexec()%s     : %sOK%s\n", C.DIM, C.RESET, C.GREEN, C.RESET);
            close(pfd[0]); close(pfd[1]);
        } else {
            printf("  pipe_cloexec()     : %sFAILED (%s)%s\n", C.RED, strerror(errno), C.RESET);
        }

        bool native = ytui::compat::has_native_pdeathsig();
        printf("  has_native_pdeathsig: %s%s%s\n",
               native ? C.GREEN : C.DIM,
               native ? "yes (kernel-managed)" : "no (pipe watchdog used)",
               C.RESET);

#if defined(YTUI_MACOS)
        // Test a dummy fork+watchdog to verify it doesn't crash
        int test_pipe[2];
        if (ytui::compat::pipe_cloexec(test_pipe) == 0) {
            pid_t tp = fork();
            if (tp == 0) {
                close(test_pipe[1]);
                // Watchdog grandchild would block here; just exit for the test
                char b; read(test_pipe[0], &b, 1);
                close(test_pipe[0]);
                _exit(0);
            } else if (tp > 0) {
                close(test_pipe[0]);
                usleep(5000);
                close(test_pipe[1]); // Triggers EOF → child exits
                waitpid(tp, nullptr, 0);
                printf("  pipe watchdog fork : %sOK (fork+pipe+EOF test passed)%s\n",
                       C.GREEN, C.RESET);
            } else {
                printf("  pipe watchdog fork : %sFAILED (fork error: %s)%s\n",
                       C.RED, strerror(errno), C.RESET);
            }
        }
#endif
    }
    printf("\n");

    // ── Recent debug log tail ─────────────────────────────────────────────
    {
        std::ifstream lf(log_file);
        if (lf.is_open()) {
            printf("%s%s[LAST 20 LOG LINES]%s  (%s)\n",
                   C.BOLD, C.YELLOW, C.RESET, log_file.c_str());
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(lf, line)) {
                lines.push_back(line);
                if (lines.size() > 20) lines.erase(lines.begin());
            }
            for (auto& l : lines)
                printf("  %s%s%s\n", C.DIM, l.c_str(), C.RESET);
            printf("\n");
        }
    }

    printf("%s%s══ Diagnostic complete ══%s\n\n", C.BOLD, C.CYAN, C.RESET);
}

// ─── --injectconfig: write a key=value into config.json ───────────────────────
// Usage: avcui --injectconfig key=value [key=value ...]
// Supports all Config fields: max_results, theme, grayscale, sort_by,
//   filter_type, filter_dur, ytdlp_path, mpv_path

static int run_injectconfig(const std::vector<std::string>& pairs, const Ansi& C) {
    ytui::Config cfg;
    cfg.load();

    bool any_set = false;

    for (const auto& pair : pairs) {
        // Bare shorthand: `--injectconfig mlterm` (also mono / bw) persists the
        // strict black & white mlterm theme without needing key=value syntax.
        if (pair == "mlterm" || pair == "mono" || pair == "bw") {
            cfg.theme_name = "mlterm";
            printf("  %sset%s theme = mlterm (strict B&W, no thumbnails/emphasis)\n",
                   C.GREEN, C.RESET);
            printf("  %snote%s: avcui already auto-detects mlterm at startup, so this\n"
                   "        is only needed if you want black & white on every terminal\n"
                   "        (it will also drop colour on non-mlterm terminals).\n",
                   C.YELLOW, C.RESET);
            any_set = true;
            continue;
        }
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "%sError%s: invalid format '%s' — expected key=value\n",
                    C.RED, C.RESET, pair.c_str());
            continue;
        }
        std::string key = pair.substr(0, eq);
        std::string val = pair.substr(eq + 1);

        if (key == "max_results") {
            cfg.max_results = atoi(val.c_str());
            printf("  %sset%s max_results = %d\n", C.GREEN, C.RESET, cfg.max_results);
            any_set = true;
        } else if (key == "theme") {
            cfg.theme_name = val;
            printf("  %sset%s theme = %s\n", C.GREEN, C.RESET, cfg.theme_name.c_str());
            any_set = true;
        } else if (key == "grayscale") {
            cfg.grayscale = (val == "true" || val == "1" || val == "yes");
            printf("  %sset%s grayscale = %s\n", C.GREEN, C.RESET, cfg.grayscale ? "true" : "false");
            any_set = true;
        } else if (key == "sort_by") {
            cfg.sort_by = val;
            printf("  %sset%s sort_by = %s\n", C.GREEN, C.RESET, cfg.sort_by.c_str());
            any_set = true;
        } else if (key == "filter_type") {
            cfg.filter_type = val;
            printf("  %sset%s filter_type = %s\n", C.GREEN, C.RESET, cfg.filter_type.c_str());
            any_set = true;
        } else if (key == "filter_dur") {
            cfg.filter_dur = val;
            printf("  %sset%s filter_dur = %s\n", C.GREEN, C.RESET, cfg.filter_dur.c_str());
            any_set = true;
        } else if (key == "ytdlp_path") {
            cfg.ytdlp_path = val;
            printf("  %sset%s ytdlp_path = %s\n", C.GREEN, C.RESET, cfg.ytdlp_path.c_str());
            any_set = true;
        } else if (key == "mpv_path") {
            cfg.mpv_path = val;
            printf("  %sset%s mpv_path = %s\n", C.GREEN, C.RESET, cfg.mpv_path.c_str());
            any_set = true;
        } else {
            fprintf(stderr, "  %sUnknown key%s: '%s'\n  Valid keys: max_results, theme, "
                    "grayscale, sort_by, filter_type, filter_dur, ytdlp_path, mpv_path\n",
                    C.RED, C.RESET, key.c_str());
        }
    }

    if (any_set) {
        cfg.save();
        printf("  %sSaved%s → %s/config.json\n",
               C.GREEN, C.RESET, ytui::Config::config_dir().c_str());
    }
    return any_set ? 0 : 1;
}

// ─── Help ─────────────────────────────────────────────────────────────────────

static void print_version() {
    printf("avcui %s\n", ytui::VERSION);
}

static void print_help(const Ansi& C) {
    // A clean, grouped help screen. Colour when stdout is a TTY (via Ansi),
    // plain text when piped. Two-column layout: flag on the left, description
    // on the right, aligned. No ncurses — this is just stdout.
    printf("%s%savcui%s %s  %s— a terminal adult-video client%s\n\n",
           C.BOLD, C.CYAN, C.RESET, ytui::VERSION, C.DIM, C.RESET);

    printf("%sUsage%s\n", C.BOLD, C.RESET);
    printf("  avcui [options]        %sstart the app%s\n", C.DIM, C.RESET);
    printf("  avcui --theme dracula  %spick a colour theme%s\n", C.DIM, C.RESET);
    printf("  avcui --diag           %sprint a system diagnostic and exit%s\n\n", C.DIM, C.RESET);

    printf("%sGeneral%s\n", C.BOLD, C.RESET);
    printf("  %s-h, --help%s          show this help\n", C.GREEN, C.RESET);
    printf("  %s-v, --version%s       show version\n", C.GREEN, C.RESET);
    printf("  %s-t, --theme%s <name>  set colour theme (see Themes below)\n", C.GREEN, C.RESET);
    printf("  %s--mode%s <mode>       ui mode: auto | normal | streamlined\n", C.GREEN, C.RESET);
    printf("  %s--provider%s <name>   content backend: pornhub | missav\n", C.GREEN, C.RESET);
    printf("  %s--gfx%s <mode>        thumbnails: sixel | kitty | iterm | blocks | auto | off\n", C.GREEN, C.RESET);
    printf("  %s%s%s               default off. A raster mode also turns on the slower\n",
           C.DIM, "     ", C.RESET);
    printf("  %s%s%s               metadata pass, so durations and uploaders fill in too\n",
           C.DIM, "     ", C.RESET);
    printf("  %s--mono%s, %s--bw%s       strict black & white\n", C.GREEN, C.RESET, C.GREEN, C.RESET);
    printf("  %s--colors%s            list colour elements + a config example\n\n", C.GREEN, C.RESET);

    printf("%sPlayback%s %s(this session only)%s\n", C.BOLD, C.RESET, C.DIM, C.RESET);
    printf("  %s--volume%s <0-130>    starting volume (default 80)\n", C.GREEN, C.RESET);
    printf("  %s--no-ha%s             disable mpv hardware acceleration\n", C.GREEN, C.RESET);
    printf("  %s--no-cache%s          disable mpv demuxer cache\n", C.GREEN, C.RESET);
    printf("  %s--mpv-verbose%s       don't silence mpv's own output\n\n", C.GREEN, C.RESET);

    printf("%sConfig%s\n", C.BOLD, C.RESET);
    printf("  %s--injectconfig%s k=v  set a config value without opening the app\n", C.GREEN, C.RESET);
    printf("  %s-d, --debug%s         write a debug log to ~/.cache/avcui/debug.log\n", C.GREEN, C.RESET);
    printf("  %s--logdump%s           write a full timestamped log to ~/avcui-DATE.log\n", C.GREEN, C.RESET);
    printf("  %s--missav-test%s [q]   exercise the native MissAV provider and exit\n", C.GREEN, C.RESET);
    printf("  %s(tip)%s               press %sCtrl-S%s inside the app to open live settings\n",
           C.DIM, C.RESET, C.CYAN, C.RESET);
    printf("                       %s— change themes, remap keys, toggle options on the fly%s\n\n",
           C.DIM, C.RESET);

    printf("%sKeys%s %s(press %s?%s%s in-app for the full list)%s\n",
           C.BOLD, C.RESET, C.DIM, C.RESET, C.DIM, C.DIM, C.RESET);
    printf("  %sSpace / p%s   pause / resume        %s+  -%s   volume up / down\n",
           C.CYAN, C.RESET, C.CYAN, C.RESET);
    printf("  %s<  >%s        seek back / forward    %s/%s     search\n",
           C.CYAN, C.RESET, C.CYAN, C.RESET);
    printf("  %sj / k%s       move down / up         %sTab%s   cycle panels\n",
           C.CYAN, C.RESET, C.CYAN, C.RESET);
    printf("  %sEnter%s       select                 %sEsc%s   back\n",
           C.CYAN, C.RESET, C.CYAN, C.RESET);
    printf("  %sCtrl-S%s      settings (theme + keys) %sq%s     quit\n\n",
           C.CYAN, C.RESET, C.CYAN, C.RESET);

    printf("%sThemes%s %s(or change live in-app with Ctrl-S)%s\n", C.BOLD, C.RESET, C.DIM, C.RESET);
    printf("  default  grayscale  nord  dracula  solarized  monokai  gruvbox\n");
    printf("  tokyo  pink  green  blue  purple  red  amber  ocean  mint  coral  slate\n\n");

    printf("%sFiles%s\n", C.BOLD, C.RESET);
    printf("  config    ~/.config/avcui/config.json\n");
    printf("  library   ~/.local/share/avcui/\n");
    printf("  cache     ~/.cache/avcui/\n\n");

    printf("%sMore%s\n", C.BOLD, C.RESET);
    printf("  %sbased on ytcui — https://github.com/MilkmanAbi/ytcui%s\n", C.DIM, C.RESET);
    printf("  %smlterm terminals are auto-detected and fall back to B&W; no setup needed.%s\n",
           C.DIM, C.RESET);
}


// ═══════════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "");

#if defined(__APPLE__) && defined(__MACH__)
    // MacPorts legacysupport's redirect_bins wraps installed binaries in a
    // script exporting DYLD_LIBRARY_PATH=/opt/local/lib/libgcc. dyld already
    // consumed that override when it launched us; from here on the variable
    // only affects children. In mpv it swaps Apple's /usr/lib/libstdc++ for
    // the MacPorts one by leaf-name match, and on 10.6/ppc a C++ throw inside
    // an Apple framework (ImageIO decoding mpv's app icon) then unwinds across
    // mismatched runtimes — SIGBUS in unw_get_proc_info. Children (mpv,
    // yt-dlp, sh pipelines) must start without it. DYLD_INSERT_LIBRARIES is
    // deliberately kept: an unwinder-interpose shim would ride on it.
    unsetenv("DYLD_LIBRARY_PATH");
#endif

    // libcurl's global init is not thread-safe and must happen before any Http
    // is constructed and outlive them all — the native provider builds handles
    // on enrichment worker threads.
    ytui::CurlGlobal curl_global;

    Ansi C(isatty(STDOUT_FILENO) != 0);

    // Upstream routed --ois/--update/--uninstall/--reinstall/--install-info to
    // its bundled OIS installer here. avcui is packaged externally, so those
    // flags are not ours to serve — installation is the packager's job.

    // ── Pre-scan for --no-update-check ────────────────────────────────────
    bool skip_update_check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-update-check") == 0) {
            skip_update_check = true;
            break;
        }
    }

    // ── Collect all argv into a vector for easier parsing ─────────────────
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);

    // ── Early-exit commands: --help, --version ─────────────────────────────
    for (auto& a : args) {
        if (a == "--help" || a == "-h") { print_help(C); return 0; }
        if (a == "--version" || a == "-v") { print_version(); return 0; }
    }

    // ── --diag: full system diagnostic (early exit, no TUI) ───────────────
    for (auto& a : args) {
        if (a == "--diag") {
            // Let --mlterm influence the diagnostic too (it forces the terminal
            // identity before detection), so `avcui --mlterm --diag` shows the
            // hardened result.
            for (auto& b : args)
                if (b == "--mlterm") ytui::TermCaps::set_force_mlterm(true);
            run_diag(C);
            return 0;
        }
    }

    // ── --missav-test: exercise the native MissAV path, no TUI ────────────
    // Deliberately standalone. The Recombee signing and the packed-JS m3u8
    // unpacking are the two things most likely to be wrong or to rot, and both
    // are far cheaper to verify here than through the UI.
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] != "--missav-test") continue;
        std::string q = (i + 1 < args.size() && args[i + 1][0] != '-')
                            ? args[i + 1] : "stepdaughter";
        ytui::Log::init(true);   // route [missav] diagnostics to the debug log

        printf("\n%s%s[MISSAV] native provider test%s\n\n", C.BOLD, C.CYAN, C.RESET);
        printf("  search   : %s\n", q.c_str());
        fflush(stdout);

        ytui::MissAV mv;
        auto results = mv.search(q, 10);
        printf("  results  : %zu\n\n", results.size());
        if (results.empty()) {
            printf("  %sSEARCH FAILED%s — Recombee returned nothing. Either the\n",
                   C.RED, C.RESET);
            printf("  public token was rotated, or the request was rejected.\n");
            printf("  See ~/.cache/avcui/debug.log for the HTTP status.\n\n");
            return 1;
        }

        int shown = 0;
        for (const auto& v : results) {
            printf("  [%d] %s\n", shown, v.title.c_str());
            printf("      url   : %s\n", v.url.c_str());
            if (!v.thumbnail_url.empty())
                printf("      thumb : %s %s(from search — no detail fetch needed)%s\n",
                       v.thumbnail_url.substr(0, 70).c_str(), C.GREEN, C.RESET);
            if (++shown >= 3) break;
        }
        printf("\n");

        printf("  detail fetch on [0]...\n");
        fflush(stdout);
        auto full = mv.get_video(results[0].url);
        if (!full) {
            printf("  %sDETAIL FAILED%s — could not parse the video page.\n\n", C.RED, C.RESET);
            return 1;
        }
        printf("      title : %s\n", full->title.c_str());
        printf("      dur   : %s (%d s)\n", full->duration.c_str(), full->duration_seconds);
        printf("      date  : %s\n", full->upload_date.c_str());
        printf("      thumb : %s\n", full->thumbnail_url.substr(0, 78).c_str());
        printf("      tags  : %s\n", full->tags.substr(0, 78).c_str());
        if (full->stream_url.empty()) {
            printf("      stream: %sMISSING — packed-JS layout changed%s\n", C.RED, C.RESET);
            return 1;
        }
        printf("      stream: %s%s%s\n", C.GREEN, full->stream_url.c_str(), C.RESET);

        // Both CDNs hotlink-protect: a bare fetch of either 403s despite the
        // URL being correct. Prove it here so a 403 later isn't mistaken for a
        // bad extraction.
        printf("\n  checking CDN access (both need a missav Referer)...\n");
        fflush(stdout);
        {
            std::string cmd = std::string("curl -s -o /dev/null -w '%{http_code}' --max-time 10 ")
                            + "-A '" + ytui::MissAV::user_agent() + "' "
                            + "-e '" + ytui::MissAV::referer() + "' '" + full->stream_url + "'";
            FILE* p = popen(cmd.c_str(), "r");
            char code[8] = {0};
            if (p) { char* r = fgets(code, sizeof(code), p); (void)r; pclose(p); }
            printf("      manifest  : HTTP %s%s\n", code,
                   strncmp(code, "200", 3) == 0 ? "  ok" : "  <- FAILED");
        }
        if (!full->thumbnail_url.empty()) {
            std::string cmd = std::string("curl -s -o /dev/null -w '%{http_code}' --max-time 10 ")
                            + "-A '" + ytui::MissAV::user_agent() + "' "
                            + "-e '" + ytui::MissAV::referer() + "' '" + full->thumbnail_url + "'";
            FILE* p = popen(cmd.c_str(), "r");
            char code[8] = {0};
            if (p) { char* r = fgets(code, sizeof(code), p); (void)r; pclose(p); }
            printf("      thumbnail : HTTP %s%s\n", code,
                   strncmp(code, "200", 3) == 0 ? "  ok" : "  <- FAILED");
        }

        printf("\n  %sExtraction OK.%s Play with (headers are required — a bare\n",
               C.GREEN, C.RESET);
        printf("  mpv on this URL returns 403):\n\n      mpv --ytdl=no \\\n");
        for (const auto& h : ytui::MissAV::mpv_header_args())
            printf("        \"%s\" \\\n", h.c_str());
        printf("        '%s'\n\n", full->stream_url.c_str());
        return 0;
    }

    // ── --colors: print colour reference and config example ───────────────
    for (auto& a : args) {
        if (a == "--colors" || a == "--colour" || a == "--color-help") {
            printf("\n%s avcui colour elements%s\n\n", C.BOLD, C.RESET);
            printf("  Each element can be set to a 256-colour index (0–255) or -1 for terminal default.\n");
            printf("  Custom colours are set in %s~/.config/avcui/config.json%s under \"colors\": {}\n", C.CYAN, C.RESET);
            printf("  They are applied ON TOP of any base theme — override as many or as few as you like.\n\n");
            printf("  %sElements:%s\n", C.BOLD, C.RESET);
            const struct { const char* name; const char* desc; } elems[] = {
                { "bg",         "general text / default foreground" },
                { "search_box", "search input text & cursor" },
                { "title",      "video title rows in results" },
                { "channel",    "channel / uploader name" },
                { "stats",      "view count, duration, stats line" },
                { "selected",   "selected-row foreground (usually -1)" },
                { "action",     "action menu items (unselected)" },
                { "action_sel", "action menu selected item (usually -1)" },
                { "status",     "status bar text at the bottom" },
                { "border",     "box / panel borders" },
                { "header",     "top header bar text" },
                { "accent",     "tabs active, playlist highlight, popups" },
                { "tag",        "[LIVE] / [4K] tag labels" },
                { "published",  "publish / upload date" },
                { "bookmark",   "bookmark indicator" },
                { "desc",       "video description text" },
            };
            for (auto& e : elems)
                printf("    %s%-12s%s  %s\n", C.CYAN, e.name, C.RESET, e.desc);
            printf("\n  %sExample config.json:%s\n\n", C.BOLD, C.RESET);
            printf("  {\n");
            printf("    \"theme\": \"dracula\",\n");
            printf("    \"colors\": {\n");
            printf("      \"accent\": 198,\n");
            printf("      \"title\":  213,\n");
            printf("      \"border\": 141\n");
            printf("    }\n");
            printf("  }\n\n");
            printf("  %sAvailable themes:%s\n", C.BOLD, C.RESET);
            printf("    default, grayscale, nord, dracula, solarized, monokai, gruvbox, tokyo\n");
            printf("    pink, green, blue, purple, red, amber, ocean, mint, coral, slate\n");
            printf("    mlterm (strictly black & white, no colour, reverse-video selection)\n\n");
            printf("  %s256-colour reference:%s https://www.ditig.com/256-colors-cheat-sheet\n\n", C.BOLD, C.RESET);
            return 0;
        }
    }

    // ── --injectconfig: write config keys (early exit, no TUI) ───────────
    {
        bool inject_mode = false;
        std::vector<std::string> inject_pairs;
        for (int i = 0; i < (int)args.size(); i++) {
            if (args[i] == "--injectconfig") {
                inject_mode = true;
            } else if (inject_mode) {
                // Collect all subsequent key=value pairs (stop at next -- flag)
                if (args[i][0] == '-' && args[i].size() > 1 && args[i][1] == '-') break;
                inject_pairs.push_back(args[i]);
            }
        }
        if (inject_mode) {
            if (inject_pairs.empty()) {
                fprintf(stderr, "Usage: avcui --injectconfig key=value [key=value ...]\n");
                fprintf(stderr, "Keys: max_results, theme, grayscale, sort_by, "
                                "filter_type, filter_dur, ytdlp_path, mpv_path\n");
                return 1;
            }
            return run_injectconfig(inject_pairs, C);
        }
    }

    // ── Parse runtime flags ────────────────────────────────────────────────
    ytui::Theme theme = ytui::Theme::Default;
    bool debug        = false;
    bool logdump      = false;
    std::string gfx_mode;             // --gfx auto|blocks|sixel|kitty|iterm|off
    std::string ui_mode;
    std::string provider_override;    // --provider pornhub|missav
    ytui::PlayerOptions player_opts;  // defaults: vol=80, hwdec=auto, cache=on

    // Seed hardware-accel preference from config.json (written by the
    // installer). A --no-ha flag below still overrides it for this run.
    {
        ytui::Config seed_cfg;
        seed_cfg.load();
        player_opts.no_hardware_accel = seed_cfg.no_hardware_accel;
    }

    for (int i = 0; i < (int)args.size(); i++) {
        const std::string& a = args[i];

        if ((a == "--theme" || a == "-t") && i + 1 < (int)args.size()) {
            theme = ytui::string_to_theme(args[++i]);
        } else if (a == "--grayscale" || a == "-g") {
            theme = ytui::Theme::Grayscale;
        } else if (a == "--mlterm") {
            // "This is mlterm" — select the B&W theme AND force terminal
            // hardening (bold/dim strip, no thumbnails) even if auto-detection
            // didn't identify it. Must be set before TermCaps::detect() runs.
            theme = ytui::Theme::MLterm;
            ytui::TermCaps::set_force_mlterm(true);
        } else if (a == "--mono" || a == "--bw") {
            // Pure black & white colour theme. On a real mlterm, detect() still
            // applies the full hardening; on a colour terminal this just drops
            // all colour (bold/emphasis is left intact — it renders fine there).
            theme = ytui::Theme::MLterm;
        } else if (a == "--debug" || a == "-d") {
            debug = true;
        } else if (a == "--logdump") {
            logdump = true;
            debug = true;   // logdump implies debug — no separate flag needed
        } else if (a == "--no-ha" || a == "--no-hardware-acceleration") {
            player_opts.no_hardware_accel = true;
        } else if (a == "--no-cache") {
            player_opts.no_cache = true;
        } else if (a == "--mpv-verbose") {
            player_opts.verbose_mpv = true;
        } else if (a == "--volume" && i + 1 < (int)args.size()) {
            int v = atoi(args[++i].c_str());
            if (v < 0 || v > 130) {
                fprintf(stderr, "Warning: --volume %d out of range (0-130), clamping\n", v);
                v = (v < 0) ? 0 : 130;
            }
            player_opts.volume = v;
        } else if ((a == "--gfx" || a == "--graphics") && i + 1 < (int)args.size()) {
            gfx_mode = args[++i];
            if (gfx_mode != "auto" && gfx_mode != "blocks" && gfx_mode != "sixel"
                && gfx_mode != "kitty" && gfx_mode != "iterm" && gfx_mode != "off") {
                fprintf(stderr, "Invalid --gfx mode: %s "
                                "(auto|blocks|sixel|kitty|iterm|off)\n", gfx_mode.c_str());
                return 1;
            }
        } else if (a == "--mode" && i + 1 < (int)args.size()) {
            ui_mode = args[++i];
            if (ui_mode != "auto" && ui_mode != "normal" && ui_mode != "streamlined") {
                fprintf(stderr, "Invalid --mode: %s (auto|normal|streamlined)\n", ui_mode.c_str());
                return 1;
            }
        } else if (a == "--provider") {
            if (i + 1 >= args.size()) {
                fprintf(stderr, "--provider needs a value (pornhub|missav)\n");
                return 1;
            }
            provider_override = args[++i];
            if (!ytui::Provider::valid(provider_override)) {
                fprintf(stderr, "Invalid --provider: %s (pornhub|missav)\n",
                        provider_override.c_str());
                return 1;
            }
        } else if (a == "--no-update-check") {
            // already handled by the pre-scan above
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a.c_str());
            fprintf(stderr, "Try 'avcui --help' for more information.\n");
            return 1;
        }
    }

    if (debug) ytui::Log::init(true, logdump);
    ytui::Log::write("avcui %s starting (theme=%s debug=%s logdump=%s vol=%d no_ha=%s no_cache=%s)",
                     ytui::VERSION,
                     ytui::theme_to_string(theme).c_str(),
                     debug   ? "yes" : "no",
                     logdump ? "yes" : "no",
                     player_opts.volume,
                     player_opts.no_hardware_accel ? "yes" : "no",
                     player_opts.no_cache          ? "yes" : "no");

    if (debug) {
        ytui::Log::write("--- compat platform dump ---");
#if defined(YTUI_MACOS)
        ytui::Log::write("macOS major version: %d", ytui::compat::macos_major_version());
#endif
        ytui::Log::write("has_native_pdeathsig: %s",
                         ytui::compat::has_native_pdeathsig() ? "yes" : "no");
    }

    if (logdump) {
        // Tell the user where the dump file landed (printed before TUI starts)
        fprintf(stderr, "Logdump → %s\n", ytui::Log::get_logdump_path().c_str());
    }

    // ── Dependency checks ─────────────────────────────────────────────────
    // yt-dlp is required only by the providers that shell out to it. The whole
    // point of the native MissAV backend is that it needs neither yt-dlp nor
    // Python, so demanding it here would defeat that.
    {
        std::string prov = provider_override;
        if (prov.empty()) { ytui::Config c; c.load(); prov = c.provider; }
        if (prov != "missav" && !ytui::Pornhub::is_available()) {
            fprintf(stderr, "Error: yt-dlp not found. Install with: pip install yt-dlp\n");
            fprintf(stderr, "  (not needed with --provider missav)\n");
            fprintf(stderr, "Run 'avcui --diag' for a full system diagnostic.\n");
            return 1;
        }
    }
    if (!ytui::Player::is_available()) {
        fprintf(stderr, "Error: mpv not found. Install with: brew install mpv / apt install mpv\n");
        fprintf(stderr, "Run 'avcui --diag' for a full system diagnostic.\n");
        return 1;
    }

    // ── Background update check ───────────────────────────────────────────
    // Keep this joinable (not detached): a detached thread doing curl work can
    // still be running when curl_global_cleanup() fires at process exit, which
    // is a use-after-free. We join it before returning from main().
    // (A no-op unless AVCUI_VERSION_URL was defined at build time.)
    std::thread update_thread;
    if (!skip_update_check)
        update_thread = std::thread(check_for_updates_async);

    usleep(100000);
    if (g_update_available) {
        fprintf(stderr, "\033[33m⬆ Update available: %s → %s\033[0m\n\n",
                ytui::VERSION, g_remote_version.c_str());
        usleep(1500000);
    }

    // ── Launch app ────────────────────────────────────────────────────────
    // Identify the terminal and its capabilities BEFORE ncurses takes the tty
    // (the detection handshake needs raw tty access to read query replies).
    ytui::TermCaps::detect();

    ytui::App app(theme, provider_override);
    app.set_player_options(player_opts);
    app.set_graphics_mode(gfx_mode);   // no-op when --gfx wasn't passed
    app.set_ui_mode(ui_mode);          // no-op when --mode wasn't passed
    g_app = &app;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = app.run();
    g_app = nullptr;

    // ── Orderly teardown ──────────────────────────────────────────────────
    // Join the update-check thread so no thread is alive during static
    // teardown. Upstream also stopped a prefetch worker here; our extractor
    // shells out per search and owns no background threads.
    if (update_thread.joinable()) update_thread.join();

    ytui::Log::shutdown();
    return ret;
}
