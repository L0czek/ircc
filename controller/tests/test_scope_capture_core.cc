// Host unit tests for scope::CaptureCore (pure logic, no HAL).
// Simulates the producer DMA (half-ring events) and the PSRAM burst writer and
// checks the data that ends up in each PSRAM region against the samples produced.

#include "scope_capture_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace scope;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

// Sample value = its absolute index (mod 2^16) so data placement can be verified.
struct Sim {
    CaptureCore core;
    CoreConfig cfg;
    std::vector<std::vector<uint16_t>> ring;     // SRAM rings
    std::vector<std::vector<uint16_t>> region;   // PSRAM regions
    std::vector<uint64_t> written;               // samples the "DMA" has produced per stream
    std::vector<uint64_t> committed;

    explicit Sim(const CoreConfig& c) : cfg(c) {
        CHECK(core.configure(c));
        ring.assign(c.num_streams, std::vector<uint16_t>(c.ring_samples, 0xDEAD));
        region.assign(c.num_streams, std::vector<uint16_t>(c.region_samples, 0xBEEF));
        written.assign(c.num_streams, 0);
        committed.assign(c.num_streams, 0);
        core.arm();
    }

    static uint16_t value(uint32_t stream, uint64_t index) {
        return static_cast<uint16_t>(index * 4 + stream);
    }

    // One sample-clock tick: every stream's DMA writes one sample.
    void tick() {
        for (uint32_t s = 0; s < cfg.num_streams; ++s) {
            ring[s][written[s] % cfg.ring_samples] = value(s, written[s]);
            ++written[s];
            if (written[s] % (cfg.ring_samples / 2) == 0) {   // HT / TC event
                committed[s] = written[s];
                core.on_produced(s, committed[s]);
            }
        }
    }

    // Run the writer until it has nothing to do (it is faster than the producer).
    void drain_writer(int max_bursts = 1 << 30) {
        for (int i = 0; i < max_bursts; ++i) {
            auto b = core.next_burst();
            if (!b) {
                return;
            }
            CHECK(b->ring_offset + b->samples <= cfg.ring_samples);       // never wraps SRAM
            CHECK(b->region_offset + b->samples <= cfg.region_samples);   // never wraps PSRAM
            for (uint32_t k = 0; k < b->samples; ++k) {
                region[b->stream][b->region_offset + k] = ring[b->stream][b->ring_offset + k];
            }
            core.burst_done(true);
        }
    }

    void stop_production() {
        std::array<uint64_t, kMaxStreams> fin{};
        for (uint32_t s = 0; s < cfg.num_streams; ++s) {
            fin[s] = written[s];
        }
        core.production_stopped(fin);
    }

    // Verify that every sample of the reported window is in PSRAM with the right value.
    void verify_window() {
        CHECK(core.state() == State::Done);
        const Window w = core.window();
        CHECK(w.end == w.trigger + cfg.post_samples);
        CHECK(w.trigger >= w.first);
        CHECK(w.trigger - w.first >= cfg.pre_samples || w.first == 0);
        for (uint32_t s = 0; s < cfg.num_streams; ++s) {
            for (uint64_t i = w.first; i < w.end; ++i) {
                const uint16_t got = region[s][i % cfg.region_samples];
                if (got != value(s, i)) {
                    std::printf("  stream %u sample %llu: got %u want %u\n", s,
                                static_cast<unsigned long long>(i), got, value(s, i));
                    CHECK(false);
                    return;
                }
            }
        }
    }
};

CoreConfig base_config() {
    CoreConfig c;
    c.num_streams = 3;
    c.ring_samples = 512;
    c.burst_samples = 64;
    c.region_samples = 4096;
    c.pre_samples = 1000;
    c.post_samples = 2000;
    c.require_full_pretrigger = true;
    return c;
}

void test_config_validation() {
    CaptureCore core;
    CoreConfig c = base_config();
    CHECK(core.configure(c));
    c.post_samples = 3000;   // pre + post + ring > region
    CHECK(!core.configure(c));
    c = base_config();
    c.ring_samples = 500;    // not a multiple of 2 * burst
    CHECK(!core.configure(c));
    c = base_config();
    c.num_streams = 5;
    CHECK(!core.configure(c));
}

// Long pre-trigger recording with many PSRAM region wraps, then trigger and drain.
void test_wrap_and_trigger() {
    Sim sim(base_config());
    for (int i = 0; i < 20000; ++i) {
        sim.tick();
        sim.drain_writer(2);   // writer interleaved with production
    }
    CHECK(sim.core.state() == State::PreTrigger);
    CHECK(sim.core.trigger(sim.written[0], 1));
    CHECK(!sim.core.trigger(sim.written[0] + 5, 2));   // only the first trigger counts
    while (!sim.core.should_stop_production()) {
        sim.tick();
        sim.drain_writer(2);
        CHECK(sim.core.state() == State::PostTrigger);
    }
    // a few extra samples arrive before the timer actually stops
    for (int i = 0; i < 37; ++i) {
        sim.tick();
    }
    sim.stop_production();
    CHECK(sim.core.state() == State::Stopping);
    sim.drain_writer();
    sim.verify_window();
    CHECK(sim.core.trigger_source() == 1);
}

// Odd number of samples at stop: final partial burst is padded to an even count.
void test_odd_final_count() {
    Sim sim(base_config());
    for (int i = 0; i < 3000; ++i) {
        sim.tick();
        sim.drain_writer(1);
    }
    CHECK(sim.core.trigger(2500, 0));
    while (!sim.core.should_stop_production()) {
        sim.tick();
        sim.drain_writer(1);
    }
    sim.tick();   // makes the count odd
    sim.stop_production();
    sim.drain_writer();
    sim.verify_window();
}

void test_holdoff() {
    Sim sim(base_config());
    for (int i = 0; i < 500; ++i) {
        sim.tick();
    }
    CHECK(!sim.core.trigger(500, 0));   // fewer than pre_samples recorded
    CHECK(sim.core.state() == State::PreTrigger);

    CoreConfig c = base_config();
    c.require_full_pretrigger = false;
    Sim early(c);
    CHECK(early.core.trigger(10, 0));
    while (!early.core.should_stop_production()) {
        early.tick();
        early.drain_writer(1);
    }
    early.stop_production();
    early.drain_writer();
    CHECK(early.core.state() == State::Done);
    CHECK(early.core.window().first == 0);
    early.verify_window();
}

// Writer starved for more than half a ring: overrun must be detected, not silent.
void test_overrun() {
    Sim sim(base_config());
    for (int i = 0; i < 2000 && sim.core.state() != State::Error; ++i) {
        sim.tick();   // no writer at all
    }
    CHECK(sim.core.state() == State::Error);
    CHECK(sim.core.fault() == Fault::Overrun);
}

void test_write_error_and_abort() {
    Sim sim(base_config());
    for (int i = 0; i < 300; ++i) {
        sim.tick();
    }
    auto b = sim.core.next_burst();
    CHECK(b.has_value());
    CHECK(!sim.core.next_burst().has_value());   // one burst in flight at a time
    sim.core.burst_done(false);
    CHECK(sim.core.state() == State::Error);
    CHECK(sim.core.fault() == Fault::WriteError);

    Sim user_stop(base_config());
    for (int i = 0; i < 300; ++i) {
        user_stop.tick();
    }
    user_stop.stop_production();   // stop before any trigger
    CHECK(user_stop.core.state() == State::Error);
    CHECK(user_stop.core.fault() == Fault::Aborted);
}

// Single stream, minimum sizes, round-robin fairness irrelevant; checks exact fit.
void test_single_stream_exact_fit() {
    CoreConfig c;
    c.num_streams = 1;
    c.ring_samples = 128;
    c.burst_samples = 32;
    c.region_samples = 384;   // ring + pre + post = 128 + 100 + 156: no spare sample
    c.pre_samples = 100;
    c.post_samples = 156;
    Sim sim(c);
    for (int i = 0; i < 1000; ++i) {
        sim.tick();
        sim.drain_writer(1);
    }
    CHECK(sim.core.trigger(sim.written[0], 3));
    while (!sim.core.should_stop_production()) {
        sim.tick();
        sim.drain_writer(1);
    }
    sim.stop_production();
    sim.drain_writer();
    sim.verify_window();
}

} // namespace

int main() {
    test_config_validation();
    test_wrap_and_trigger();
    test_odd_final_count();
    test_holdoff();
    test_overrun();
    test_write_error_and_abort();
    test_single_stream_exact_fit();
    if (failures) {
        std::printf("scope capture core: %d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("scope capture core: all tests passed\n");
    return EXIT_SUCCESS;
}
