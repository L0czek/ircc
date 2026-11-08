#include "ui/format.hpp"

#include <cmath>
#include <cstring>

namespace ui {

namespace {

constexpr uint32_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};

struct Writer {
    char* buf;
    size_t n;
    size_t len = 0;

    void put(char c) noexcept {
        if (len + 1 < n) {
            buf[len] = c;
        }
        ++len;
    }
    void put(const char* s) noexcept {
        while (s && *s) {
            put(*s++);
        }
    }
    void put_uint(uint64_t v, int min_digits = 1) noexcept {
        char tmp[24];
        int i = 0;
        do {
            tmp[i++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v != 0 && i < 24);
        while (i < min_digits && i < 24) {
            tmp[i++] = '0';
        }
        while (i > 0) {
            put(tmp[--i]);
        }
    }
    size_t finish() noexcept {
        if (n > 0) {
            buf[len < n ? len : n - 1] = '\0';
        }
        return len < n ? len : (n > 0 ? n - 1 : 0);
    }
};

void write_fixed(Writer& w, float value, int decimals, bool plus) noexcept {
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 6) {
        decimals = 6;
    }
    if (std::isnan(value)) {
        w.put("--");
        return;
    }
    const bool negative = value < 0.0f;
    double mag = std::fabs(static_cast<double>(value));
    if (mag > 1e12) {
        w.put(negative ? "-inf" : "inf");
        return;
    }
    const uint64_t scaled = static_cast<uint64_t>(mag * kPow10[decimals] + 0.5);
    const uint64_t ipart = scaled / kPow10[decimals];
    const uint64_t fpart = scaled % kPow10[decimals];
    if (negative && scaled != 0) {
        w.put('-');
    } else if (plus && scaled != 0) {
        w.put('+');
    }
    w.put_uint(ipart);
    if (decimals > 0) {
        w.put('.');
        w.put_uint(fpart, decimals);
    }
}

} // namespace

size_t format_fixed(char* buf, size_t n, float value, int decimals, bool plus) noexcept {
    Writer w{buf, n};
    write_fixed(w, value, decimals, plus);
    return w.finish();
}

size_t format_value(char* buf, size_t n, float value, int decimals, const char* unit, bool plus) noexcept {
    Writer w{buf, n};
    write_fixed(w, value, decimals, plus);
    w.put(unit);
    return w.finish();
}

size_t format_si(char* buf, size_t n, float value, const char* unit, int digits) noexcept {
    struct Prefix {
        float scale;
        const char* name;
    };
    static constexpr Prefix kPrefixes[] = {
        {1e6f, "M"}, {1e3f, "k"}, {1.0f, ""}, {1e-3f, "m"}, {1e-6f, "µ"},
    };
    Writer w{buf, n};
    if (std::isnan(value)) {
        w.put("--");
        w.put(unit);
        return w.finish();
    }
    const float mag = std::fabs(value);
    const Prefix* p = &kPrefixes[2];
    if (mag != 0.0f) {
        for (const auto& candidate : kPrefixes) {
            if (mag >= candidate.scale * 0.9995f) {
                p = &candidate;
                break;
            }
            p = &candidate;
        }
    }
    const float scaled = value / p->scale;
    const float smag = std::fabs(scaled);
    int int_digits = smag >= 99.95f ? 3 : (smag >= 9.995f ? 2 : 1);
    int decimals = digits - int_digits;
    if (decimals < 0) {
        decimals = 0;
    }
    write_fixed(w, scaled, decimals, false);
    w.put(p->name);
    w.put(unit);
    return w.finish();
}

size_t format_duration(char* buf, size_t n, uint32_t seconds) noexcept {
    Writer w{buf, n};
    const uint32_t h = seconds / 3600;
    const uint32_t m = (seconds / 60) % 60;
    const uint32_t s = seconds % 60;
    if (h > 0) {
        w.put_uint(h);
        w.put(':');
        w.put_uint(m, 2);
    } else {
        w.put_uint(m);
    }
    w.put(':');
    w.put_uint(s, 2);
    return w.finish();
}

size_t format_uint(char* buf, size_t n, uint32_t value) noexcept {
    Writer w{buf, n};
    w.put_uint(value);
    return w.finish();
}

size_t append(char* buf, size_t n, const char* src) noexcept {
    size_t len = std::strlen(buf);
    while (src && *src && len + 1 < n) {
        buf[len++] = *src++;
    }
    if (n > 0) {
        buf[len < n ? len : n - 1] = '\0';
    }
    return len;
}

} // namespace ui
