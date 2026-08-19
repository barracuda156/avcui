#include "player.h"
#include "log.h"
#include <cstring>
#include <algorithm>
#include <cerrno>
#include <vector>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace ytui {

Player::Player() {
    death_pipe_[0] = -1;
    death_pipe_[1] = -1;
}

Player::~Player() { stop(); }

bool Player::is_available() {
    return system("which mpv > /dev/null 2>&1") == 0;
}

void Player::play(const std::string& url, const std::string& title, PlayMode mode) {
    stop();
    if (mode == PlayMode::Video)
        play_direct(url, title);
    else
        play_piped(url, title, mode);
}

void Player::stop() {
    if (ipc_.connected()) ipc_.quit();   // best-effort clean exit
    ipc_.disconnect();
    ipc_.cleanup();
    kill_mpv();
    playing_ = false;
    paused_  = false;
    current_title_.clear();
}

void Player::close_death_pipe() {}

bool Player::toggle_pause() {
    if (!playing_ || mpv_pid_ <= 0) return false;
    if (ipc_.connected() && ipc_.toggle_pause()) {
        paused_ = !paused_;   // optimistic; pump() will confirm from mpv
        Log::write("IPC %s pid=%d", paused_ ? "paused" : "resumed", mpv_pid_);
    } else {
        // Fallback: SIGSTOP/SIGCONT (laggy but works without IPC)
        if (paused_) {
            kill(-mpv_pid_, SIGCONT);
            paused_ = false;
        } else {
            kill(-mpv_pid_, SIGSTOP);
            paused_ = true;
        }
        Log::write("Signal %s pgid -%d (IPC unavailable)", paused_ ? "paused" : "resumed", mpv_pid_);
    }
    return paused_;
}

bool Player::volume_up(int step) {
    current_volume_ = std::min(150, current_volume_ + step);
    if (ipc_.connected()) return ipc_.adjust_volume(step);
    return false;
}

bool Player::volume_down(int step) {
    current_volume_ = std::max(0, current_volume_ - step);
    if (ipc_.connected()) return ipc_.adjust_volume(-step);
    return false;
}

bool Player::set_volume(int vol) {
    current_volume_ = std::clamp(vol, 0, 150);
    if (ipc_.connected()) return ipc_.set_volume(current_volume_);
    return false;
}

int Player::get_volume() const {
    // Prefer mpv's reported volume when we have it; else our local shadow.
    if (ipc_.connected() && ipc_.volume() >= 0) return ipc_.volume();
    return current_volume_;
}

void Player::tick() {
    if (!playing_) return;
    if (!ipc_.connected()) ipc_.try_connect();  // one cheap attempt per frame
    ipc_.pump();                                 // drain events, refresh cache
}

bool Player::seek_forward(double secs) {
    if (ipc_.connected()) return ipc_.seek(secs);
    return false;
}

bool Player::seek_backward(double secs) {
    if (ipc_.connected()) return ipc_.seek(-secs);
    return false;
}

bool Player::is_playing() const {
    if (!playing_ || mpv_pid_ <= 0) return false;
    int status = 0;
    pid_t r = waitpid(mpv_pid_, &status, WNOHANG);
    if (r == mpv_pid_) {
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    if (r < 0) {
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    if (kill(-mpv_pid_, 0) < 0 && errno == ESRCH) {
        waitpid(mpv_pid_, &status, WNOHANG);
        const_cast<Player*>(this)->playing_ = false;
        const_cast<Player*>(this)->mpv_pid_ = -1;
        return false;
    }
    return true;
}

std::string Player::now_playing() const {
    if (is_playing()) return current_title_;
    return "";
}

// ─── helpers ──────────────────────────────────────────────────────────────────

static void child_setup(bool log_to_file) {
    setpgid(0, 0);
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        if (!log_to_file) dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    if (log_to_file) {
        std::string el = Log::get_log_dir() + "/mpv.log";
        int ef = open(el.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (ef >= 0) { dup2(ef, STDERR_FILENO); close(ef); }
    }
}

static std::string vol_flag(int v) {
    char buf[32]; snprintf(buf, sizeof(buf), "--volume=%d", v); return buf;
}

// Exec mpv with an args vector, fork-safe. Returns false on exec failure.
static bool spawn_mpv(const std::vector<std::string>& args, pid_t& out_pid, bool log_to_file, const std::string& ipc_sock = "") {
    // Inject the IPC socket flag just after argv[0] ("mpv"), BEFORE any
    // positional stream URL. Options placed after the URL can be misparsed
    // by some mpv builds, which is enough to make playback fail to start.
    std::vector<std::string> full_args;
    full_args.reserve(args.size() + 1);
    bool injected = false;
    for (size_t i = 0; i < args.size(); ++i) {
        full_args.push_back(args[i]);
        if (i == 0 && !ipc_sock.empty()) {          // right after "mpv"
            full_args.push_back("--input-ipc-server=" + ipc_sock);
            injected = true;
        }
    }
    if (!injected && !ipc_sock.empty())
        full_args.insert(full_args.begin() + (full_args.empty() ? 0 : 1),
                         "--input-ipc-server=" + ipc_sock);
    std::vector<const char*> argv;
    for (auto& a : full_args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    int exec_pipe[2];
    if (pipe(exec_pipe) < 0) {
        Log::write("exec pipe failed: %s", strerror(errno));
        return false;
    }
    fcntl(exec_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid == 0) {
        close(exec_pipe[0]);
        child_setup(log_to_file);
        execvp("mpv", (char* const*)argv.data());
        int err = errno;
        ssize_t w = write(exec_pipe[1], &err, sizeof(err)); (void)w;
        _exit(127);
    } else if (pid > 0) {
        close(exec_pipe[1]);
        int child_errno = 0;
        ssize_t n = read(exec_pipe[0], &child_errno, sizeof(child_errno));
        close(exec_pipe[0]);
        if (n > 0) {
            Log::write("mpv exec failed: %s", strerror(child_errno));
            waitpid(pid, nullptr, 0);
            return false;
        }
        usleep(10000);
        out_pid = pid;
        return true;
    } else {
        close(exec_pipe[0]); close(exec_pipe[1]);
        Log::write("fork failed: %s", strerror(errno));
        return false;
    }
}

// ─── play_piped: audio-only modes ────────────────────────────────────────────
//
// Spawns yt-dlp and pipes its stdout straight into mpv's stdin.
//
// Not reachable from the UI: the action menus offer video only (see
// App::build_actions). Kept because PlayMode::Audio / AudioLoop remain valid
// Player inputs and this is the correct implementation for them.

void Player::play_piped(const std::string& url, const std::string& title, PlayMode mode) {
    std::string vol = vol_flag(opts_.volume);

    // "bestaudio/best": this catalogue serves muxed HLS, so a strict
    // bestaudio[ext=...] chain would find no format at all.
    std::string ytdlp_cmd =
        "yt-dlp --no-warnings --no-playlist "
        "-f 'bestaudio/best' "
        "--audio-quality 0 "
        "-o - '" + url + "'";

    std::string mpv_cmd = "mpv --no-video --no-terminal " + vol;
    if (!opts_.no_cache)
        mpv_cmd += " --audio-buffer=2 --cache=yes --demuxer-max-bytes=50M";
    else
        mpv_cmd += " --cache=no";
    mpv_cmd += " --audio-pitch-correction=yes";
    if (opts_.no_hardware_accel) mpv_cmd += " --hwdec=no";
    if (mode == PlayMode::AudioLoop) mpv_cmd += " --loop=inf";
    mpv_cmd += " -";

    std::string cmd = ytdlp_cmd + " | " + mpv_cmd;
    Log::write("Piped play: %s", cmd.c_str());

    pid_t pid = fork();
    if (pid == 0) {
        child_setup(Log::is_logdump());
        execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        usleep(10000);
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
        current_volume_ = opts_.volume;
        // yt-dlp pipe path: mpv is a child of sh, IPC socket path
        // would need to be injected into the shell command. Skip IPC
        // for the legacy path — SIGSTOP fallback still works.
        Log::write("Piped play pid=%d", pid);
    } else {
        Log::write("fork failed: %s", strerror(errno));
    }
}

// ─── play_direct: video mode ──────────────────────────────────────────────────
//
// Upstream pre-resolved the stream URL with `yt-dlp -g` and handed mpv a direct
// CDN link (--ytdl=no) to skip mpv's own yt-dlp round-trip. That is wrong here:
// this catalogue serves HLS, whose manifests reference short-lived segment URLs.
// A URL resolved seconds before playback can already be stale, and mpv has no
// way to re-resolve it once it holds a bare link. So we always let mpv drive
// yt-dlp itself (--ytdl=yes) and resolve just-in-time.

void Player::play_direct(const std::string& url, const std::string& title) {
    std::string vol = vol_flag(opts_.volume);

    std::vector<std::string> args = {
        "mpv",
        "--force-window=yes",
        "--no-terminal",
        vol,
        "--geometry=854x480",
        "--autofit-larger=70%",
        "--autofit-smaller=640x360",
        "--title=" + title,
    };

    if (direct_stream_) {
        // The provider already resolved an HLS manifest URL. mpv fetches the
        // manifest and follows its segments itself, so nothing is pre-resolved
        // and nothing goes stale — yt-dlp is not involved at all.
        args.push_back("--ytdl=no");
    } else {
        args.push_back("--ytdl=yes");
        // Muxed HLS: there is no separate bestvideo+bestaudio to merge.
        args.push_back("--ytdl-format=best[height<=1080]/best");
    }

    // Provider HTTP headers (hotlink-protected CDNs return 403 without them).
    for (const auto& a : extra_args_) args.push_back(a);

    if (!opts_.no_cache) {
        args.push_back("--cache=yes");
        args.push_back("--demuxer-max-bytes=100M");
    } else {
        args.push_back("--cache=no");
    }
    if (opts_.no_hardware_accel) {
        // Disable hardware DECODING only. Do NOT set --vo=libmpv: that is
        // mpv's embedding render API, not a standalone video output, and
        // with --force-window it breaks or blanks the video window. The
        // default vo (gpu) is correct; --hwdec=no alone is the right knob.
        args.push_back("--hwdec=no");
    }
    args.push_back(url);

    Log::write("Direct play (--ytdl=yes): vol=%d %s", opts_.volume, url.c_str());

    std::string sock = ipc_.init_socket();
    if (spawn_mpv(args, mpv_pid_, Log::is_logdump(), sock)) {
        playing_       = true;
        current_title_ = title;
        current_volume_ = opts_.volume;
        ipc_.try_connect();
        Log::write("Direct play pid=%d ipc=%s", mpv_pid_, ipc_.connected() ? "ok" : "pending");
    }
}

void Player::play_xdg(const std::string& url, const std::string& title) {
    Log::write("xdg/open: %s", url.c_str());
    pid_t pid = fork();
    if (pid == 0) {
        child_setup(false);
#if defined(__APPLE__) && defined(__MACH__)
        execlp("open", "open", url.c_str(), nullptr);
#else
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
#endif
        _exit(127);
    } else if (pid > 0) {
        usleep(10000);
        mpv_pid_       = pid;
        playing_       = true;
        current_title_ = title;
    } else {
        Log::write("fork failed: %s", strerror(errno));
    }
}

void Player::kill_mpv() {
    if (mpv_pid_ <= 0) return;
    Log::write("Killing pgid -%d", mpv_pid_);
    kill(-mpv_pid_, SIGTERM);
    int status;
    pid_t r = waitpid(mpv_pid_, &status, WNOHANG);
    if (r == 0) {
        usleep(300000);
        r = waitpid(mpv_pid_, &status, WNOHANG);
        if (r == 0) {
            kill(-mpv_pid_, SIGKILL);
            waitpid(mpv_pid_, &status, 0);
        }
    }
    mpv_pid_ = -1;
}

double Player::get_position() const { return ipc_.position(); }
double Player::get_duration() const { return ipc_.duration(); }
bool   Player::have_progress() const { return ipc_.have_progress(); }

} // namespace ytui
