// Demo for the concurrent TCP server.
//
//   ./concurrent_tcp_server_demo           self-contained: starts a server, runs 4 concurrent clients, stops
//   ./concurrent_tcp_server_demo serve 9000  interactive: serves on port 9000 until Ctrl+C
//                                            try it with:  nc 127.0.0.1 9000

#include "tcp_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using roadmap::net::Connection;
using roadmap::net::TcpServer;

namespace {

std::mutex print_mutex;
template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    std::lock_guard lock(print_mutex);
    std::println(fmt, std::forward<Args>(args)...);
}

// Echoes every byte back in upper case. Runs on the connection's own thread.
void uppercase_echo(Connection& connection) {
    log("[server] connection #{} from {}", connection.id(), connection.peer());
    std::array<std::byte, 4096> buffer;
    for (;;) {
        auto n = connection.receive(buffer);
        if (!n || *n == 0) break;  // client closed, or server shutting down
        auto bytes = std::span(buffer).first(*n);
        std::ranges::transform(bytes, bytes.begin(), [](std::byte b) {
            return static_cast<std::byte>(std::toupper(static_cast<unsigned char>(b)));
        });
        if (!connection.send_all(bytes)) break;
    }
    log("[server] connection #{} finished", connection.id());
}

std::atomic<bool> interrupted{false};  // lock-free atomics are safe to set from a signal handler
extern "C" void on_sigint(int) { interrupted = true; }

int serve_forever(std::uint16_t port) {
    auto server = TcpServer::start({.host = "0.0.0.0", .port = port}, uppercase_echo);
    if (!server) {
        std::println("failed to start: {}", server.error().message());
        return 1;
    }
    std::signal(SIGINT, on_sigint);
    std::println("serving on port {} - try:  nc 127.0.0.1 {}   (Ctrl+C to stop)", (*server)->port(),
                 (*server)->port());
    while (!interrupted) std::this_thread::sleep_for(100ms);
    std::println("\nstopping: active connections = {}", (*server)->active_connections());
    (*server)->stop();
    std::println("stopped cleanly");
    return 0;
}

int self_contained_demo() {
    auto server = TcpServer::start({.max_connections = 8}, uppercase_echo);
    if (!server) {
        std::println("failed to start: {}", server.error().message());
        return 1;
    }
    const std::uint16_t port = (*server)->port();
    log("[main] server listening on 127.0.0.1:{}", port);

    {
        std::vector<std::jthread> clients;
        for (int id = 0; id < 4; ++id) {
            clients.emplace_back([port, id] {
                auto socket = roadmap::net::connect_tcp("127.0.0.1", port);
                if (!socket) return log("[client {}] connect failed: {}", id, socket.error().message());
                (void)socket->set_receive_timeout(2s);
                for (int n = 0; n < 3; ++n) {
                    const std::string message = std::format("hello from client {} #{}", id, n);
                    if (!socket->send_all(message)) return;
                    std::string reply(message.size(), '\0');
                    std::size_t got = 0;
                    while (got < reply.size()) {  // TCP may deliver the reply in pieces
                        auto r = socket->receive(std::as_writable_bytes(std::span(reply)).subspan(got));
                        if (!r || *r == 0) return;
                        got += *r;
                    }
                    log("[client {}] sent \"{}\" got \"{}\"", id, message, reply);
                    std::this_thread::sleep_for(20ms);
                }
            });
        }
    }  // clients disconnect here

    log("[main] accepted={} rejected={} active={} (handlers may not have noticed EOF yet)",
        (*server)->total_accepted(), (*server)->total_rejected(), (*server)->active_connections());
    (*server)->stop();
    log("[main] server stopped; all threads joined");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "serve") {
        const int port = argc >= 3 ? std::atoi(argv[2]) : 9000;
        return serve_forever(static_cast<std::uint16_t>(port));
    }
    return self_contained_demo();
}
