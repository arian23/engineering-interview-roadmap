#pragma once
// TcpServer — a concurrent TCP server (one thread per connection) with explicit lifecycle management.
//
//   auto server = TcpServer::start({.port = 8080}, [](Connection& c) { /* talk to one client */ });
//   ...
//   server.value()->stop();   // graceful: stop accepting, wake every connection, join every thread
//
// Lifecycle (see README.md for diagrams):
//   start()  binds + listens, then launches the accept thread.
//   accept   poll()s the listening socket AND a wake-up pipe; each new client gets a session thread.
//   reap     finished session threads are joined by the accept thread (a thread cannot join itself).
//   stop()   wakes the accept thread, shuts down every client socket (which unblocks their reads),
//            asks every session to stop, and joins all threads. Idempotent; called by the destructor.

#include "socket.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace roadmap::net {

/// What a handler sees: one connected client. Only valid during the handler call.
class Connection {
public:
    Connection(std::uint64_t id, std::shared_ptr<const Socket> socket, std::string peer, std::stop_token stop)
        : id_(id), socket_(std::move(socket)), peer_(std::move(peer)), stop_(std::move(stop)) {}
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

    /// True once the server is shutting down. Long-running handlers should check it.
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.stop_requested(); }

    /// Returns 0 when the client closed the connection OR the server is shutting down.
    [[nodiscard]] std::expected<std::size_t, std::error_code> receive(std::span<std::byte> buffer) const {
        return socket_->receive(buffer);
    }
    [[nodiscard]] std::expected<void, std::error_code> send_all(std::span<const std::byte> data) const {
        return socket_->send_all(data);
    }
    [[nodiscard]] std::expected<void, std::error_code> send_all(std::string_view text) const {
        return socket_->send_all(text);
    }

private:
    std::uint64_t id_;
    std::shared_ptr<const Socket> socket_;
    std::string peer_;
    std::stop_token stop_;
};

class TcpServer {
public:
    struct Options {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;                     // 0 = let the kernel pick a free port
        std::size_t max_connections = 64;           // extra clients are accepted and closed immediately
        int backlog = 128;                          // kernel queue of not-yet-accepted connections
        std::chrono::milliseconds reap_interval{50};  // how often finished sessions are joined
    };

    /// Called once per connection, on that connection's own thread, concurrently with other
    /// connections. It is `const`-callable: shared state it touches must be thread-safe.
    using Handler = std::move_only_function<void(Connection&) const>;

    [[nodiscard]] static std::expected<std::unique_ptr<TcpServer>, std::error_code> start(Options options,
                                                                                       Handler handler) {
        auto listener = listen_tcp(options.host, options.port, options.backlog);
        if (!listener) return std::unexpected(listener.error());
        // Non-blocking so accept() can never hang after poll() reported readiness
        // (the client may have reset the connection in between).
        if (auto ok = set_nonblocking(listener->fd(), true); !ok) return std::unexpected(ok.error());

        auto port = local_port(*listener);
        if (!port) return std::unexpected(port.error());
        auto wake = WakePipe::create();
        if (!wake) return std::unexpected(wake.error());

        std::unique_ptr<TcpServer> server(
            new TcpServer(std::move(options), std::move(handler), std::move(*listener), std::move(*wake), *port));
        server->accept_thread_ = std::jthread([raw = server.get()](std::stop_token stop) { raw->accept_loop(stop); });
        return server;
    }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    ~TcpServer() { stop(); }

    /// Graceful shutdown. Blocks until every thread has finished. Safe to call more than once
    /// and from several threads: std::call_once makes concurrent callers wait for completion.
    void stop() {
        std::call_once(stop_once_, [this] { shutdown(); });
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// Sessions whose handler is still running.
    [[nodiscard]] std::size_t active_connections() const {
        std::lock_guard lock(mutex_);
        return sessions_.size() - finished_.size();
    }
    [[nodiscard]] std::uint64_t total_accepted() const noexcept { return accepted_.load(); }
    [[nodiscard]] std::uint64_t total_rejected() const noexcept { return rejected_.load(); }
    [[nodiscard]] std::uint64_t handler_errors() const noexcept { return handler_errors_.load(); }

private:
    struct Session {
        std::shared_ptr<const Socket> socket;  // shared with the Connection given to the handler
        std::jthread thread;
    };

    TcpServer(Options options, Handler handler, Socket listener, WakePipe wake, std::uint16_t port)
        : options_(std::move(options)),
          handler_(std::move(handler)),
          listener_(std::move(listener)),
          wake_(std::move(wake)),
          port_(port) {}

    // ------------------------------------------------------------------ accept thread

    void accept_loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            pollfd fds[2] = {{listener_.fd(), POLLIN, 0}, {wake_.read_fd(), POLLIN, 0}};
            const int ready = ::poll(fds, 2, static_cast<int>(options_.reap_interval.count()));
            reap_finished();  // runs on every wake-up and at least every reap_interval
            if (ready < 0) {
                if (errno == EINTR) continue;
                return;  // unrecoverable poll error: stop accepting
            }
            if (fds[1].revents != 0) {
                wake_.drain();
                continue;  // loop condition re-checks stop
            }
            if ((fds[0].revents & POLLIN) != 0) accept_pending();
        }
    }

    // Accepts every connection currently waiting in the kernel backlog.
    void accept_pending() {
        for (;;) {
            sockaddr_in addr{};
            socklen_t len = sizeof addr;
            const int fd = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) continue;  // transient
                return;  // EAGAIN: backlog empty. EMFILE/ENFILE: out of descriptors, retry later.
            }

            Socket client(fd);
            // macOS/BSD: an accepted socket INHERITS O_NONBLOCK from the listener. Handlers use
            // blocking I/O, so clear it explicitly (Linux would not need this, but it is harmless).
            (void)set_nonblocking(client.fd(), false);
            set_close_on_exec(client.fd());
            disable_sigpipe(client.fd());
            set_no_delay(client.fd());
            ++accepted_;
            start_session(std::move(client), to_string(addr));
        }
    }

    void start_session(Socket client, std::string peer) {
        std::unique_lock lock(mutex_);
        if (sessions_.size() - finished_.size() >= options_.max_connections) {
            lock.unlock();
            ++rejected_;  // load shedding: closing the socket tells the client to go away
            return;
        }

        const std::uint64_t id = next_id_++;
        auto socket = std::make_shared<const Socket>(std::move(client));
        Session& session = sessions_[id];
        session.socket = socket;
        try {
            // The session thread is created while we hold the mutex. Its last action is to lock the
            // mutex and report itself finished, so it can never be reaped before this assignment.
            session.thread = std::jthread([this, id, socket, peer = std::move(peer)](std::stop_token stop) mutable {
                run_session(id, std::move(socket), std::move(peer), std::move(stop));
            });
        } catch (const std::system_error&) {  // could not create a thread (resource limit)
            sessions_.erase(id);
            ++rejected_;
        }
    }

    // ------------------------------------------------------------------ session threads

    void run_session(std::uint64_t id, std::shared_ptr<const Socket> socket, std::string peer,
                     std::stop_token stop) {
        {
            Connection connection(id, socket, std::move(peer), std::move(stop));
            try {
                handler_(connection);
            } catch (...) {
                ++handler_errors_;  // one bad connection must never take the whole server down
            }
        }
        socket->shutdown_both();  // the client sees EOF now; the fd itself is closed when reaped
        std::lock_guard lock(mutex_);
        // Invariant: finished_ only holds ids that are still in sessions_. During stop() the map has
        // already been taken over by shutdown(), which joins this thread itself, so do not report.
        if (sessions_.contains(id)) finished_.push_back(id);
    }

    // Joins the threads of sessions that have finished. Runs on the accept thread.
    void reap_finished() {
        std::vector<Session> done;
        {
            std::lock_guard lock(mutex_);
            for (std::uint64_t id : finished_) {
                if (auto node = sessions_.extract(id)) done.push_back(std::move(node.mapped()));
            }
            finished_.clear();
        }
        // `done` is destroyed here, OUTSIDE the lock: each jthread destructor joins its thread,
        // and each Socket closes its descriptor once the last shared_ptr goes away.
    }

    // ------------------------------------------------------------------ shutdown

    void shutdown() {
        // 1. Stop accepting: wake the accept thread out of poll() and wait for it to exit.
        //    After this, no new session can be created.
        accept_thread_.request_stop();
        wake_.notify();
        if (accept_thread_.joinable()) accept_thread_.join();

        // 2. Signal every live session. shutdown_both() makes any blocked receive() return 0,
        //    so even a handler waiting for a silent client wakes up.
        std::unordered_map<std::uint64_t, Session> remaining;
        {
            std::lock_guard lock(mutex_);
            for (auto& [id, session] : sessions_) {
                session.thread.request_stop();
                session.socket->shutdown_both();
            }
            remaining.swap(sessions_);
            finished_.clear();
        }

        // 3. Join every session thread (jthread destructors), outside the lock: a session that is
        //    finishing needs the mutex to report itself done.
        remaining.clear();

        // 4. Release the listening port.
        listener_.reset();
    }

    Options options_;
    const Handler handler_;
    Socket listener_;
    WakePipe wake_;
    std::uint16_t port_;

    mutable std::mutex mutex_;  // guards sessions_, finished_, next_id_
    std::unordered_map<std::uint64_t, Session> sessions_;
    std::vector<std::uint64_t> finished_;  // invariant: every id here is also a key of sessions_
    std::uint64_t next_id_ = 1;

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> handler_errors_{0};

    std::once_flag stop_once_;
    std::jthread accept_thread_;
};

}  // namespace roadmap::net
