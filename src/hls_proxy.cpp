#include "hls_proxy.h"
#include "http.h"
#include "log.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace ytui {

namespace {

// Portability: accept4/SOCK_CLOEXEC and MSG_NOSIGNAL are Linux-only. macOS
// and the BSDs get close-on-exec from fcntl, and SIGPIPE suppression per socket.
static int cloexec(int fd) {
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
static void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Appended to segment names whose extension ffmpeg would refuse; see header.
constexpr const char* kSegSuffix = "~.ts";

struct Upstream {
    std::vector<std::string> headers;
    std::string              impersonate;
};

class Server {
public:
    static Server& get() { static Server s; return s; }

    bool start() {
        std::lock_guard<std::mutex> lk(mu_);
        if (port_) return true;
        if (failed_) return false;

        int fd = cloexec(socket(AF_INET, SOCK_STREAM, 0));
        if (fd < 0) { failed_ = true; return false; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;   // let the kernel pick
        socklen_t len = sizeof(a);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 16) < 0 ||
            getsockname(fd, (sockaddr*)&a, &len) < 0) {
            Log::write("[hls-proxy] could not listen on loopback: %s", strerror(errno));
            close(fd);
            failed_ = true;
            return false;
        }
        port_ = ntohs(a.sin_port);
        if (!Http::impersonation_available())
            Log::write("[hls-proxy] WARNING: libcurl is not curl-impersonate; "
                       "CDNs that check the TLS fingerprint will answer 403");
        Log::write("[hls-proxy] listening on 127.0.0.1:%d", port_);

        // Detached for the life of the process: players come and go, and a
        // proxy that outlives the last one costs an idle thread.
        std::thread([fd] {
            while (true) {
                int c = cloexec(accept(fd, nullptr, nullptr));
                if (c < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
                no_sigpipe(c);
                std::thread([c] { Server::get().serve(c); }).detach();
            }
        }).detach();
        return true;
    }

    int port() const { return port_; }

    void allow(const std::string& host, const Upstream& up) {
        std::lock_guard<std::mutex> lk(mu_);
        hosts_[host] = up;
    }

private:
    std::mutex mu_;
    int  port_   = 0;
    bool failed_ = false;
    std::map<std::string, Upstream> hosts_;
    // Keep-alive handles, reused across requests so segment fetches do not
    // each pay a fresh TLS handshake to the CDN.
    std::vector<std::unique_ptr<Http>> pool_;

    bool lookup(const std::string& host, Upstream& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = hosts_.find(host);
        if (it == hosts_.end()) return false;
        out = it->second;
        return true;
    }

    std::unique_ptr<Http> acquire(const std::string& target) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!pool_.empty()) {
                auto h = std::move(pool_.back());
                pool_.pop_back();
                return h;
            }
        }
        auto h = std::make_unique<Http>();
        h->impersonate(target);
        h->set_timeout(20);
        return h;
    }

    void release(std::unique_ptr<Http> h) {
        std::lock_guard<std::mutex> lk(mu_);
        if (pool_.size() < 8) pool_.push_back(std::move(h));
    }

    static void send_all(int fd, const char* p, size_t n) {
        while (n > 0) {
            ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
            if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
            p += w;
            n -= (size_t)w;
        }
    }

    static void reply(int fd, int status, const char* type, const std::string& body,
                      bool head) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         status, status == 200 ? "OK" : "Error", type, body.size());
        send_all(fd, hdr, (size_t)n);
        if (!head) send_all(fd, body.data(), body.size());
    }

    static bool ends_with(const std::string& s, const char* suf) {
        size_t n = strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    }

    // Extension of the path part of a URI (before any query), lowercased.
    static std::string extension(const std::string& uri) {
        std::string path = uri.substr(0, uri.find_first_of("?#"));
        size_t slash = path.find_last_of('/');
        size_t dot   = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
        std::string e = path.substr(dot + 1);
        for (auto& c : e) c = (char)tolower((unsigned char)c);
        return e;
    }

    // Point one playlist URI back at the proxy. Absolute URIs on a host we
    // already proxy become loopback URLs; relative ones resolve against the
    // proxied playlist URL by themselves. Media segments with an extension
    // ffmpeg would reject get the "~.ts" suffix.
    std::string rewrite_uri(const std::string& uri, const Upstream& up) {
        std::string out = uri;
        for (const char* scheme : {"https://", "http://"}) {
            if (out.compare(0, strlen(scheme), scheme) != 0) continue;
            std::string rest = out.substr(strlen(scheme));
            std::string host = rest.substr(0, rest.find('/'));
            // A sibling CDN host gets the same treatment as the one we were
            // given — the variant list may point anywhere on the CDN.
            allow(host, up);
            out = "http://127.0.0.1:" + std::to_string(port_) + "/" + rest;
            break;
        }
        static const char* ok_ext[] = {"m3u8", "ts", "m4s", "mp4", "m4a", "m4v",
                                       "aac", "mp3", "vtt", "key", ""};
        std::string e = extension(out);
        for (const char* k : ok_ext) if (e == k) return out;
        size_t q = out.find_first_of("?#");
        if (q == std::string::npos) return out + kSegSuffix;
        return out.substr(0, q) + kSegSuffix + out.substr(q);
    }

    std::string rewrite_playlist(const std::string& body, const Upstream& up) {
        std::string out;
        out.reserve(body.size() + body.size() / 8);
        size_t pos = 0;
        while (pos < body.size()) {
            size_t eol = body.find('\n', pos);
            if (eol == std::string::npos) eol = body.size();
            std::string line = body.substr(pos, eol - pos);
            bool cr = !line.empty() && line.back() == '\r';
            if (cr) line.pop_back();

            if (!line.empty() && line[0] != '#') {
                line = rewrite_uri(line, up);
            } else if (line.rfind("#EXT", 0) == 0) {
                // Tags carrying URI="..." (#EXT-X-KEY, -MAP, -MEDIA).
                size_t u = line.find("URI=\"");
                if (u != std::string::npos) {
                    size_t s = u + 5, e = line.find('"', s);
                    if (e != std::string::npos)
                        line = line.substr(0, s) + rewrite_uri(line.substr(s, e - s), up) +
                               line.substr(e);
                }
            }
            out += line;
            if (cr) out += '\r';
            if (eol < body.size()) out += '\n';
            pos = eol + 1;
        }
        return out;
    }

    void serve(int fd) {
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
            ssize_t r = recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; close(fd); return; }
            req.append(buf, (size_t)r);
        }

        // "GET /host/path HTTP/1.1"
        size_t sp1 = req.find(' ');
        size_t sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) { close(fd); return; }
        std::string method = req.substr(0, sp1);
        std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
        bool head = method == "HEAD";
        if ((method != "GET" && !head) || target.size() < 2 || target[0] != '/') {
            reply(fd, 400, "text/plain", "bad request\n", head);
            close(fd);
            return;
        }

        std::string rest = target.substr(1);
        std::string host = rest.substr(0, rest.find('/'));
        Upstream up;
        if (!lookup(host, up)) {
            reply(fd, 403, "text/plain", "host not proxied\n", head);
            close(fd);
            return;
        }

        std::string path = rest.substr(host.size());
        std::string query;
        size_t q = path.find('?');
        if (q != std::string::npos) { query = path.substr(q); path.resize(q); }
        bool segment = ends_with(path, kSegSuffix);
        if (segment) path.resize(path.size() - strlen(kSegSuffix));

        std::string url = "https://" + host + path + query;
        auto http = acquire(up.impersonate);
        auto r = http->get(url, up.headers);
        release(std::move(http));

        if (!r.ok()) {
            Log::write("[hls-proxy] %s -> HTTP %ld", url.c_str(), r.status);
            reply(fd, r.status ? (int)r.status : 502, "text/plain", r.body, head);
            close(fd);
            return;
        }

        if (extension(path) == "m3u8" || r.body.compare(0, 7, "#EXTM3U") == 0)
            reply(fd, 200, "application/vnd.apple.mpegurl", rewrite_playlist(r.body, up), head);
        else
            reply(fd, 200, segment ? "video/mp2t" : "application/octet-stream", r.body, head);
        close(fd);
    }
};

} // namespace

std::string HlsProxy::wrap(const std::string& url,
                           const std::vector<std::string>& headers,
                           const std::string& impersonate) {
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0) return url;
    auto& s = Server::get();
    if (!s.start()) return url;

    std::string rest = url.substr(scheme.size());
    s.allow(rest.substr(0, rest.find('/')), Upstream{headers, impersonate});
    return "http://127.0.0.1:" + std::to_string(s.port()) + "/" + rest;
}

std::string HlsProxy::pick_variant(const std::string& master_url, int max_height) {
    Http http;
    http.set_timeout(15);
    auto r = http.get(master_url);
    if (!r.ok() || r.body.find("#EXT-X-STREAM-INF") == std::string::npos) return master_url;

    // #EXT-X-STREAM-INF:...RESOLUTION=WxH...  followed by the URI line.
    std::string best, lowest;
    int best_h = -1, lowest_h = 1 << 30;
    int pending_h = -1;
    bool pending = false;
    size_t pos = 0;
    while (pos < r.body.size()) {
        size_t eol = r.body.find('\n', pos);
        if (eol == std::string::npos) eol = r.body.size();
        std::string line = r.body.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("#EXT-X-STREAM-INF", 0) == 0) {
            pending = true;
            pending_h = 0;
            size_t res = line.find("RESOLUTION=");
            if (res != std::string::npos) {
                size_t x = line.find('x', res);
                if (x != std::string::npos) pending_h = atoi(line.c_str() + x + 1);
            }
            continue;
        }
        if (line[0] == '#' || !pending) continue;
        pending = false;

        if (pending_h <= max_height && pending_h > best_h) { best_h = pending_h; best = line; }
        if (pending_h < lowest_h) { lowest_h = pending_h; lowest = line; }
    }
    std::string pick = best.empty() ? lowest : best;
    if (pick.empty()) return master_url;
    if (pick.find("://") != std::string::npos) return pick;

    std::string base = master_url.substr(0, master_url.find_first_of("?#"));
    base = base.substr(0, base.find_last_of('/') + 1);
    Log::write("[hls] rendition %dp picked for max %dp", best.empty() ? lowest_h : best_h,
               max_height);
    return base + pick;
}

} // namespace ytui
