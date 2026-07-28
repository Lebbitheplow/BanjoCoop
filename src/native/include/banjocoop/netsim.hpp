/* Artificial network conditions.
 *
 * The plan calls for this on day one, not as a later debugging aid: replication bugs in this
 * project overwhelmingly do not reproduce on loopback, where latency is ~0 and loss is 0. Every
 * phase's exit criteria are meant to be re-run at 0 / 80 / 150 ms and at 150 ms + 3% loss.
 *
 * Applied to outbound packets, so it models this peer's uplink. Ordering is preserved for
 * sequenced traffic even when jitter would otherwise reorder deliveries — ENet's sequenced
 * channels would drop the reordered packet anyway, which would look like extra loss and quietly
 * misrepresent the conditions being simulated.
 */

#ifndef BANJOCOOP_NETSIM_HPP
#define BANJOCOOP_NETSIM_HPP

#include <chrono>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace bcnet {

using Clock = std::chrono::steady_clock;

struct NetSimConfig {
    uint32_t latency_ms = 0;  /* one-way base delay */
    uint32_t jitter_ms = 0;   /* uniform +/- applied to latency */
    float loss = 0.0f;        /* 0.0 - 1.0 */

    bool enabled() const { return latency_ms != 0 || jitter_ms != 0 || loss > 0.0f; }
};

/* Broadcast to every connected peer rather than a specific one. */
constexpr uint32_t kBroadcast = 0xFFFFFFFFu;

struct PendingPacket {
    Clock::time_point deliver_at;
    std::vector<uint8_t> data;
    uint32_t target; /* peer index, or kBroadcast */
    uint8_t channel;
    bool reliable;
};

class NetSim {
public:
    NetSim() : rng_(std::random_device{}()) {}

    void configure(const NetSimConfig& cfg) { cfg_ = cfg; }
    const NetSimConfig& config() const { return cfg_; }

    /* Returns false if the packet was dropped, true if it was accepted (possibly delayed).
     * When simulation is off the packet is scheduled with a zero delay, so callers always
     * follow the same release path and the simulated and unsimulated code paths stay identical. */
    bool submit(std::vector<uint8_t>&& data, uint32_t target, uint8_t channel, bool reliable) {
        if (!cfg_.enabled()) {
            queue_.push_back({Clock::now(), std::move(data), target, channel, reliable});
            return true;
        }

        /* Reliable packets are never dropped here. ENet would retransmit them anyway, so
         * dropping them would only simulate added latency while pretending to be loss. */
        if (!reliable && cfg_.loss > 0.0f) {
            std::uniform_real_distribution<float> d(0.0f, 1.0f);
            if (d(rng_) < cfg_.loss) {
                return false;
            }
        }

        int32_t delay = static_cast<int32_t>(cfg_.latency_ms);
        if (cfg_.jitter_ms > 0) {
            std::uniform_int_distribution<int32_t> d(-static_cast<int32_t>(cfg_.jitter_ms),
                                                     static_cast<int32_t>(cfg_.jitter_ms));
            delay += d(rng_);
        }
        if (delay < 0) {
            delay = 0;
        }

        Clock::time_point deliver_at = Clock::now() + std::chrono::milliseconds(delay);

        /* Preserve ordering (see header comment). */
        if (!queue_.empty() && deliver_at < queue_.back().deliver_at) {
            deliver_at = queue_.back().deliver_at;
        }

        queue_.push_back({deliver_at, std::move(data), target, channel, reliable});
        return true;
    }

    /* Moves all packets whose delay has elapsed into `out`. */
    void release_due(std::vector<PendingPacket>& out) {
        Clock::time_point now = Clock::now();
        while (!queue_.empty() && queue_.front().deliver_at <= now) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }

    void clear() { queue_.clear(); }
    size_t pending() const { return queue_.size(); }

private:
    NetSimConfig cfg_{};
    std::deque<PendingPacket> queue_;
    std::mt19937 rng_;
};

} // namespace bcnet

#endif
