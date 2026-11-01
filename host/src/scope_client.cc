#include "scope_client.hpp"
#include "error_host.hpp"
#include "scope_data.hpp"
#include <bit>
#include <format>
#include <fstream>

ScopeClient::ScopeClient(boost::asio::io_context& io, SendFn send, LogFn log)
    : send_(std::move(send)), log_(std::move(log)), stall_timer_(io) {}

bool ScopeClient::read(uint32_t channel_mask, int64_t start, uint32_t count, uint32_t sample_rate_hz, DataLink link,
                       std::string csv_path, std::string& error) {
    std::lock_guard lock(mutex_);
    if (active_) {
        error = "a scope read is already running";
        return false;
    }
    if (channel_mask == 0 || channel_mask > 0xFu || count == 0) {
        error = "need a channel mask (1..0xF) and a sample count";
        return false;
    }
    capture_ = Capture{};
    capture_.channel_mask = channel_mask;
    capture_.start = start;
    capture_.count = count;
    capture_.sample_rate_hz = sample_rate_hz;
    const int channels = 32 - std::countl_zero(channel_mask);
    capture_.samples.assign(channels, {});
    capture_.have.assign(channels, {});
    for (int ch = 0; ch < channels; ++ch) {
        if (channel_mask & (1u << ch)) {
            capture_.samples[ch].assign(count, 0);
            capture_.have[ch].assign(count, false);
        }
    }
    csv_path_ = std::move(csv_path);
    link_ = link;
    pending_.clear();
    pending_.push_back({channel_mask, 0, count});
    round_ = 0;
    received_ = 0;
    lost_frames_ = 0;
    active_ = true;
    issue_next_locked();
    return true;
}

void ScopeClient::cancel() {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return;
    }
    if (in_flight_) {
        Request req = Request_init_zero;
        req.which_command = Request_scope_read_cancel_tag;
        req.command.scope_read_cancel.transfer_id = transfer_id_;
        send_(req);
    }
    pending_.clear();
    finish_locked(false, "cancelled");
}

void ScopeClient::issue_next_locked() {
    if (pending_.empty()) {
        transfer_done_locked("");
        return;
    }
    current_ = pending_.front();
    pending_.pop_front();
    transfer_id_ = next_transfer_id_++;
    last_data_seq_.reset();
    in_flight_ = true;

    Request req = Request_init_zero;
    req.which_command = Request_scope_read_tag;
    auto& r = req.command.scope_read;
    r.transfer_id = transfer_id_;
    r.channel_mask = current_.mask;
    r.start = capture_.start + current_.offset;
    r.count = current_.count;
    r.has_link = true;
    r.link = link_;
    request_seq_ = send_(req);
    arm_stall_timer_locked();
}

void ScopeClient::arm_stall_timer_locked() {
    stall_timer_.expires_after(kStallTimeout);
    const uint32_t id = transfer_id_;
    stall_timer_.async_wait([this, id](boost::system::error_code ec) {
        if (ec) {
            return;   // re-armed or cancelled
        }
        std::lock_guard lock(mutex_);
        if (!active_ || !in_flight_ || transfer_id_ != id) {
            return;
        }
        Request req = Request_init_zero;
        req.which_command = Request_scope_read_cancel_tag;
        req.command.scope_read_cancel.transfer_id = id;
        send_(req);
        transfer_done_locked("transfer stalled");
    });
}

void ScopeClient::on_message(const Response& resp, frame::Type type, uint16_t seq) {
    std::lock_guard lock(mutex_);
    if (!active_ || !in_flight_) {
        return;
    }
    if (type == frame::Type::Response && seq == request_seq_) {
        if (resp.which_payload == Response_scope_read_started_tag &&
            resp.payload.scope_read_started.transfer_id == transfer_id_) {
            arm_stall_timer_locked();
            return;
        }
        // Rejected ScopeRead: the capture cannot be read (out of range, no capture, busy...)
        std::string why = "scope read rejected";
        if (resp.has_result && resp.result.has_error) {
            why += ": " + get_error_message(static_cast<ControllerError>(resp.result.error));
        }
        in_flight_ = false;
        stall_timer_.cancel();
        pending_.clear();
        finish_locked(false, why);
        return;
    }
    if (type == frame::Type::Event && resp.which_payload == Response_scope_read_complete_tag &&
        resp.payload.scope_read_complete.transfer_id == transfer_id_) {
        const auto& done = resp.payload.scope_read_complete;
        if (done.has_error && log_) {
            log_("Scope: device reported " + get_error_message(static_cast<ControllerError>(done.error)) +
                 " during transfer");
        }
        transfer_done_locked("");
    }
}

void ScopeClient::on_data(const frame::View& view) {
    scope_data::Header h{};
    if (!scope_data::read_header(view.payload, view.length, h)) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (!active_ || !in_flight_ || h.transfer_id != static_cast<uint16_t>(transfer_id_)) {
        return;
    }
    if (last_data_seq_ && view.seq != static_cast<uint16_t>(*last_data_seq_ + 1)) {
        lost_frames_ += static_cast<uint16_t>(view.seq - *last_data_seq_ - 1);
    }
    last_data_seq_ = view.seq;
    if (h.channel >= capture_.samples.size() || !(current_.mask & (1u << h.channel))) {
        return;
    }
    auto& samples = capture_.samples[h.channel];
    auto& have = capture_.have[h.channel];
    const uint64_t base = static_cast<uint64_t>(current_.offset) + h.sample_offset;
    for (uint32_t i = 0; i < h.sample_count; ++i) {
        const uint64_t at = base + i;
        if (at >= capture_.count) {
            break;
        }
        samples[at] = scope_data::sample(view.payload, i);
        if (!have[at]) {
            have[at] = true;
            ++received_;
        }
    }
    arm_stall_timer_locked();
}

std::deque<ScopeClient::Range> ScopeClient::missing_ranges_locked() const {
    std::deque<Range> ranges;
    auto missing_mask = [this](uint32_t i) {
        uint32_t m = 0;
        for (size_t ch = 0; ch < capture_.have.size(); ++ch) {
            if ((capture_.channel_mask & (1u << ch)) && !capture_.have[ch][i]) {
                m |= 1u << ch;
            }
        }
        return m;
    };
    uint32_t i = 0;
    while (i < capture_.count) {
        uint32_t m = missing_mask(i);
        if (m == 0) {
            ++i;
            continue;
        }
        const uint32_t begin = i;
        while (i < capture_.count) {
            const uint32_t mi = missing_mask(i);
            if (mi == 0) {
                break;
            }
            m |= mi;
            ++i;
        }
        ranges.push_back({m, begin, i - begin});
    }
    return ranges;
}

void ScopeClient::transfer_done_locked(const std::string& why) {
    in_flight_ = false;
    stall_timer_.cancel();
    if (!why.empty() && log_) {
        log_("Scope: " + why);
    }
    if (!pending_.empty()) {
        issue_next_locked();
        return;
    }
    auto missing = missing_ranges_locked();
    if (missing.empty()) {
        finish_locked(true, "");
        return;
    }
    if (round_ >= kMaxRounds) {
        uint64_t samples = 0;
        for (const auto& r : missing) samples += static_cast<uint64_t>(r.count) * std::popcount(r.mask);
        finish_locked(false, std::format("{} samples still missing after {} resend rounds", samples, kMaxRounds));
        return;
    }
    ++round_;
    if (log_) {
        log_(std::format("Scope: re-requesting {} missing range(s), round {}", missing.size(), round_));
    }
    pending_ = std::move(missing);
    issue_next_locked();
}

void ScopeClient::finish_locked(bool ok, const std::string& error) {
    active_ = false;
    in_flight_ = false;
    stall_timer_.cancel();
    capture_.complete = ok;
    capture_.failed = !ok;
    capture_.error = error;
    std::string msg = ok ? std::format("Scope: read complete, {} samples", received_)
                         : "Scope: read failed - " + error;
    if (!csv_path_.empty() && received_ > 0) {
        std::string werr;
        if (write_csv(capture_, csv_path_, werr)) {
            msg += ", saved to " + csv_path_;
        } else {
            msg += ", " + werr;
        }
    }
    last_ = capture_;
    if (log_) {
        log_(msg);
    }
}

ScopeClient::Progress ScopeClient::progress() const {
    std::lock_guard lock(mutex_);
    Progress p;
    p.active = active_;
    p.received = received_;
    p.total = static_cast<uint64_t>(capture_.count) * std::popcount(capture_.channel_mask);
    p.round = round_;
    p.lost_frames = lost_frames_;
    return p;
}

std::optional<ScopeClient::Capture> ScopeClient::last_capture() const {
    std::lock_guard lock(mutex_);
    return last_;
}

bool ScopeClient::write_csv(const Capture& capture, const std::string& path, std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot open " + path;
        return false;
    }
    out << "sample";
    if (capture.sample_rate_hz) {
        out << ",time_s";
    }
    for (size_t ch = 0; ch < capture.samples.size(); ++ch) {
        if (capture.channel_mask & (1u << ch)) {
            out << ",ch" << ch;
        }
    }
    out << '\n';
    for (uint32_t i = 0; i < capture.count; ++i) {
        const int64_t n = capture.start + i;
        out << n;
        if (capture.sample_rate_hz) {
            out << std::format(",{:.9g}", static_cast<double>(n) / capture.sample_rate_hz);
        }
        for (size_t ch = 0; ch < capture.samples.size(); ++ch) {
            if (capture.channel_mask & (1u << ch)) {
                out << ',';
                if (capture.have[ch][i]) {
                    out << capture.samples[ch][i];
                }
            }
        }
        out << '\n';
    }
    if (!out) {
        error = "write to " + path + " failed";
        return false;
    }
    return true;
}
