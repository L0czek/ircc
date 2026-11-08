// Host unit test for controller/inc/usb_pd_policy.hpp (PDO selection and encodings)
#include "usb_pd_policy.hpp"
#include <cstdio>

using namespace usb_pd;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static constexpr uint32_t k5V3A = 0x0001912C;
static constexpr uint32_t k9V3A = 0x0002D12C;
static constexpr uint32_t k15V3A = 0x0004B12C;
static constexpr uint32_t k20V3A = 0x0006412C;
static constexpr uint32_t fixed(uint32_t mv, uint32_t ma) { return ((mv / 50) << 10) | (ma / 10); }
static constexpr uint32_t pps(uint32_t min_mv, uint32_t max_mv, uint32_t ma) {
    return (3u << 30) | ((max_mv / 100) << 17) | ((min_mv / 100) << 8) | (ma / 50);
}

int main() {
    // Decoding
    CHECK(decode_pdo(k5V3A).type == PdoType::Fixed && decode_pdo(k5V3A).max_mv == 5000 && decode_pdo(k5V3A).max_ma == 3000);
    CHECK(decode_pdo(k20V3A).max_mv == 20000);
    const Pdo p = decode_pdo(pps(3300, 21000, 3000));
    CHECK(p.type == PdoType::Apdo && p.pps && p.min_mv == 3300 && p.max_mv == 21000 && p.max_ma == 3000);

    // Selection: highest power, max voltage / current limits, APDOs ignored
    const uint32_t caps[] = {k5V3A, k9V3A, k15V3A, k20V3A};
    Selection s = select_sink_pdo(caps, 4, 20000, 3000);
    CHECK(s.position == 4 && s.mv == 20000 && s.ma == 3000);
    s = select_sink_pdo(caps, 4, 9000, 3000);
    CHECK(s.position == 2 && s.mv == 9000);
    s = select_sink_pdo(caps, 4, 20000, 1500);
    CHECK(s.position == 4 && s.ma == 1500);
    const uint32_t caps2[] = {k5V3A, k15V3A, fixed(20000, 2250)};   // 45 W both: higher voltage wins
    s = select_sink_pdo(caps2, 3, 20000, 3000);
    CHECK(s.position == 3 && s.mv == 20000 && s.ma == 2250);
    const uint32_t caps3[] = {k5V3A, fixed(9000, 2000), fixed(15000, 1000)};   // 18 W vs 15 W
    s = select_sink_pdo(caps3, 3, 20000, 3000);
    CHECK(s.position == 2);
    const uint32_t caps4[] = {k5V3A, k9V3A, pps(3300, 21000, 5000)};
    s = select_sink_pdo(caps4, 3, 20000, 3000);
    CHECK(s.position == 2);
    s = select_sink_pdo(caps, 0, 20000, 3000);
    CHECK(s.position == 1 && s.mv == 5000 && s.ma == 500);

    // RDO layout
    const uint32_t rdo = fixed_rdo(4, 3000, 3000);
    CHECK(((rdo >> 28) & 7) == 4 && ((rdo >> 10) & 0x3FF) == 300 && (rdo & 0x3FF) == 300);
    CHECK((rdo & (1u << 25)) && (rdo & (1u << 24)) && !(rdo & (1u << 26)));
    const RdoFields f = decode_rdo(rdo);
    CHECK(f.position == 4 && f.op_ma == 3000 && f.max_ma == 3000 && !f.mismatch);

    // Sink PDOs
    uint32_t out[7];
    CHECK(sink_pdos(out, 7, 20000, 3000, false) == 4);
    CHECK((out[0] & 0x3FFFFFFF & ~(0xFu << 26)) == k5V3A);
    CHECK((out[0] >> 29 & 1) && (out[0] >> 28 & 1) && !(out[0] >> 27 & 1) && (out[0] >> 26 & 1));
    CHECK(out[1] == k9V3A && out[2] == k15V3A && out[3] == k20V3A);
    CHECK(sink_pdos(out, 7, 9000, 1500, true) == 2 && (out[0] >> 27 & 1) && out[1] == fixed(9000, 1500));
    CHECK(sink_pdos(out, 7, 5000, 3000, false) == 1);

    // Source PDO and request evaluation
    CHECK(decode_pdo(kSourcePdo).max_mv == 5000 && decode_pdo(kSourcePdo).max_ma == 3000 && (kSourcePdo >> 29 & 1));
    CHECK(evaluate_source_request(fixed_rdo(1, 3000, 3000)) == RequestVerdict::Accept);
    CHECK(evaluate_source_request(fixed_rdo(1, 900, 900)) == RequestVerdict::Accept);
    CHECK(evaluate_source_request(fixed_rdo(2, 1000, 1000)) == RequestVerdict::Reject);
    CHECK(evaluate_source_request(fixed_rdo(1, 3500, 3500)) == RequestVerdict::Reject);
    CHECK(evaluate_source_request(fixed_rdo(1, 2000, 4000)) == RequestVerdict::Reject);
    CHECK(evaluate_source_request(fixed_rdo(1, 2000, 4000) | (1u << 26)) == RequestVerdict::Accept);

    // Limits
    CHECK(vindpm_for_mv(20000) == 17000 && vindpm_for_mv(5000) == 4250 && vindpm_for_mv(3000) == 3600);
    CHECK(typec_current_ma(1) == 500 && typec_current_ma(2) == 1500 && typec_current_ma(3) == 3000 && typec_current_ma(0) == 500);

    // VOUT bridge
    CHECK(vout_fits_source(true, true, true, false, 5000, 3000));
    CHECK(vout_fits_source(true, true, true, false, 5250, 2950));
    CHECK(!vout_fits_source(false, true, true, false, 5000, 3000));   // never read
    CHECK(!vout_fits_source(true, false, true, false, 5000, 3000));   // EN pin low
    CHECK(!vout_fits_source(true, true, false, false, 5000, 3000));   // output off
    CHECK(!vout_fits_source(true, true, true, true, 5000, 3000));     // fault
    CHECK(!vout_fits_source(true, true, true, false, 9000, 3000));
    CHECK(!vout_fits_source(true, true, true, false, 4700, 3000));
    CHECK(!vout_fits_source(true, true, true, false, 5000, 2000));

    if (g_failures) {
        std::printf("usb_pd_policy: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("usb_pd_policy: all tests passed\n");
    return 0;
}
