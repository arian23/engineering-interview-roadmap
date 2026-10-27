#include "minitest.hpp"
#include "tcp_server.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using roadmap::net::Connection;
using roadmap::net::Socket;
using roadmap::net::TcpServer;

namespace {

void echo_handler(Connection& connection) {
    std::array<std::byte, 4096> buffer;
    for (;;) {
        auto n = connection.receive(buffer);
        if (!n || *n == 0) return;  // client closed, error, or server shutting down
        if (!connection.send_all(std::span(buffer).first(*n))) return;
    }
}

std::unique_ptr<TcpServer> start_server(TcpServer::Handler handler, TcpServer::Options options = {}) {
    auto server = TcpServer::start(std::move(options), std::move(handler));
    CHECK(server.has_value());
    return std::move(*server);
}

Socket connect_client(std::uint16_t port) {
    auto socket = roadmap::net::connect_tcp("127.0.0.1", port);
    CHECK(socket.has_value());
    CHECK(socket->set_receive_timeout(3s).has_value());  // a broken test fails instead of hanging
    return std::move(*socket);
}

// TCP is a byte stream: one receive() can return fewer bytes than were sent. Loop until we have `size`.
std::string read_exact(const Socket& socket, std::size_t size) {
    std::string out(size, '\0');
    std::size_t got = 0;
    while (got < size) {
        auto n = socket.receive(std::as_writable_bytes(std::span(out)).subspan(got));
        if (!n || *n == 0) break;
        got += *n;
    }
    out.resize(got);
    return out;
}

// True when the peer closed the connection (EOF or reset) rather than the read timing out.
bool reads_eof(const Socket& socket) {
    std::array<std::byte, 16> buffer;
    auto n = socket.receive(buffer);
    if (n) return *n == 0;
    return n.error() != std::errc::resource_unavailable_try_again && n.error() != std::errc::operation_would_block;
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

}  // namespace

TEST(starts_on_an_ephemeral_port) {
    auto server = start_server(echo_handler);
    CHECK(server->port() != 0);
}

TEST(echoes_data) {
    auto server = start_server(echo_handler);
    Socket client = connect_client(server->port());
    CHECK(client.send_all("hello, server").has_value());
    CHECK_EQ(read_exact(client, 13), std::string("hello, server"));
}

TEST(serves_clients_concurrently) {
    // Each handler waits until ALL clients are inside a handler at the same time.
    // A serial server could never get there, so `all_met` would stay false.
    constexpr int clients = 8;
    std::atomic<int> arrived{0};
    std::atomic<int> all_met{0};
    auto server = start_server([&](Connection&) {
        ++arrived;
        if (eventually([&] { return arrived.load() == clients; }, 2s)) ++all_met;
    });

    std::vector<Socket> sockets;
    for (int i = 0; i < clients; ++i) sockets.push_back(connect_client(server->port()));
    for (auto& socket : sockets) CHECK(reads_eof(socket));  // handlers return -> server closes
    CHECK_EQ(all_met.load(), clients);
}

TEST(many_clients_many_round_trips) {
    constexpr int clients = 16;
    constexpr int round_trips = 200;
    auto server = start_server(echo_handler);
    std::atomic<int> mismatches{0};
    {
        std::vector<std::jthread> threads;
        for (int c = 0; c < clients; ++c) {
            threads.emplace_back([&, c] {
                auto socket = roadmap::net::connect_tcp("127.0.0.1", server->port());
                if (!socket || !socket->set_receive_timeout(3s)) {
                    ++mismatches;
                    return;
                }
                for (int i = 0; i < round_trips; ++i) {
                    const std::string message = std::format("client-{:02}-msg-{:04}", c, i);
                    if (!socket->send_all(message) || read_exact(*socket, message.size()) != message) {
                        ++mismatches;
                        return;
                    }
                }
            });
        }
    }
    CHECK_EQ(mismatches.load(), 0);
    CHECK_EQ(server->total_accepted(), static_cast<std::uint64_t>(clients));
}

TEST(stop_wakes_handlers_blocked_on_silent_clients) {
    auto server = start_server(echo_handler);
    Socket idle = connect_client(server->port());  // connects but never sends anything
    CHECK(eventually([&] { return server->active_connections() == 1; }));

    const auto start = std::chrono::steady_clock::now();
    server->stop();  // must not wait for the client to say something
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    CHECK(reads_eof(idle));
    CHECK_EQ(server->active_connections(), 0u);
}

TEST(rejects_clients_beyond_max_connections) {
    auto server = start_server(echo_handler, {.max_connections = 1});
    Socket first = connect_client(server->port());
    CHECK(eventually([&] { return server->active_connections() == 1; }));

    Socket second = connect_client(server->port());  // TCP handshake succeeds (kernel backlog)...
    CHECK(reads_eof(second));                         // ...but the server closes it straight away
    CHECK_EQ(server->total_rejected(), 1u);

    CHECK(first.send_all("still here").has_value());  // the admitted client is unaffected
    CHECK_EQ(read_exact(first, 10), std::string("still here"));
}

TEST(finished_sessions_are_reaped) {
    auto server = start_server(echo_handler);
    for (int i = 0; i < 20; ++i) {
        Socket client = connect_client(server->port());
        CHECK(client.send_all("ping").has_value());
        CHECK_EQ(read_exact(client, 4), std::string("ping"));
    }  // each client closes here; its session ends and must be joined by the server
    CHECK(eventually([&] { return server->active_connections() == 0; }));
    CHECK_EQ(server->total_accepted(), 20u);
}

TEST(a_throwing_handler_only_drops_its_own_connection) {
    auto server = start_server([](Connection& connection) {
        std::array<std::byte, 64> buffer;
        auto n = connection.receive(buffer);
        if (!n || *n == 0) return;
        if (std::string_view(reinterpret_cast<const char*>(buffer.data()), *n) == "boom") {
            throw std::runtime_error("handler failure");
        }
        (void)connection.send_all(std::span(buffer).first(*n));
    });

    Socket bad = connect_client(server->port());
    CHECK(bad.send_all("boom").has_value());
    CHECK(reads_eof(bad));
    CHECK(eventually([&] { return server->handler_errors() == 1; }));

    Socket good = connect_client(server->port());
    CHECK(good.send_all("fine").has_value());
    CHECK_EQ(read_exact(good, 4), std::string("fine"));
}

TEST(stop_is_idempotent_and_thread_safe) {
    auto server = start_server(echo_handler);
    Socket client = connect_client(server->port());
    {
        std::jthread a([&] { server->stop(); });
        std::jthread b([&] { server->stop(); });
    }
    server->stop();
    CHECK(reads_eof(client));
    server.reset();  // destructor calls stop() once more
}

TEST(port_is_released_after_stop) {
    auto first = start_server(echo_handler);
    const std::uint16_t port = first->port();
    first->stop();
    auto again = TcpServer::start({.port = port}, echo_handler);
    CHECK(again.has_value());
}

TEST(start_fails_cleanly_when_port_is_taken) {
    auto first = start_server(echo_handler);
    auto second = TcpServer::start({.port = first->port()}, echo_handler);
    CHECK(!second.has_value());
    CHECK(second.error() == std::errc::address_in_use);
}

TEST(rejects_invalid_host) {
    auto server = TcpServer::start({.host = "not-an-ip"}, echo_handler);
    CHECK(!server.has_value());
    CHECK(server.error() == std::errc::invalid_argument);
}

int main() { return minitest::run_all(); }
