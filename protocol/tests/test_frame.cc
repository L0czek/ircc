// Host unit tests for protocol/inc/frame.hpp (link framing and stream parser).

#include "frame.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

struct Got {
    frame::Type type;
    uint8_t flags;
    uint16_t seq;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> make_frame(frame::Type type, uint16_t seq, const std::vector<uint8_t>& payload,
                                uint8_t flags = 0) {
    std::vector<uint8_t> buf(frame::kMaxFrame);
    const size_t n = frame::encode(buf.data(), buf.size(), type, flags, seq, payload.data(), payload.size());
    buf.resize(n);
    return buf;
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(seed + i * 7);
    }
    return v;
}

std::vector<Got> parse(frame::Parser& p, const std::vector<uint8_t>& stream, size_t chunk) {
    std::vector<Got> out;
    for (size_t i = 0; i < stream.size(); i += chunk) {
        const size_t n = std::min(chunk, stream.size() - i);
        p.feed(stream.data() + i, n, [&](const frame::View& v) {
            out.push_back({v.type, v.flags, v.seq, std::vector<uint8_t>(v.payload, v.payload + v.length)});
        });
    }
    return out;
}

void append(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
}

void test_crc_check_value() {
    const char* s = "123456789";
    CHECK(frame::crc32_mpeg2(reinterpret_cast<const uint8_t*>(s), 9) == 0x0376E6E7u);
}

void test_round_trip_any_chunking() {
    const auto p1 = pattern(100, 1);
    const auto p2 = pattern(frame::kMaxPayload, 2);
    std::vector<uint8_t> stream;
    append(stream, make_frame(frame::Type::Request, 7, p1));
    append(stream, make_frame(frame::Type::Data, 65535, p2, 0x81));
    append(stream, make_frame(frame::Type::Event, 1, {}));
    for (size_t chunk : {size_t{1}, size_t{3}, size_t{64}, size_t{1000}, stream.size()}) {
        frame::Parser p;
        auto got = parse(p, stream, chunk);
        CHECK(got.size() == 3);
        if (got.size() == 3) {
            CHECK(got[0].type == frame::Type::Request && got[0].seq == 7 && got[0].payload == p1);
            CHECK(got[1].type == frame::Type::Data && got[1].seq == 65535 && got[1].flags == 0x81 && got[1].payload == p2);
            CHECK(got[2].type == frame::Type::Event && got[2].payload.empty());
        }
        CHECK(p.stats().crc_errors == 0 && p.stats().header_errors == 0);
    }
}

void test_garbage_and_resync() {
    const auto good = make_frame(frame::Type::Response, 42, pattern(50, 9));
    std::vector<uint8_t> stream = {'x', 'Q', 'C', 'W', 'Q', 'Q', 0, 0xFF};   // noise incl. partial magic
    append(stream, good);
    stream.push_back('Q');   // noise between frames
    append(stream, good);
    frame::Parser p;
    auto got = parse(p, stream, 5);
    CHECK(got.size() == 2);
    CHECK(p.stats().dropped_bytes > 0);
}

void test_corruptions_recover() {
    const auto f = make_frame(frame::Type::Request, 3, pattern(300, 4));
    const auto ok = make_frame(frame::Type::Request, 4, pattern(20, 5));

    auto bad_crc = f;
    bad_crc[frame::kHeaderSize + 10] ^= 0x01;   // payload bit flip
    auto bad_len = f;
    bad_len[6] ^= 0x10;                          // length no longer matches ~length
    auto huge_len = f;                           // consistent but > kMaxPayload
    frame::put_u16(huge_len.data() + 6, 4000);
    frame::put_u16(huge_len.data() + 8, static_cast<uint16_t>(~4000));
    auto bad_type = f;
    bad_type[4] = 0x7F;

    for (const auto& corrupt : {bad_crc, bad_len, huge_len, bad_type}) {
        std::vector<uint8_t> stream = corrupt;
        append(stream, ok);
        frame::Parser p;
        auto got = parse(p, stream, 17);
        CHECK(got.size() == 1);
        if (!got.empty()) {
            CHECK(got[0].seq == 4);
        }
        CHECK(p.stats().crc_errors + p.stats().header_errors >= 1);
    }
}

void test_truncated_frame_then_good() {
    auto f = make_frame(frame::Type::Request, 1, pattern(200, 1));
    f.resize(f.size() - 30);   // sender reset mid-frame
    std::vector<uint8_t> stream = f;
    append(stream, make_frame(frame::Type::Request, 2, pattern(10, 2)));
    frame::Parser p;
    auto got = parse(p, stream, 64);
    // The truncated frame claims more bytes than follow: the parser waits ...
    CHECK(got.empty());
    // ... until the link goes idle, then it rescans and finds the second frame.
    p.on_idle([&](const frame::View& v) {
        got.push_back({v.type, v.flags, v.seq, std::vector<uint8_t>(v.payload, v.payload + v.length)});
    });
    CHECK(p.idle());
    CHECK(got.size() == 1);
    if (!got.empty()) {
        CHECK(got[0].seq == 2);
    }
}

void test_encode_limits() {
    std::vector<uint8_t> buf(frame::kMaxFrame + 10);
    std::vector<uint8_t> big(frame::kMaxPayload + 1);
    CHECK(frame::encode(buf.data(), buf.size(), frame::Type::Data, 0, 0, big.data(), big.size()) == 0);
    std::vector<uint8_t> small_buf(20);
    CHECK(frame::encode(small_buf.data(), small_buf.size(), frame::Type::Data, 0, 0, big.data(), 10) == 0);
}

} // namespace

int main() {
    test_crc_check_value();
    test_round_trip_any_chunking();
    test_garbage_and_resync();
    test_corruptions_recover();
    test_truncated_frame_then_good();
    test_encode_limits();
    if (failures) {
        std::printf("frame: %d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("frame: all tests passed\n");
    return EXIT_SUCCESS;
}
