#ifndef __USB_PD_POLICY_HPP__
#define __USB_PD_POLICY_HPP__

#include <cstdint>

/**
 * @brief Pure USB-PD helpers: PDO decoding, sink PDO selection, RDO/PDO encoding and the
 *        input limits derived from a contract (docs/usb_pd.md). No HAL, unit tested on
 *        the host (controller/tests/test_usb_pd_policy.cc).
 *
 * Bit layouts: USB PD 3.x, Tables 6-9..6-23 (same as the ST unions USBPD_PDO_TypeDef /
 * USBPD_SNKRDO_TypeDef in usbpd_def.h, which the controller cannot include).
 */
namespace usb_pd {

enum class PdoType : uint8_t { Fixed = 0, Battery = 1, Variable = 2, Apdo = 3 };

struct Pdo {
    PdoType type = PdoType::Fixed;
    bool pps = false;          // APDO subtype 0 (programmable power supply)
    uint32_t min_mv = 0;       // fixed: = max_mv
    uint32_t max_mv = 0;
    uint32_t max_ma = 0;       // fixed / variable / PPS
    uint32_t max_mw = 0;       // battery
};

constexpr Pdo decode_pdo(uint32_t raw) {
    Pdo p{};
    p.type = static_cast<PdoType>(raw >> 30);
    switch (p.type) {
        case PdoType::Fixed:
            p.max_mv = p.min_mv = ((raw >> 10) & 0x3FFu) * 50u;
            p.max_ma = (raw & 0x3FFu) * 10u;
            break;
        case PdoType::Variable:
            p.max_mv = ((raw >> 20) & 0x3FFu) * 50u;
            p.min_mv = ((raw >> 10) & 0x3FFu) * 50u;
            p.max_ma = (raw & 0x3FFu) * 10u;
            break;
        case PdoType::Battery:
            p.max_mv = ((raw >> 20) & 0x3FFu) * 50u;
            p.min_mv = ((raw >> 10) & 0x3FFu) * 50u;
            p.max_mw = (raw & 0x3FFu) * 250u;
            break;
        case PdoType::Apdo:
            p.pps = ((raw >> 28) & 0x3u) == 0u;
            if (p.pps) {
                p.max_mv = ((raw >> 17) & 0xFFu) * 100u;
                p.min_mv = ((raw >> 8) & 0xFFu) * 100u;
                p.max_ma = (raw & 0x7Fu) * 50u;
            }
            break;
    }
    return p;
}

struct Selection {
    uint32_t position = 1;     // 1-based object position
    uint32_t mv = 5000;
    uint32_t ma = 0;
};

/**
 * @brief Pick the fixed PDO with the most power V x min(Imax, max_ma) between 5 V and
 *        max_mv (ties: the higher voltage, less cable loss). Falls back to PDO 1 (vSafe5V,
 *        always fixed 5 V by the spec).
 */
constexpr Selection select_sink_pdo(const uint32_t* pdos, uint32_t count, uint32_t max_mv, uint32_t max_ma) {
    Selection best{};
    uint64_t best_mw = 0;
    bool found = false;
    for (uint32_t i = 0; i < count && i < 7u; ++i) {
        const Pdo p = decode_pdo(pdos[i]);
        if (p.type != PdoType::Fixed || p.max_mv < 5000u || p.max_mv > max_mv) {
            continue;
        }
        const uint32_t ma = p.max_ma < max_ma ? p.max_ma : max_ma;
        const uint64_t mw = static_cast<uint64_t>(p.max_mv) * ma;
        if (!found || mw > best_mw || (mw == best_mw && p.max_mv > best.mv)) {
            found = true;
            best_mw = mw;
            best = {i + 1u, p.max_mv, ma};
        }
    }
    if (!found) {
        const Pdo p = count ? decode_pdo(pdos[0]) : Pdo{};
        const uint32_t ma = p.max_ma < max_ma ? p.max_ma : max_ma;
        best = {1u, 5000u, ma ? ma : (max_ma < 500u ? max_ma : 500u)};
    }
    return best;
}

/** @brief Fixed/variable Request Data Object: operating = max current, USB comm capable, no USB suspend */
constexpr uint32_t fixed_rdo(uint32_t position, uint32_t op_ma, uint32_t max_ma) {
    const uint32_t op = (op_ma / 10u) & 0x3FFu;
    const uint32_t max = (max_ma / 10u) & 0x3FFu;
    return ((position & 0x7u) << 28) |   // ObjectPosition
           (1u << 25) |                  // USBCommunicationsCapable (the port carries our CDC device)
           (1u << 24) |                  // NoUSBSuspend
           (op << 10) | max;
}

struct RdoFields {
    uint32_t position;
    uint32_t op_ma;
    uint32_t max_ma;
    bool mismatch;
};

constexpr RdoFields decode_rdo(uint32_t rdo) {
    return {(rdo >> 28) & 0x7u, ((rdo >> 10) & 0x3FFu) * 10u, (rdo & 0x3FFu) * 10u, ((rdo >> 26) & 1u) != 0};
}

/** @brief Our Sink_Capabilities: 5 V and 9/15/20 V up to max_mv, each at max_ma. Returns the count. */
constexpr uint32_t sink_pdos(uint32_t* out, uint32_t max_count, uint32_t max_mv, uint32_t max_ma, bool unconstrained) {
    constexpr uint32_t kVoltages[] = {5000u, 9000u, 15000u, 20000u};
    uint32_t n = 0;
    for (uint32_t mv : kVoltages) {
        if (n >= max_count || (n > 0 && mv > max_mv)) {
            break;
        }
        uint32_t pdo = ((mv / 50u) << 10) | ((max_ma / 10u) & 0x3FFu);   // fixed, [31:30] = 00
        if (n == 0) {
            pdo |= (1u << 29) |                       // dual-role power
                   (1u << 28) |                       // higher capability (wants more than vSafe5V)
                   ((unconstrained ? 1u : 0u) << 27) |
                   (1u << 26);                        // USB communications capable
        }
        out[n++] = pdo;
    }
    return n;
}

/** @brief Our only Source PDO: fixed 5 V 3 A, dual-role, USB communications capable */
constexpr uint32_t kSourcePdo = (1u << 29) | (1u << 26) | ((5000u / 50u) << 10) | (3000u / 10u);
constexpr uint32_t kSourceMv = 5000u;
constexpr uint32_t kSourceMa = 3000u;

enum class RequestVerdict : uint8_t { Accept, Reject };

/** @brief Partner's Request against kSourcePdo */
constexpr RequestVerdict evaluate_source_request(uint32_t rdo) {
    const RdoFields f = decode_rdo(rdo);
    if (f.position != 1u || f.op_ma > kSourceMa || (f.max_ma > kSourceMa && !f.mismatch)) {
        return RequestVerdict::Reject;
    }
    return RequestVerdict::Accept;
}

/** @brief VINDPM for a contract voltage: 85 %, at least the chip minimum 3.6 V */
constexpr uint32_t vindpm_for_mv(uint32_t mv) {
    const uint32_t v = mv * 85u / 100u;
    return v < 3600u ? 3600u : v;
}

/** @brief Type-C Rp level (USBPD_APP_RP_*: 1 default, 2 1.5 A, 3 3.0 A) to input current */
constexpr uint32_t typec_current_ma(uint8_t rp) {
    switch (rp) {
        case 2: return 1500u;
        case 3: return 3000u;
        default: return 500u;
    }
}

} // namespace usb_pd

#endif
