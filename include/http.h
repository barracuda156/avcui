#pragma once
// ─── Native HTTP client ───────────────────────────────────────────────────────
// A thin persistent-handle libcurl wrapper, modelled on ytcui-dl's
// ytfast_http.h (which is the only genuinely reusable piece of that library —
// the other 1500 lines are a YouTube-specific InnerTube client).
//
// One CURL handle per instance, so successive requests reuse the TCP+TLS
// connection. That is the whole performance argument for going native: no
// process spawn, no Python interpreter start (~0.4s each), no reconnect.
//
// BUILD: needs libcurl. Link -lcurl, or link curl-impersonate's libcurl to get
// browser TLS fingerprints (see impersonate() below).

#include <string>
#include <vector>
#include <dlfcn.h>
#include <curl/curl.h>

namespace ytui {

class Http {
public:
    struct Response {
        long        status = 0;
        std::string body;
        bool ok() const { return status >= 200 && status < 300; }
    };

    Http() {
        curl_ = curl_easy_init();
        if (!curl_) return;
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE,   1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT,  8L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT,         30L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION,  1L);
        curl_easy_setopt(curl_, CURLOPT_MAXREDIRS,       5L);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER,  1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST,  2L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION,   write_cb);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL,        1L);
    }
    ~Http() { if (curl_) curl_easy_cleanup(curl_); }

    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    // Shorter deadline for interactive paths: the default 30s would freeze the
    // UI thread on a hung connection.
    void set_timeout(long seconds) {
        if (curl_) curl_easy_setopt(curl_, CURLOPT_TIMEOUT, seconds);
    }

    // Ask curl-impersonate to present a browser's TLS/HTTP2 fingerprint, e.g.
    // "chrome131", along with that browser's default headers (ours still win
    // where both set one). This is the standalone C library — nothing to do
    // with Python's curl_cffi, and not subject to yt-dlp's version gate.
    //
    // curl-impersonate exposes this as a FUNCTION, curl_easy_impersonate(), not
    // a curl option, so it is looked up at run time: the same source builds and
    // runs against stock libcurl, where this is a no-op returning false.
    bool impersonate(const std::string& target) {
        if (!curl_ || target.empty()) return false;
        auto fn = impersonate_fn();
        return fn && fn(curl_, target.c_str(), 1) == CURLE_OK;
    }

    // Whether the libcurl this process loaded is curl-impersonate. Sites behind
    // Cloudflare's bot rules (MissAV's pages and its stream CDN) reject stock
    // libcurl's TLS fingerprint whatever headers it sends.
    static bool impersonation_available() { return impersonate_fn() != nullptr; }

    Response get(const std::string& url, const std::vector<std::string>& headers = {}) {
        return perform(url, headers, nullptr);
    }

    Response post_json(const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers = {}) {
        return perform(url, headers, &body);
    }

    static std::string url_encode(const std::string& s) {
        CURL* c = curl_easy_init();
        if (!c) return s;
        char* enc = curl_easy_escape(c, s.c_str(), (int)s.size());
        std::string out = enc ? enc : s;
        if (enc) curl_free(enc);
        curl_easy_cleanup(c);
        return out;
    }

private:
    CURL* curl_ = nullptr;

    using ImpersonateFn = CURLcode (*)(CURL*, const char*, int);
    static ImpersonateFn impersonate_fn() {
        static ImpersonateFn fn = [] () -> ImpersonateFn {
            void* imp  = dlsym(RTLD_DEFAULT, "curl_easy_impersonate");
            void* init = dlsym(RTLD_DEFAULT, "curl_easy_init");
            // Only usable if our handles come from the same library: with both
            // stock libcurl and curl-impersonate loaded, curl_easy_init may
            // resolve to the stock one, and its handle would crash the other.
            Dl_info a{}, b{};
            if (!imp || !init || !dladdr(imp, &a) || !dladdr(init, &b) ||
                a.dli_fbase != b.dli_fbase)
                return nullptr;
            return reinterpret_cast<ImpersonateFn>(imp);
        }();
        return fn;
    }
    std::string buf_;

    static size_t write_cb(char* p, size_t sz, size_t n, void* ud) {
        auto* out = static_cast<std::string*>(ud);
        out->append(p, sz * n);
        return sz * n;
    }

    Response perform(const std::string& url,
                     const std::vector<std::string>& headers,
                     const std::string* post_body) {
        Response r;
        if (!curl_) return r;
        buf_.clear();

        curl_easy_setopt(curl_, CURLOPT_URL,       url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &buf_);
        if (post_body) {
            curl_easy_setopt(curl_, CURLOPT_POST,          1L);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,    post_body->c_str());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)post_body->size());
        } else {
            curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        }

        struct curl_slist* hdrs = nullptr;
        for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hdrs);

        CURLcode rc = curl_easy_perform(curl_);
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
        if (hdrs) curl_slist_free_all(hdrs);
        // Reset so a later GET on this handle isn't still a POST.
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);

        if (rc != CURLE_OK) { r.status = 0; return r; }
        r.body.swap(buf_);
        return r;
    }
};

// Process-wide curl init/teardown. Declare ONE of these in main() — libcurl's
// global init is not thread-safe and must outlive every Http instance.
struct CurlGlobal {
    CurlGlobal()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

} // namespace ytui
