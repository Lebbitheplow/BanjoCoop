/* ENet transport for BanjoCoop.
 *
 * Topology is a star: the host is authoritative and relays state between clients. That matches
 * the ownership model in the plan (owner simulates, everyone else applies) and avoids the NxN
 * connection mesh that would otherwise be needed.
 *
 * Threading: ENet is not thread-safe, so every enet_* call happens on the net thread and nothing
 * else touches the host. The game thread exchanges data through two small mutex-guarded
 * snapshots. A mutex rather than a lock-free queue is deliberate — for per-frame state we only
 * ever want the *latest* value, so a queue would just be a buffer of stale frames to throw away.
 * Contention is one tiny critical section per frame against a 30 Hz net thread.
 */

#ifndef BANJOCOOP_TRANSPORT_HPP
#define BANJOCOOP_TRANSPORT_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "banjocoop/netsim.hpp"
#include "banjocoop/protocol.h"

namespace bcnet {

class Link; /* the byte transport; defined in link.hpp, forward-declared to keep this header light */

constexpr size_t ROM_HASH_LEN = 20; /* sha1 */

/* Identity exchanged at handshake. A mismatch in any of these produces silent, baffling desync
 * if allowed through, so all three are checked before a peer is admitted. */
struct Identity {
    uint32_t mod_version = 0;
    std::array<uint8_t, ROM_HASH_LEN> rom_hash{};
    std::string name;
};

enum class Status : uint32_t {
    Offline = 0,
    Hosting = 1,
    Connecting = 2,
    Connected = 3,
    Failed = 4,
    Rejected = 5,
};

struct PeerSlot {
    bool active = false;
    /* A peer is known (it completed the handshake) well before its first state packet arrives.
     * Reporting it to the mod during that window would spawn a puppet at the origin for a frame
     * or two, so the snapshot omits peers until this is true. */
    bool has_state = false;
    uint32_t player_id = 0;
    std::string name;
    /* Stored exactly as it arrived: big-endian, uninterpreted. */
    bc_player_state state{};
    /* Last map we sent this peer a state snapshot for. Under free-roam a player arriving in a map
     * has missed every change made there while they were elsewhere — and a player who just joined
     * has missed all of them everywhere — so entering a map has to hand them that map's state.
     * The sentinel guarantees the first state packet counts as an arrival. */
    uint32_t snapshot_map = 0xFFFFFFFFu;
};

class Transport {
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool host(uint16_t port, const Identity& id, std::string& error);
    bool join(const std::string& address, uint16_t port, const Identity& id, std::string& error);
    /* WebSocket variants, for play over a Cloudflare tunnel. host_ws binds a local ws:// server a
     * tunnel points at; join_ws connects to a full ws:// or wss:// URL from an expanded join code.
     * Everything above the byte transport is identical to the ENet paths. */
    bool host_ws(uint16_t port, const Identity& id, std::string& error);
    bool join_ws(const std::string& url, const Identity& id, std::string& error);
    void shutdown();

    /* Called from the game thread once per frame. `state` is raw big-endian bytes from rdram. */
    void set_local_state(const bc_player_state& state);

    /* Called from the game thread once per frame; fills the mod's incoming staging buffer and
     * drains the accepted-event queue into it. */
    void snapshot(bc_incoming& out);

    /* Called from the game thread once per frame with the events the mod's hooks queued up.
     *
     * On a client these go to the host for adjudication. On the host they are adjudicated
     * immediately and relayed, which keeps one code path for "decide what happens to an event"
     * regardless of who originated it.
     */
    void submit_events(const bc_event_queue& out);

    /* Called from the game thread once per frame with the objects this peer owns and simulates.
     * A peer that owns nothing passes count 0 and nothing is sent. */
    void submit_objects(const bc_object_frame& out);

    /* Host only: the authoritative progression mirror, resent periodically so a client that
     * missed one is corrected by the next rather than left stranded. */
    void submit_progress(const bc_progress& out);

    /* The path of this peer's own save file, handed over by the mod (only the game knows it).
     * The host reads it to send; a client uses its directory to work out where to put the host's.
     */
    void set_save_path(const std::string& path);

    /* Non-empty once the host's save has been written locally: the name to hand to
     * recomp_change_save_file. Read once by the mod, then cleared. */
    std::string take_pending_save_name();

    /* A player's name, for the player list. Strings cannot ride in the staging structs — rdram's
     * word-swapped layout makes sub-word access there wrong (see protocol.h) — so they are fetched
     * one at a time and written byte-wise. */
    std::string player_name(uint32_t player_id) const;

    /* Chat. Sent by anyone, stamped and relayed by the host like any other attributable fact. */
    void submit_chat(const bc_chat_line& line);
    void chat_log(bc_chat_log& out) const;

    Status status() const { return status_.load(std::memory_order_relaxed); }
    uint32_t reject_reason() const { return reject_reason_.load(std::memory_order_relaxed); }
    uint32_t local_player_id() const { return local_player_id_.load(std::memory_order_relaxed); }
    uint32_t ping_ms() const { return ping_ms_.load(std::memory_order_relaxed); }

    void configure_sim(const NetSimConfig& cfg);

    /* Send rate for player state. 30 Hz per the plan; independent of the game's frame rate so
     * bandwidth stays predictable if the game runs uncapped. */
    static constexpr uint32_t kStateHz = 30;

private:
    /* Stand the session up once the Link is bound. Shared by the ENet and WebSocket entry points,
     * which differ only in which Link they create. */
    bool start_host(std::unique_ptr<Link> link, const Identity& id);
    bool start_join(std::unique_ptr<Link> link, const Identity& id);

    void run();                 /* net thread entry */
    void service_host();
    void service_client();
    void pump_sim();
    void broadcast_state();
    void broadcast_objects();
    void broadcast_progress();
    void stop_thread();

    /* Move events the game thread queued onto the net thread, where all enet_* access lives. */
    void drain_outbox();
    /* Host-side adjudication. `origin` is the player id that reported the event — the host's own
     * id when the host saw it itself. Records the event in the registry, decides who needs to
     * hear about it, and queues the sends. Returns false if the event was refused. */
    bool adjudicate(const bc_event& ev, uint32_t origin);
    /* Queue an event for the local game thread to apply on the next snapshot(). */
    void accept_locally(const bc_event& ev);
    void send_event_to(const bc_event& ev, uint32_t player_id);
    /* Replay the registry for one map to one player, as ordinary events. Reusing the normal apply
     * path rather than inventing a snapshot message means there is only one piece of code that
     * knows how a collected note is applied. */
    void send_map_snapshot(uint32_t player_id, uint32_t map_id, uint32_t level_id);
    void relay_event(const bc_event& ev, uint32_t skip_player_id);
    void accept_objects(uint32_t seq, const bc_object_frame& frame);
    void accept_progress(const bc_progress& prog);
    /* Host: tell everyone who is in the session. Sent on every join and departure — a client has
     * no other way to learn that somebody left, because "left" looks exactly like "has gone quiet"
     * from the state stream. */
    void broadcast_roster();
    void accept_chat(uint32_t from, const bc_chat_line& line);
    void send_save_file_to(uint32_t peer_index);
    void store_save_file(const std::vector<uint8_t>& bytes);

    struct Impl;                /* hides the enet headers from callers */
    Impl* impl_ = nullptr;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<Status> status_{Status::Offline};
    std::atomic<uint32_t> reject_reason_{0};
    std::atomic<uint32_t> local_player_id_{0};
    std::atomic<uint32_t> ping_ms_{0};
    std::atomic<bool> is_host_{false};

    Identity identity_;

    mutable std::mutex state_mutex_;
    bc_player_state local_state_{};
    /* Objects we own, and the newest frame received from whoever owns the ones around us. Only
     * the latest of each is kept: an object frame is a snapshot, not a change, so a stale one has
     * no value once a newer has arrived. */
    bc_object_frame local_objects_{};
    bc_object_frame remote_objects_{};
    /* Object-frame sequence, so an out-of-order delivery is dropped instead of rewinding every
     * enemy to a stale position. */
    uint32_t object_seq_tx_ = 0;
    uint32_t object_seq_rx_ = 0;
    /* Save-file transfer. The host's save is the session's save — a client that joins adopts it
     * wholesale rather than trying to reconcile two histories. */
    mutable std::mutex chat_mutex_;
    std::deque<bc_chat_line> chat_;

    std::mutex save_mutex_;
    std::string save_path_;
    std::string pending_save_name_;
    bc_progress local_progress_{};
    bc_progress remote_progress_{};
    std::array<PeerSlot, BCNET_MAX_PLAYERS> peers_{};

    /* Events accepted for this peer, waiting for the game thread to collect them. A deque rather
     * than a fixed array because the net thread must never drop a world change on the floor: the
     * game thread drains it every frame, so it only grows if the game is stalled. */
    std::mutex inbox_mutex_;
    std::deque<bc_event> inbox_;
    std::atomic<uint32_t> inbox_dropped_{0};

    /* Events the game thread produced this frame, waiting to be picked up by the net thread.
     * The handoff exists because every enet_* call and all of impl_ belong to the net thread;
     * the game thread only ever touches this deque. */
    std::mutex outbox_mutex_;
    std::deque<bc_event> outbox_;

    /* Host-only. The authoritative record of which static notes are gone, keyed by
     * (map_id << 32) | note_index — the identity scheme from the plan's §1.5, which is stable
     * across machines for props because prop spawn order is fixed static-level-data order.
     *
     * This is what makes "no double-collects" true rather than merely unlikely: the second claim
     * for a key already in here is refused, whatever the timing.
     */
    std::mutex registry_mutex_;
    std::unordered_set<uint64_t> collected_notes_;

    NetSim sim_;
    std::mutex sim_mutex_;

    /* Bound on the inbox, so a game thread that has stopped draining (loading screen, pause,
     * crash) cannot grow it without limit. Overflow is counted, not silently dropped. */
    static constexpr size_t kMaxInbox = 4096;
};

} // namespace bcnet

#endif
