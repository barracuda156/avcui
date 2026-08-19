#include "pornhub.h"
#include "log.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <array>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

using json = nlohmann::json;

namespace ytui {

Pornhub::Pornhub() {}
Pornhub::~Pornhub() {}

bool Pornhub::is_available() {
    return system("which yt-dlp > /dev/null 2>&1") == 0;
}

// Strip invalid UTF-8 bytes and control chars, keeping only clean printable text
std::string Pornhub::sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.c_str();
    const unsigned char* end = p + s.size();

    while (p < end) {
        if (*p < 0x80) {
            // ASCII: keep printable chars, spaces, tabs
            if (*p >= 32 || *p == '\t' || *p == '\n')
                out += (char)*p;
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end && (p[1] & 0xC0) == 0x80) {
            // 2-byte UTF-8
            out.append((const char*)p, 2);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            // 3-byte UTF-8 (CJK, etc)
            out.append((const char*)p, 3);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            // 4-byte UTF-8 (emoji, etc)
            out.append((const char*)p, 4);
            p += 4;
        } else {
            // Invalid byte — skip it
            p++;
        }
    }
    return out;
}

std::string Pornhub::format_views(long long views) {
    if (views >= 1000000000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2fB", views / 1e9); return buf;
    } else if (views >= 1000000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2fM", views / 1e6); return buf;
    } else if (views >= 1000) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.2fK", views / 1e3); return buf;
    }
    return std::to_string(views);
}

std::string Pornhub::format_duration(int secs) {
    if (secs <= 0) return "0:00";
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

std::vector<Video> Pornhub::search(const std::string& query, int max_results,
                                      const std::string& cookie_args) {
    std::vector<Video> videos;
    Log::write("Searching Pornhub: '%s' (max %d)%s", query.c_str(), max_results,
               cookie_args.empty() ? "" : " [auth]");

    std::vector<std::string> args;

    // Add cookie args if provided (for age-restricted content)
    if (!cookie_args.empty()) {
        std::istringstream iss(cookie_args);
        std::string tok;
        while (iss >> tok) args.push_back(tok);
    }

    // Pornhub search URL pattern
    args.push_back("\"https://pornhub.com/video/search?search=" + query + "\"");
    args.push_back("-j");
    args.push_back("--flat-playlist");
    args.push_back("--playlist-end");
    args.push_back(std::to_string(max_results));
    args.push_back("--no-warnings");
    args.push_back("--ignore-errors");

    std::string output = exec_ytdlp(args);
    if (output.empty()) {
        Log::write("yt-dlp returned empty output for search");
        return videos;
    }

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);

            auto safe_str = [&](const char* key, const std::string& fallback = "") -> std::string {
                if (j.contains(key) && j[key].is_string()) {
                    return sanitize_utf8(j[key].get<std::string>());
                }
                return fallback;
            };

            // Thumbnail URL, tolerant of yt-dlp's two shapes. Under
            // --flat-playlist many extractors omit the flat "thumbnail" string
            // and only emit a "thumbnails" array; taking just "thumbnail" then
            // yields an empty URL and nothing is ever downloaded.
            auto best_thumbnail = [&]() -> std::string {
                std::string flat = safe_str("thumbnail");
                if (!flat.empty()) return flat;
                if (!j.contains("thumbnails") || !j["thumbnails"].is_array()) return "";
                // Prefer the widest entry; the array is usually ascending, so a
                // plain last-with-a-url also works as a fallback.
                std::string best; long long best_w = -1;
                for (const auto& t : j["thumbnails"]) {
                    if (!t.is_object() || !t.contains("url") || !t["url"].is_string()) continue;
                    std::string u = sanitize_utf8(t["url"].get<std::string>());
                    if (u.empty()) continue;
                    long long w = (t.contains("width") && t["width"].is_number())
                                      ? t["width"].get<long long>() : 0;
                    if (w >= best_w) { best_w = w; best = u; }
                }
                return best;
            };

            Video v;
            v.id = safe_str("id");
            v.title = safe_str("title", "Unknown");

            // Pornhub uses "uploader" field
            v.channel = safe_str("uploader");
            if (v.channel.empty()) v.channel = safe_str("channel", "Unknown");
            v.channel_id = safe_str("uploader_id");

            v.thumbnail_url = best_thumbnail();
            v.description = safe_str("description");

            if (j.contains("duration") && j["duration"].is_number())
                v.duration_seconds = j["duration"].get<int>();
            v.duration = format_duration(v.duration_seconds);

            v.url = safe_str("url");
            if (v.url.empty() && !v.id.empty()) {
                v.url = safe_str("webpage_url");
            }

            if (j.contains("view_count") && j["view_count"].is_number()) {
                v.view_count = format_views(j["view_count"].get<long long>());
            } else {
                v.view_count = "N/A";
            }

            // Adult-specific fields
            v.quality = safe_str("format_note", "HD");
            if (j.contains("tags") && j["tags"].is_array()) {
                std::string tags_str;
                for (const auto& tag : j["tags"]) {
                    if (tag.is_string()) {
                        if (!tags_str.empty()) tags_str += ", ";
                        tags_str += tag.get<std::string>();
                    }
                }
                v.tags = tags_str;
            }

            std::string raw_date = safe_str("upload_date");
            if (raw_date.size() == 8)
                v.upload_date = raw_date.substr(0, 4) + "-" + raw_date.substr(4, 2) + "-" + raw_date.substr(6, 2);
            else if (!raw_date.empty())
                v.upload_date = raw_date;

            if (!v.url.empty()) {
                Log::write("  [%s] %s (%s)", v.id.c_str(), v.title.c_str(), v.duration.c_str());
                videos.push_back(std::move(v));
            }
        } catch (const std::exception& e) {
            Log::write("JSON parse error: %s", e.what());
            continue;
        }
    }

    Log::write("Search returned %zu results", videos.size());
    return videos;
}

std::vector<Video> Pornhub::get_trending(int max_results) {
    std::vector<Video> videos;
    Log::write("Getting Pornhub trending (max %d)", max_results);

    std::vector<std::string> args;

    // Pornhub trending/most viewed URL
    args.push_back("\"https://pornhub.com/video?o=mv\"");
    args.push_back("-j");
    args.push_back("--flat-playlist");
    args.push_back("--playlist-end");
    args.push_back(std::to_string(max_results));
    args.push_back("--no-warnings");
    args.push_back("--ignore-errors");

    std::string output = exec_ytdlp(args);

    // Parse same as search
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            Video v = parse_video_json(line);
            if (!v.url.empty()) {
                videos.push_back(std::move(v));
            }
        } catch (...) {
            continue;
        }
    }

    return videos;
}

std::vector<Video> Pornhub::get_category(const std::string& category, int max_results) {
    std::vector<Video> videos;
    Log::write("Getting Pornhub category '%s' (max %d)", category.c_str(), max_results);

    std::vector<std::string> args;

    // Pornhub category URL
    args.push_back("\"https://pornhub.com/categories/" + category + "\"");
    args.push_back("-j");
    args.push_back("--flat-playlist");
    args.push_back("--playlist-end");
    args.push_back(std::to_string(max_results));
    args.push_back("--no-warnings");
    args.push_back("--ignore-errors");

    std::string output = exec_ytdlp(args);

    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            Video v = parse_video_json(line);
            if (!v.url.empty()) {
                videos.push_back(std::move(v));
            }
        } catch (...) {
            continue;
        }
    }

    return videos;
}

std::string Pornhub::get_stream_url(const std::string& video_url, const std::string& quality) {
    std::vector<std::string> args = {
        video_url,
        "-g",
        "--no-warnings"
    };

    // Quality selection
    if (quality == "best") {
        args.push_back("-f");
        args.push_back("bestvideo+bestaudio/best");
    } else if (quality == "audio") {
        args.push_back("-f");
        args.push_back("bestaudio");
    } else if (!quality.empty()) {
        // Specific quality like "720p", "1080p"
        args.push_back("-f");
        args.push_back("bestvideo[height<=" + quality.substr(0, quality.find('p')) + "]+bestaudio/best");
    }

    return exec_ytdlp(args);
}

std::optional<Video> Pornhub::get_video_info(const std::string& video_url) {
    std::vector<std::string> args = {
        video_url,
        "-j",
        "--no-warnings"
    };

    std::string output = exec_ytdlp(args);
    if (output.empty()) return std::nullopt;

    try {
        Video v = parse_video_json(output);
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

Video Pornhub::parse_video_json(const std::string& json_str) {
    Video v;

    try {
        auto j = json::parse(json_str);

        auto safe_str = [&](const char* key, const std::string& fallback = "") -> std::string {
            if (j.contains(key) && j[key].is_string()) {
                return sanitize_utf8(j[key].get<std::string>());
            }
            return fallback;
        };

        v.id = safe_str("id");
        v.title = safe_str("title", "Unknown");
        v.channel = safe_str("uploader");
        if (v.channel.empty()) v.channel = safe_str("channel", "Unknown");
        v.channel_id = safe_str("uploader_id");
        v.thumbnail_url = safe_str("thumbnail");
        v.description = safe_str("description");
        v.url = safe_str("webpage_url");

        if (j.contains("duration") && j["duration"].is_number())
            v.duration_seconds = j["duration"].get<int>();
        v.duration = format_duration(v.duration_seconds);

        if (j.contains("view_count") && j["view_count"].is_number()) {
            v.view_count = format_views(j["view_count"].get<long long>());
        }

        v.quality = safe_str("format_note", "HD");

        std::string raw_date = safe_str("upload_date");
        if (raw_date.size() == 8)
            v.upload_date = raw_date.substr(0, 4) + "-" + raw_date.substr(4, 2) + "-" + raw_date.substr(6, 2);

    } catch (const std::exception& e) {
        Log::write("JSON parse error: %s", e.what());
    }

    return v;
}

std::string Pornhub::exec_ytdlp(const std::vector<std::string>& args) {
    std::string cmd = "yt-dlp";
    for (const auto& arg : args) cmd += " " + arg;

    // Keep stderr visible if logging is enabled for debugging
    if (Log::is_enabled()) {
        cmd += " 2>&1";  // Include stderr in output for debugging
    } else {
        cmd += " 2>/dev/null";  // Hide errors in normal mode
    }

    Log::write("exec: %s", cmd.c_str());

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        Log::write("popen failed for yt-dlp command");
        return result;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    int status = pclose(pipe);

    if (status != 0) {
        Log::write("yt-dlp exit code: %d", WEXITSTATUS(status));
    }

    if (result.empty()) {
        Log::write("yt-dlp returned empty result");
    } else {
        Log::write("yt-dlp returned %zu bytes", result.size());
    }

    return result;
}

// ─── Metadata enrichment ──────────────────────────────────────────────────────
void Pornhub::enrich(const std::vector<std::string>& urls,
                     const std::string& cookie_args,
                     const std::function<void(Video)>& on_item,
                     const std::function<void(pid_t)>& on_spawn,
                     const std::atomic<bool>& cancel) {
    if (urls.empty()) return;

    // Built as an argv vector and exec'd directly — no shell. exec_ytdlp()
    // concatenates into a shell string, which would need every URL quoted, and
    // folds stderr into stdout when logging is on. Both are fatal here: we parse
    // this stream as one JSON object per line, so a single warning line
    // corrupts it.
    std::vector<std::string> argv_s = {
        "yt-dlp", "-j", "--no-warnings", "--no-playlist", "--ignore-errors"
    };
    if (!cookie_args.empty()) {
        std::istringstream iss(cookie_args);
        std::string tok;
        while (iss >> tok) argv_s.push_back(tok);
    }
    for (const auto& u : urls) argv_s.push_back(u);

    int fds[2];
    if (pipe(fds) != 0) {
        Log::write("[enrich] pipe failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        Log::write("[enrich] fork failed");
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        std::vector<const char*> av;
        av.reserve(argv_s.size() + 1);
        for (const auto& s : argv_s) av.push_back(s.c_str());
        av.push_back(nullptr);
        execvp("yt-dlp", (char* const*)av.data());
        _exit(127);
    }

    close(fds[1]);
    if (on_spawn) on_spawn(pid);
    Log::write("[enrich] pid=%d fetching %zu urls", pid, urls.size());

    FILE* f = fdopen(fds[0], "r");
    if (!f) {
        close(fds[0]);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return;
    }

    // yt-dlp emits one JSON object per line, flushed per video, so results
    // surface as each page finishes rather than at the end.
    std::string line;
    char buf[8192];
    int got = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line += buf;
        if (line.empty() || line.back() != '\n') continue;   // partial read
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty() && line[0] == '{') {
            Video v = parse_video_json(line);
            if (!v.id.empty() && on_item) { on_item(std::move(v)); got++; }
        }
        line.clear();
        if (cancel.load()) break;
    }
    fclose(f);   // closes fds[0]

    // A cancelled child is still mid-fetch; kill it so waitpid returns now.
    if (cancel.load()) kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    Log::write("[enrich] pid=%d done, %d/%zu enriched", pid, got, urls.size());
}

} // namespace ytui
