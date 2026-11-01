#ifndef __SCOPE_CLIENT_HPP__
#define __SCOPE_CLIENT_HPP__

#include "frame.hpp"
#include "protocol.pb.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Downloads an oscilloscope capture (docs/host_protocol.md, "Scope read")
 *
 * read() asks for samples [start, start + count) of the channels in `mask`. DATA frames are
 * placed by (channel, offset); after ScopeReadComplete (or when the transfer stalls) the
 * missing sample ranges are requested again with new ScopeReads (resend by range), up to
 * kMaxRounds times. The finished capture is optionally written to a CSV file.
 *
 * on_message() / on_data() are called on the io thread (Connector hooks); read(), cancel()
 * and the accessors may be called from any thread.
 */
class ScopeClient {
public:
    static constexpr unsigned kMaxRounds = 3;
    static constexpr auto kStallTimeout = std::chrono::seconds(3);

    using SendFn = std::function<uint16_t(const Request&)>;
    using LogFn = std::function<void(const std::string&)>;

    struct Capture {
        uint32_t channel_mask{0};
        int64_t start{0};                  // first sample, relative to the trigger
        uint32_t count{0};                 // samples per channel
        uint32_t sample_rate_hz{0};        // 0 = unknown
        std::vector<std::vector<uint16_t>> samples;   // [channel][i], channel = bit index
        std::vector<std::vector<bool>> have;
        bool complete{false};
        bool failed{false};
        std::string error;
    };

    struct Progress {
        bool active{false};
        uint64_t received{0};    // samples (all channels)
        uint64_t total{0};
        unsigned round{0};       // 0 = first pass, >0 = resend rounds
        uint32_t lost_frames{0}; // gaps seen in DATA frame seq
    };

    ScopeClient(boost::asio::io_context& io, SendFn send, LogFn log);

    /** @return false (with `error`) if a read is already running or the request is invalid */
    bool read(uint32_t channel_mask, int64_t start, uint32_t count, uint32_t sample_rate_hz, DataLink link,
              std::string csv_path, std::string& error);
    void cancel();

    void on_message(const Response& resp, frame::Type type, uint16_t seq);
    void on_data(const frame::View& view);

    Progress progress() const;
    /** @brief Most recent finished capture (shared, immutable), nullptr if none */
    std::shared_ptr<const Capture> last_capture() const;

    /** @brief Make a capture saved by write_csv() the "last capture" (for viewing) */
    bool load_csv(const std::string& path, std::string& error);

    /** @brief Write a capture as CSV: sample, time_s, ch<N>... (missing samples left empty) */
    static bool write_csv(const Capture& capture, const std::string& path, std::string& error);

private:
    struct Range {
        uint32_t mask;
        uint32_t offset;   // relative to Capture::start
        uint32_t count;
    };

    SendFn send_;
    LogFn log_;
    boost::asio::steady_timer stall_timer_;

    mutable std::mutex mutex_;
    bool active_{false};
    Capture capture_;
    std::shared_ptr<const Capture> last_;
    std::string csv_path_;
    DataLink link_{DataLink_DATA_LINK_ORIGIN};
    std::deque<Range> pending_;
    unsigned round_{0};
    uint64_t received_{0};
    uint32_t lost_frames_{0};
    uint32_t next_transfer_id_{1};

    // current transfer
    bool in_flight_{false};
    uint32_t transfer_id_{0};
    uint16_t request_seq_{0};
    Range current_{};
    std::optional<uint16_t> last_data_seq_;

    void issue_next_locked();
    void transfer_done_locked(const std::string& why);
    void finish_locked(bool ok, const std::string& error);
    void arm_stall_timer_locked();
    std::deque<Range> missing_ranges_locked() const;
};

#endif
