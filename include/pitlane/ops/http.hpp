#pragma once

// Minimal HTTP/1.1 server for the ops console. One background thread, one
// request per connection, an 8 KB request cap, and no third-party dependency.
// The handler runs on the server thread and must only touch thread-safe state.

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitlane {

struct HttpReq {
    char method[8]{};
    char path[128]{};
    char query[256]{};
    const char* body{nullptr};
    uint32_t body_len{0};
};

struct HttpResp {
    int status{200};
    char content_type[32]{};
    const char* body{nullptr};
    uint32_t body_len{0};
    char small[4096]{};

    void text(int code, const char* s, uint32_t n) noexcept {
        status = code;
        std::snprintf(content_type, sizeof content_type, "text/plain; charset=utf-8");
        set(s, n);
    }

    void json(int code, const char* s, uint32_t n) noexcept {
        status = code;
        std::snprintf(content_type, sizeof content_type, "application/json; charset=utf-8");
        set(s, n);
    }

private:
    void set(const char* s, uint32_t n) noexcept {
        if (s && n < sizeof small) {
            std::memcpy(small, s, n);
            small[n] = 0;
            body = small;
            body_len = n;
        } else {
            body = s;
            body_len = n;
        }
    }
};

using HttpHandler = bool (*)(void* user, const HttpReq& req, HttpResp& resp);

class HttpServer {
public:
    static constexpr uint32_t kMaxDoc = 262144;
    static constexpr uint32_t kMaxReq = 8192;

    // `bind_addr` empty or null keeps the console on loopback; pass an IPv4
    // literal such as "0.0.0.0" to expose it to other hosts.
    bool start(uint16_t port, HttpHandler fn, void* user, const char* doc_root = nullptr,
               const char* bind_addr = nullptr) noexcept;
    void stop() noexcept;
    uint16_t port() const noexcept { return port_; }
    bool running() const noexcept { return running_; }

private:
    static void* thread_main(void* self);
    void serve(int fd) noexcept;
    void serve_static(const HttpReq& req, HttpResp& resp) noexcept;

    int listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    pthread_t thread_{};
    HttpHandler fn_{nullptr};
    void* user_{nullptr};
    char* doc_{nullptr};
    uint32_t doc_len_{0};
};

}  // namespace pitlane
