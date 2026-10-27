#pragma once
// Thin RAII + std::expected wrappers over POSIX sockets (IPv4, TCP).
//
// Why a wrapper at all?
//   * RAII: a Socket closes its file descriptor exactly once, even on early returns/exceptions.
//   * Errors as values: every call returns std::expected<T, std::error_code> instead of -1 + errno,
//     so callers cannot forget to check, and errno is captured immediately (before it is clobbered).
//   * Portability fixes in one place: EINTR retries, SIGPIPE suppression (macOS vs Linux),
//     and macOS's habit of making accepted sockets inherit O_NONBLOCK from the listener.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace roadmap::net {

/// Captures errno as a std::error_code. Call it immediately after the failing system call.
[[nodiscard]] inline std::error_code last_error() noexcept { return {errno, std::system_category()}; }

// Writing to a socket whose peer has gone away raises SIGPIPE, whose default action KILLS the
// process. Linux lets us opt out per call (MSG_NOSIGNAL); macOS/BSD opt out per socket (SO_NOSIGPIPE).
#ifdef MSG_NOSIGNAL
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#else
inline constexpr int kSendFlags = 0;
#endif

inline void disable_sigpipe([[maybe_unused]] int fd) noexcept {
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

inline std::expected<void, std::error_code> set_nonblocking(int fd, bool enabled) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return std::unexpected(last_error());
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd, F_SETFL, flags) < 0) return std::unexpected(last_error());
    return {};
}

/// Prevents the descriptor from leaking into child processes started with exec().
inline void set_close_on_exec(int fd) noexcept { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }

/// Disables Nagle's algorithm: small writes are sent immediately instead of being batched.
inline void set_no_delay(int fd) noexcept {
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

/// Owns a socket file descriptor. Move-only.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    void reset() noexcept {
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
    }

    /// Shuts down both directions WITHOUT closing the descriptor.
    /// Any thread blocked in receive() or send_all() on this socket wakes up immediately
    /// (receive returns 0, send fails with EPIPE). Safe to call from another thread.
    void shutdown_both() const noexcept {
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    }

    /// Reads up to buffer.size() bytes. Returns 0 when the peer has closed its side (EOF).
    /// TCP is a byte stream: one receive() may return part of a message or several messages.
    [[nodiscard]] std::expected<std::size_t, std::error_code> receive(std::span<std::byte> buffer) const {
        for (;;) {
            const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
            if (n >= 0) return static_cast<std::size_t>(n);
            if (errno == EINTR) continue;  // interrupted by a signal: just retry
            return std::unexpected(last_error());
        }
    }

    /// Writes ALL bytes. send() may accept fewer bytes than asked ("short write"), so we loop.
    [[nodiscard]] std::expected<void, std::error_code> send_all(std::span<const std::byte> data) const {
        while (!data.empty()) {
            const ssize_t n = ::send(fd_, data.data(), data.size(), kSendFlags);
            if (n < 0) {
                if (errno == EINTR) continue;
                return std::unexpected(last_error());
            }
            data = data.subspan(static_cast<std::size_t>(n));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::error_code> send_all(std::string_view text) const {
        return send_all(std::as_bytes(std::span(text.data(), text.size())));
    }

    /// After this, receive() fails with EAGAIN/EWOULDBLOCK if no data arrives within `timeout`.
    std::expected<void, std::error_code> set_receive_timeout(std::chrono::milliseconds timeout) const {
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) return std::unexpected(last_error());
        return {};
    }

private:
    int fd_ = -1;
};

[[nodiscard]] inline std::expected<sockaddr_in, std::error_code> make_address(std::string_view host,
                                                                            std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);  // network byte order (big-endian)
    const std::string ip = (host == "localhost") ? std::string("127.0.0.1") : std::string(host);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    return addr;
}

[[nodiscard]] inline std::string to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
    return std::format("{}:{}", ip, ntohs(addr.sin_port));
}

/// The port the socket is actually bound to (useful after binding to port 0 = "any free port").
[[nodiscard]] inline std::expected<std::uint16_t, std::error_code> local_port(const Socket& socket) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(socket.fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return std::unexpected(last_error());
    }
    return ntohs(addr.sin_port);
}

/// socket() + bind() + listen(). Port 0 asks the kernel for any free port.
[[nodiscard]] inline std::expected<Socket, std::error_code> listen_tcp(std::string_view host, std::uint16_t port,
                                                                      int backlog) {
    auto addr = make_address(host, port);
    if (!addr) return std::unexpected(addr.error());

    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket) return std::unexpected(last_error());
    set_close_on_exec(socket.fd());

    // Allows an immediate restart on the same port while old connections sit in TIME_WAIT.
    int on = 1;
    ::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    if (::bind(socket.fd(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) < 0) {
        return std::unexpected(last_error());
    }
    if (::listen(socket.fd(), backlog) < 0) return std::unexpected(last_error());
    return socket;
}

/// socket() + connect(). A blocking client connection.
[[nodiscard]] inline std::expected<Socket, std::error_code> connect_tcp(std::string_view host, std::uint16_t port) {
    auto addr = make_address(host, port);
    if (!addr) return std::unexpected(addr.error());

    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    if (!socket) return std::unexpected(last_error());
    set_close_on_exec(socket.fd());
    disable_sigpipe(socket.fd());
    set_no_delay(socket.fd());

    if (::connect(socket.fd(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) < 0) {
        return std::unexpected(last_error());
    }
    return socket;
}

/// The "self-pipe trick": a pipe whose read end sits in poll() next to the listening socket.
/// Writing one byte to it wakes the poller from any thread, e.g. to request shutdown.
class WakePipe {
public:
    [[nodiscard]] static std::expected<WakePipe, std::error_code> create() {
        int fds[2];
        if (::pipe(fds) < 0) return std::unexpected(last_error());
        WakePipe pipe(fds[0], fds[1]);
        for (int fd : fds) {
            set_close_on_exec(fd);
            if (auto ok = set_nonblocking(fd, true); !ok) return std::unexpected(ok.error());
        }
        return pipe;
    }

    void notify() const noexcept {
        const char byte = 1;
        [[maybe_unused]] ssize_t ignored = ::write(write_.fd(), &byte, 1);  // EAGAIN = already signalled
    }

    void drain() const noexcept {
        char buffer[64];
        while (::read(read_.fd(), buffer, sizeof buffer) > 0) {
        }
    }

    [[nodiscard]] int read_fd() const noexcept { return read_.fd(); }

private:
    WakePipe(int read_fd, int write_fd) : read_(read_fd), write_(write_fd) {}
    Socket read_;  // Socket is just an owning fd wrapper; close() works for pipes too
    Socket write_;
};

}  // namespace roadmap::net
