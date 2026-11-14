#include "pitlane/ops/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>

namespace pitlane {

namespace {

const char* status_text(int code) noexcept {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
    }
    return "Error";
}

bool ieq_n(const char* a, const char* b, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool starts_with_ci(const char* s, const char* prefix) noexcept {
    std::size_t n = 0;
    while (prefix[n]) ++n;
    return ieq_n(s, prefix, n);
}

void send_all(int fd, const char* data, std::size_t len) noexcept {
    while (len > 0) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
}

void fail(int fd, int code) noexcept {
    char body[64];
    const int n = std::snprintf(body, sizeof body, "%d %s\n", code, status_text(code));
    char head[256];
    const int h = std::snprintf(head, sizeof head,
                                "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                                "Connection: close\r\n\r\n",
                                code, status_text(code), n);
    send_all(fd, head, static_cast<std::size_t>(h));
    send_all(fd, body, static_cast<std::size_t>(n));
}

}  // namespace

void* HttpServer::thread_main(void* self) {
    auto* srv = static_cast<HttpServer*>(self);
    for (;;) {
        pollfd pfd{};
        pfd.fd = srv->listen_fd_;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (srv->stop_.load(std::memory_order_acquire)) break;
        if (rc == 0) continue;
        sockaddr_in addr{};
        socklen_t alen = sizeof addr;
        const int fd = ::accept(srv->listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
        if (fd < 0) continue;
        srv->serve(fd);
        ::close(fd);
    }
    return nullptr;
}

void HttpServer::serve_static(const HttpReq& req, HttpResp& resp) noexcept {
    if (std::strcmp(req.path, "/favicon.ico") == 0) {
        resp.status = 204;
        resp.body = nullptr;
        resp.body_len = 0;
        return;
    }
    if (std::strcmp(req.path, "/") == 0 || std::strcmp(req.path, "/index.html") == 0) {
        if (!doc_) {
            static const char kMissing[] = "ops console document missing\n";
            resp.text(503, kMissing, sizeof kMissing - 1);
            return;
        }
        resp.status = 200;
        std::snprintf(resp.content_type, sizeof resp.content_type, "text/html; charset=utf-8");
        resp.body = doc_;
        resp.body_len = doc_len_;
        return;
    }
    if (fn_ && fn_(user_, req, resp)) return;
    if (resp.status == 200 && !resp.body) {
        static const char kNotFound[] = "not found\n";
        resp.text(404, kNotFound, sizeof kNotFound - 1);
    }
}

void HttpServer::serve(int fd) noexcept {
    timeval tv{};
    tv.tv_sec = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char buf[kMaxReq + 1];
    std::size_t len = 0;
    char* hdr_end = nullptr;
    while (len < kMaxReq) {
        const ssize_t n = ::recv(fd, buf + len, kMaxReq - len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(fd, 400);
            return;
        }
        if (n == 0) break;
        len += static_cast<std::size_t>(n);
        buf[len] = 0;
        hdr_end = std::strstr(buf, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (!hdr_end) {
        fail(fd, len >= kMaxReq ? 413 : 400);
        return;
    }

    HttpReq req{};
    char* line_end = std::strstr(buf, "\r\n");
    if (!line_end) {
        fail(fd, 400);
        return;
    }
    *line_end = 0;
    char method[8]{};
    char target[512]{};
    if (std::sscanf(buf, "%7s %511s", method, target) != 2) {
        fail(fd, 400);
        return;
    }
    if (std::strcmp(method, "GET") != 0 && std::strcmp(method, "POST") != 0) {
        fail(fd, 405);
        return;
    }
    std::snprintf(req.method, sizeof req.method, "%s", method);

    char* qmark = std::strchr(target, '?');
    if (qmark) {
        *qmark = 0;
        std::snprintf(req.query, sizeof req.query, "%s", qmark + 1);
    }
    if (target[0] != '/' || std::strlen(target) >= sizeof req.path || std::strstr(target, "..")) {
        fail(fd, 400);
        return;
    }
    std::snprintf(req.path, sizeof req.path, "%s", target);

    uint32_t content_length = 0;
    char* p = line_end + 2;
    while (p < hdr_end) {
        char* e = std::strstr(p, "\r\n");
        if (!e || e > hdr_end) break;
        *e = 0;
        if (starts_with_ci(p, "content-length:")) {
            const char* v = p + 15;
            while (*v == ' ' || *v == '\t') ++v;
            long long cl = 0;
            char* end = nullptr;
            cl = std::strtoll(v, &end, 10);
            if (!end || *end || cl < 0) {
                fail(fd, 400);
                return;
            }
            if (cl > static_cast<long long>(kMaxReq)) {
                fail(fd, 413);
                return;
            }
            content_length = static_cast<uint32_t>(cl);
        }
        p = e + 2;
    }
    if (std::strcmp(req.method, "POST") == 0 && content_length == 0) {
        fail(fd, 400);
        return;
    }

    const std::size_t body_off = static_cast<std::size_t>(hdr_end - buf) + 4;
    while (len < body_off + content_length) {
        const ssize_t n = ::recv(fd, buf + len, kMaxReq - len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail(fd, 400);
            return;
        }
        if (n == 0) break;
        len += static_cast<std::size_t>(n);
    }
    if (len < body_off + content_length) {
        fail(fd, 400);
        return;
    }
    if (content_length > 0) {
        req.body = buf + body_off;
        req.body_len = content_length;
    }

    HttpResp resp{};
    if (std::strcmp(req.method, "GET") == 0) serve_static(req, resp);
    else if (fn_ && fn_(user_, req, resp)) {
        // handled
    } else {
        static const char kNotFound[] = "not found\n";
        resp.text(404, kNotFound, sizeof kNotFound - 1);
    }
    if (resp.status >= 200 && resp.status != 204 && !resp.body) {
        static const char kEmpty[] = "{\"code\":-1}";
        resp.json(200, kEmpty, sizeof kEmpty - 1);
    }

    char head[320];
    const int h = std::snprintf(head, sizeof head,
                                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                                resp.status, status_text(resp.status),
                                resp.content_type[0] ? resp.content_type : "application/octet-stream", resp.body_len);
    send_all(fd, head, static_cast<std::size_t>(h));
    if (resp.body && resp.body_len > 0) send_all(fd, resp.body, resp.body_len);
}

bool HttpServer::start(uint16_t port, HttpHandler fn, void* user, const char* doc_root,
                       const char* bind_addr) noexcept {
    if (running_) return false;
    fn_ = fn;
    user_ = user;
    port_ = port;

    if (doc_root && doc_root[0]) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/index.html", doc_root);
        std::FILE* f = std::fopen(path, "rb");
        if (f) {
            if (std::fseek(f, 0, SEEK_END) == 0) {
                const long sz = std::ftell(f);
                if (sz > 0 && static_cast<unsigned long>(sz) <= kMaxDoc && std::fseek(f, 0, SEEK_SET) == 0) {
                    doc_ = static_cast<char*>(std::malloc(static_cast<std::size_t>(sz)));
                    if (doc_ && std::fread(doc_, 1, static_cast<std::size_t>(sz), f) == static_cast<std::size_t>(sz)) {
                        doc_len_ = static_cast<uint32_t>(sz);
                    }
                }
            }
            std::fclose(f);
        }
    }

    ::signal(SIGPIPE, SIG_IGN);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind_addr && bind_addr[0] && ::inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    addr.sin_port = htons(port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(listen_fd_, 16) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof bound;
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) port_ = ntohs(bound.sin_port);

    stop_.store(false, std::memory_order_release);
    running_ = true;
    if (pthread_create(&thread_, nullptr, &HttpServer::thread_main, this) != 0) {
        running_ = false;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    return true;
}

void HttpServer::stop() noexcept {
    if (!running_) return;
    stop_.store(true, std::memory_order_release);
    pthread_join(thread_, nullptr);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (doc_) {
        std::free(doc_);
        doc_ = nullptr;
        doc_len_ = 0;
    }
    running_ = false;
}

}  // namespace pitlane
