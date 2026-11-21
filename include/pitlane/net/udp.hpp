#pragma once

#include "pitlane/util/hint.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

// Non-blocking IPv4 UDP datagram link. The address keeps the packed IPv4
// address in network order and the port in host order. open/close are startup
// and teardown only; send/recv never allocate.

namespace pitlane {

struct UdpAddr {
    uint32_t ip_be;
    uint16_t port;
};

inline bool udp_addr(const char* ip, uint16_t port, UdpAddr& out) noexcept {
    if (!ip) return false;
    in_addr addr{};
    if (::inet_pton(AF_INET, ip, &addr) != 1) return false;
    out.ip_be = addr.s_addr;
    out.port = port;
    return true;
}

// Parses "a.b.c.d:port". A missing port means 0. Manual and bounded.
inline bool udp_parse(const char* text, UdpAddr& out) noexcept {
    if (!text) return false;
    char ip[16]{};
    std::size_t used = 0;
    std::size_t i = 0;
    while (text[i] != '\0' && text[i] != ':') {
        if (used + 1u >= sizeof(ip)) return false;
        ip[used++] = text[i++];
    }
    if (used == 0) return false;
    ip[used] = '\0';

    uint32_t port = 0;
    if (text[i] == ':') {
        ++i;
        if (text[i] == '\0') return false;
        while (text[i] != '\0') {
            if (text[i] < '0' || text[i] > '9') return false;
            port = port * 10u + static_cast<uint32_t>(text[i] - '0');
            if (port > 65535u) return false;
            ++i;
        }
    }
    return udp_addr(ip, static_cast<uint16_t>(port), out);
}

class UdpLink {
public:
    UdpLink() = default;
    ~UdpLink() { close(); }

    UdpLink(const UdpLink&) = delete;
    UdpLink& operator=(const UdpLink&) = delete;

    bool open(const UdpAddr& bind_to, const UdpAddr* peer = nullptr) noexcept {
        close();
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = bind_to.ip_be;
        a.sin_port = htons(bind_to.port);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd);
            return false;
        }
        fd_ = fd;
        if (peer && !set_peer(*peer)) {
            close();
            return false;
        }
        return true;
    }

    bool set_peer(const UdpAddr& peer) noexcept {
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = peer.ip_be;
        a.sin_port = htons(peer.port);
        return ::connect(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const noexcept { return fd_; }

    uint16_t local_port() const noexcept {
        if (fd_ < 0) return 0;
        sockaddr_in a{};
        socklen_t n = sizeof(a);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &n) != 0) return 0;
        return ntohs(a.sin_port);
    }

    // Returns bytes moved, -1 on EAGAIN/error, and 0 when closed.
    int send(const void* data, std::size_t len) noexcept {
        if (fd_ < 0) return 0;
        const int n = static_cast<int>(::send(fd_, data, len, MSG_DONTWAIT));
        return n < 0 ? -1 : n;
    }

    int recv(void* data, std::size_t len) noexcept {
        if (fd_ < 0) return 0;
        const int n = static_cast<int>(::recv(fd_, data, len, MSG_DONTWAIT));
        return n < 0 ? -1 : n;
    }

private:
    int fd_{-1};
};

}  // namespace pitlane
