// =====================================================================================================
// Parse a stream of messages that may arrive partially (C++23)
// =====================================================================================================
//
// THE PROBLEM: TCP is a byte stream, not a message stream
//   If a sender calls send("HELLO") then send("WORLD"), the receiver's recv() calls may return
//   "HELLOWORLD" in one call, "HEL" + "LOWOR" + "LD" in three, or any other split. TCP keeps the BYTES
//   in order and loses none, but it has no idea where one message ends and the next begins.
//   (Nagle's algorithm, MSS segmentation, receive-buffer timing and the size of your read buffer all
//   change the split.) So every protocol on top of TCP must add FRAMING: a rule for finding message
//   boundaries inside the byte stream.
//
// TWO CLASSIC FRAMINGS (both implemented below)
//   1. Length-prefixed (binary; e.g. Kafka, gRPC over HTTP/2 frames, PostgreSQL):
//        [ 4-byte big-endian payload length ][ payload ... ]
//      + payload may contain ANY bytes; the receiver knows the exact size up front.
//   2. Delimiter-terminated (text; e.g. Redis RESP lines, SMTP, HTTP headers):
//        payload "\n"
//      + human readable; - payload must not contain the delimiter (or needs escaping);
//        the receiver must SCAN for the delimiter.
//
// THE DECODER: a state machine over an accumulation buffer
//
//      recv() chunk ──► feed(bytes) ──► buffer_ ──► next() ──► complete message | nothing yet | error
//
//   feed() only APPENDS bytes. next() looks at the unconsumed part of the buffer:
//     * length-prefixed:  need >= 4 bytes for the header -> read length L
//                         need >= 4 + L bytes            -> extract the payload, consume 4 + L
//                         otherwise                      -> NeedMoreData (wait for the next chunk)
//     * delimiter:        find "\n" in the unconsumed bytes, starting where the previous scan stopped
//                         found     -> extract up to it, consume it and the delimiter
//                         not found -> NeedMoreData
//   A single chunk may complete zero, one or many messages, and may end in the middle of a header,
//   a payload or a delimiter. The parser has to handle every one of those cases.
//
// RESULT TYPE: std::expected<std::optional<Message>, FrameError>
//   value with a message  -> a full message is ready
//   value, empty          -> "need more data" (not an error: the rest is still on the network)
//   error                 -> the stream is corrupt (oversized length, missing delimiter...). After a
//                            framing error the byte stream is out of sync, and there is no reliable way
//                            to find the next boundary, so the only safe reaction is to close the connection.
//
// SAFETY: never trust the length on the wire
//   A malicious or buggy peer can send length = 0xFFFFFFFF. Allocating 4 GiB because a header said so
//   is a remote denial-of-service. Both decoders enforce max_message_size and fail BEFORE buffering.
//   The delimiter decoder also caps how many bytes it will hold while still looking for "\n".
//
// EFFICIENCY
//   * Consumed bytes are not erased one message at a time (that would be O(n^2) on a buffer holding many
//     small messages). We advance a read offset and compact the buffer only when the consumed prefix
//     is large, so each byte is copied O(1) times on average.
//   * The delimiter decoder remembers where its last scan stopped, so a long line arriving in many tiny
//     chunks is scanned once overall instead of once per chunk.
//   * next() returns std::string payloads; a zero-copy variant would return std::span views into the
//     buffer that stay valid until the next feed().
//
// SENDING SIDE
//   encode_length_prefixed() / encode_line() build frames. Writers must loop on send() because a send
//   can be partial ("short write"); socket.hpp's send_all() from the TCP server exercise does that.
// =====================================================================================================

#include "socket.hpp"  // RAII sockets from 06-networking/sockets/concurrent-tcp-server (added to include path)

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace roadmap::framing {

enum class FrameError { MessageTooLarge, MissingDelimiter };

[[nodiscard]] constexpr std::string_view to_string(FrameError e) {
    switch (e) {
        case FrameError::MessageTooLarge: return "declared message length exceeds max_message_size";
        case FrameError::MissingDelimiter: return "no delimiter within max_message_size bytes";
    }
    return "unknown framing error";
}

using NextResult = std::expected<std::optional<std::string>, FrameError>;

/// Shared buffer handling: append at the back, consume from the front via an offset, compact lazily.
class StreamBuffer {
public:
    void append(std::span<const std::byte> bytes) {
        compact_if_worthwhile();
        const auto* first = reinterpret_cast<const char*>(bytes.data());
        data_.append(first, bytes.size());
    }
    void append(std::string_view text) { append(std::as_bytes(std::span(text.data(), text.size()))); }

    [[nodiscard]] std::string_view unread() const noexcept { return std::string_view(data_).substr(read_); }
    void consume(std::size_t n) noexcept { read_ += n; }
    [[nodiscard]] std::size_t buffered() const noexcept { return data_.size() - read_; }

private:
    // Erasing the consumed prefix costs O(remaining). Doing it only when the consumed part is at least
    // half the buffer keeps the amortised cost per byte constant.
    void compact_if_worthwhile() {
        if (read_ > 0 && read_ >= data_.size() / 2) {
            data_.erase(0, read_);
            read_ = 0;
        }
    }

    std::string data_;
    std::size_t read_ = 0;
};

/// Frames: [uint32 big-endian length][payload].
class LengthPrefixedDecoder {
public:
    static constexpr std::size_t kHeaderSize = 4;

    explicit LengthPrefixedDecoder(std::uint32_t max_message_size = 1 << 20) : max_size_(max_message_size) {}

    void feed(std::span<const std::byte> bytes) { buffer_.append(bytes); }
    void feed(std::string_view bytes) { buffer_.append(bytes); }

    [[nodiscard]] NextResult next() {
        const std::string_view unread = buffer_.unread();
        if (unread.size() < kHeaderSize) return std::optional<std::string>{};  // header incomplete

        // Decode the big-endian ("network byte order") length byte by byte: portable regardless of host
        // endianness and of buffer alignment (casting the pointer to uint32_t* could be misaligned).
        const auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(unread[i])); };
        const std::uint32_t length = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
        if (length > max_size_) return std::unexpected(FrameError::MessageTooLarge);  // reject before buffering

        if (unread.size() < kHeaderSize + length) return std::optional<std::string>{};  // payload incomplete
        std::string payload(unread.substr(kHeaderSize, length));
        buffer_.consume(kHeaderSize + length);
        return payload;
    }

    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.buffered(); }

private:
    std::uint32_t max_size_;
    StreamBuffer buffer_;
};

/// Frames: payload followed by a delimiter (default "\n"). "\r\n" also works as a delimiter.
class DelimiterDecoder {
public:
    explicit DelimiterDecoder(std::string delimiter = "\n", std::size_t max_message_size = 64 * 1024)
        : delimiter_(std::move(delimiter)), max_size_(max_message_size) {}

    void feed(std::span<const std::byte> bytes) { buffer_.append(bytes); }
    void feed(std::string_view bytes) { buffer_.append(bytes); }

    [[nodiscard]] NextResult next() {
        const std::string_view unread = buffer_.unread();
        // Resume where the last scan stopped, backing up delimiter.size() - 1 bytes in case the
        // delimiter itself was split across two chunks (e.g. "\r" at the end of one, "\n" at the start of the next).
        const std::size_t from = scanned_ >= delimiter_.size() ? scanned_ - (delimiter_.size() - 1) : 0;
        const std::size_t pos = unread.find(delimiter_, from);
        if (pos == std::string_view::npos) {
            if (unread.size() > max_size_) return std::unexpected(FrameError::MissingDelimiter);
            scanned_ = unread.size();
            return std::optional<std::string>{};  // no complete line yet
        }
        if (pos > max_size_) return std::unexpected(FrameError::MissingDelimiter);
        std::string line(unread.substr(0, pos));
        buffer_.consume(pos + delimiter_.size());
        scanned_ = 0;
        return line;
    }

    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.buffered(); }

private:
    std::string delimiter_;
    std::size_t max_size_;
    StreamBuffer buffer_;
    std::size_t scanned_ = 0;  // bytes of unread() already searched without finding the delimiter
};

// ---------------------------------------------------------------------------------------- encoders

[[nodiscard]] inline std::string encode_length_prefixed(std::string_view payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.reserve(4 + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((n >> shift) & 0xFF));
    frame.append(payload);
    return frame;
}

[[nodiscard]] inline std::string encode_line(std::string_view payload, std::string_view delimiter = "\n") {
    return std::string(payload) + std::string(delimiter);
}

}  // namespace roadmap::framing

// =====================================================================================================
// Demo
// =====================================================================================================

namespace {

using namespace roadmap::framing;

std::string printable(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
            out += std::format("\\x{:02x}", static_cast<unsigned char>(c));
        else out += c;
    }
    return out;
}

// Pulls every complete message out of a decoder. Returns false on a framing error.
template <typename Decoder>
bool drain(Decoder& decoder, std::vector<std::string>& out, std::string_view label = "") {
    for (;;) {
        auto result = decoder.next();
        if (!result) {
            std::println("{}FRAMING ERROR: {} -> close the connection", label, to_string(result.error()));
            return false;
        }
        if (!*result) return true;  // need more data
        if (!label.empty()) std::println("{}message: \"{}\"", label, printable(**result));
        out.push_back(std::move(**result));
    }
}

}  // namespace

int main() {
    // ------------------------------------------------------------------ 1. hand-made awkward chunks
    std::println("1) Length-prefixed frames arriving in awkward chunks");
    {
        using namespace std::string_literals;  // "..."s keeps embedded '\0' bytes; a plain "..." stops at the first one
        const std::string stream = encode_length_prefixed("hello") + encode_length_prefixed("") +
                                   encode_length_prefixed("bin\0ary\xff"s) + encode_length_prefixed("world");
        // Splits: inside the first header, inside a payload, a chunk holding several frames, a 1-byte tail.
        const std::vector<std::size_t> cuts = {2, 7, 9, 30, stream.size() - 1, stream.size()};
        LengthPrefixedDecoder decoder;
        std::vector<std::string> messages;
        std::size_t start = 0;
        for (std::size_t cut : cuts) {
            const std::string_view chunk = std::string_view(stream).substr(start, cut - start);
            std::println("   chunk of {:>2} bytes: \"{}\"", chunk.size(), printable(chunk));
            decoder.feed(chunk);
            (void)drain(decoder, messages, "      -> ");
            start = cut;
        }
        std::println("   total messages: {} (note the empty one and the embedded zero byte)", messages.size());
    }

    // ------------------------------------------------------------------ 2. delimiter split across chunks
    std::println("\n2) CRLF-delimited lines, with the \"\\r\\n\" itself split across two chunks");
    {
        DelimiterDecoder decoder("\r\n");
        std::vector<std::string> lines;
        for (std::string_view chunk : {"GET /index", ".html HTTP/1.1\r", "\nHost: example.com\r\n\r\n", "partial-li"}) {
            std::println("   chunk: \"{}\"", printable(chunk));
            decoder.feed(chunk);
            (void)drain(decoder, lines, "      -> ");
        }
        std::println("   still buffered (incomplete line): {} bytes", decoder.buffered());
    }

    // ------------------------------------------------------------------ 3. hostile input
    std::println("\n3) Hostile input is rejected before anything is allocated");
    {
        LengthPrefixedDecoder decoder(1024);
        decoder.feed(std::string_view("\xff\xff\xff\xff", 4));  // claims a 4 GiB payload
        std::vector<std::string> ignored;
        (void)drain(decoder, ignored, "   length-prefixed: ");

        DelimiterDecoder lines("\n", 16);
        lines.feed("this line is far too long and never ends");
        (void)drain(lines, ignored, "   delimiter:       ");
    }

    // ------------------------------------------------------------------ 4. randomized stress
    std::println("\n4) Randomized check: 5,000 random messages, random chunk sizes, both decoders");
    {
        std::mt19937 rng(2024);
        std::uniform_int_distribution<int> len(0, 300);
        std::uniform_int_distribution<int> byte(0, 255);
        std::uniform_int_distribution<int> letter('a', 'z');
        std::vector<std::string> binary, text;
        std::string binary_stream, text_stream;
        for (int i = 0; i < 5'000; ++i) {
            std::string b(static_cast<std::size_t>(len(rng)), '\0'), t(static_cast<std::size_t>(len(rng)), '\0');
            for (char& c : b) c = static_cast<char>(byte(rng));    // any byte, including '\n' and '\0'
            for (char& c : t) c = static_cast<char>(letter(rng));  // text without the delimiter
            binary_stream += encode_length_prefixed(b);
            text_stream += encode_line(t);
            binary.push_back(std::move(b));
            text.push_back(std::move(t));
        }
        auto run = [&](auto decoder, const std::string& stream, const std::vector<std::string>& expected) {
            std::uniform_int_distribution<std::size_t> chunk(1, 700);
            std::vector<std::string> got;
            for (std::size_t pos = 0; pos < stream.size();) {
                const std::size_t n = std::min(chunk(rng), stream.size() - pos);
                decoder.feed(std::string_view(stream).substr(pos, n));
                pos += n;
                if (!drain(decoder, got)) return false;
            }
            return got == expected && decoder.buffered() == 0;
        };
        std::println("   length-prefixed: {} ({:.1f} MB)", run(LengthPrefixedDecoder{}, binary_stream, binary) ? "all messages intact" : "MISMATCH",
                     static_cast<double>(binary_stream.size()) / 1e6);
        std::println("   line-delimited:  {} ({:.1f} MB)", run(DelimiterDecoder{}, text_stream, text) ? "all messages intact" : "MISMATCH",
                     static_cast<double>(text_stream.size()) / 1e6);
    }

    // ------------------------------------------------------------------ 5. over a real TCP connection
    std::println("\n5) Over a real TCP socket: sender writes frames in random-sized pieces with tiny pauses");
    {
        using namespace roadmap::net;
        auto listener = listen_tcp("127.0.0.1", 0, 1);
        if (!listener) return std::println("   listen failed: {}", listener.error().message()), 1;
        const auto port = local_port(*listener).value();

        std::jthread sender([port] {
            auto socket = connect_tcp("127.0.0.1", port);
            if (!socket) return;
            std::string stream;
            for (int i = 0; i < 10; ++i) stream += encode_length_prefixed(std::format("message #{} {}", i, std::string(static_cast<std::size_t>(i) * 3, '*')));
            std::mt19937 rng(5);
            std::uniform_int_distribution<std::size_t> piece(1, 9);
            for (std::size_t pos = 0; pos < stream.size();) {
                const std::size_t n = std::min(piece(rng), stream.size() - pos);
                if (!socket->send_all(std::string_view(stream).substr(pos, n))) return;
                pos += n;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));  // forces separate TCP segments
            }
        });  // socket closes when the sender finishes -> receiver sees EOF

        const int fd = ::accept(listener->fd(), nullptr, nullptr);
        Socket connection(fd);
        LengthPrefixedDecoder decoder;
        std::vector<std::string> messages;
        std::array<std::byte, 64> buffer;
        int reads = 0;
        for (;;) {
            auto n = connection.receive(buffer);
            if (!n || *n == 0) break;  // EOF: sender closed
            ++reads;
            decoder.feed(std::span(buffer).first(*n));
            (void)drain(decoder, messages, "   ");
        }
        std::println("   {} messages reassembled from {} recv() calls; leftover bytes: {}", messages.size(), reads,
                     decoder.buffered());
    }
}
