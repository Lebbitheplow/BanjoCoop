#include "transport_internal.hpp"

#include <chrono>

namespace bcnet {

/* bc_player_state is 13 four-byte fields: map_id, level_id, flags, pos[3], vel[3], yaw,
 * transform, anim_id, anim_frame. Serialised explicitly so the wire stays big-endian
 * regardless of host endianness, and so adding a field without bumping BCNET_PROTOCOL_VERSION
 * shows up as a size mismatch here rather than as garbled positions in-game. */
constexpr size_t kStateWireSize = 13 * 4;

static_assert(sizeof(bc_player_state) == kStateWireSize,
              "bc_player_state gained a field or padding; update the wire format and bump "
              "BCNET_PROTOCOL_VERSION");

void write_state(Writer& w, const bc_player_state& s) {
    w.u32(s.map_id);
    w.u32(s.level_id);
    w.u32(s.flags);
    for (int i = 0; i < 3; i++) {
        w.u32(float_bits(s.pos[i]));
    }
    for (int i = 0; i < 3; i++) {
        w.u32(float_bits(s.vel[i]));
    }
    w.u32(float_bits(s.yaw));
    w.u32(s.transform);
    w.u32(s.anim_id);
    w.u32(float_bits(s.anim_frame));
}

bool read_state(Reader& r, bc_player_state& s) {
    s.map_id = r.u32();
    s.level_id = r.u32();
    s.flags = r.u32();
    for (int i = 0; i < 3; i++) {
        s.pos[i] = bits_float(r.u32());
    }
    for (int i = 0; i < 3; i++) {
        s.vel[i] = bits_float(r.u32());
    }
    s.yaw = bits_float(r.u32());
    s.transform = r.u32();
    s.anim_id = r.u32();
    s.anim_frame = bits_float(r.u32());
    return r.ok;
}

void Transport::set_local_state(const bc_player_state& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_state_ = state;
    uint32_t id = local_player_id_.load(std::memory_order_relaxed);
    if (id < BCNET_MAX_PLAYERS) {
        peers_[id].state = state;
        peers_[id].has_state = true;
    }
}

void Transport::snapshot(bc_incoming& out) {
    std::memset(&out, 0, sizeof(out));

    Status st = status_.load(std::memory_order_relaxed);
    bool connected = (st == Status::Hosting || st == Status::Connected);

    uint32_t local_id = local_player_id_.load(std::memory_order_relaxed);
    /* Written natively: the mod reads these straight out of rdram. */
    out.local_player_id = local_id;
    out.connected = connected ? 1u : 0u;
    out.ping_ms = ping_ms_.load(std::memory_order_relaxed);
    out.is_host = is_host_.load(std::memory_order_relaxed) ? 1u : 0u;

    /* Drain up to a queue's worth of accepted events. Anything left over is picked up next frame
     * rather than dropped — order is preserved, which matters when a set and a clear of the same
     * flag arrive together. */
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        uint32_t n = 0;
        while (n < BCNET_EVENT_QUEUE && !inbox_.empty()) {
            out.events.events[n++] = inbox_.front();
            inbox_.pop_front();
        }
        out.events.count = n;
        out.events.dropped = inbox_dropped_.load(std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        out.objects = remote_objects_;
        out.progress = remote_progress_;
    }
    chat_log(out.chat);

    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (uint32_t i = 0; i < BCNET_MAX_PLAYERS && count < BCNET_MAX_PLAYERS; i++) {
            if (!peers_[i].active || !peers_[i].has_state || i == local_id) {
                continue;
            }
            out.remotes[count].player_id = peers_[i].player_id;
            out.remotes[count].flags = BCNET_STATE_ACTIVE;
            out.remotes[count].state = peers_[i].state;
            count++;
        }
    }
    out.remote_count = count;
}

void Transport::configure_sim(const NetSimConfig& cfg) {
    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.configure(cfg);
}

void Transport::run() {
    using namespace std::chrono;
    const auto interval = milliseconds(1000 / kStateHz);
    auto next_send = steady_clock::now() + interval;
    /* The mirror is state that rarely changes, so it goes out on its own slow cadence. Once a
     * second is frequent enough that a client is never stranded for long, and cheap enough that
     * resending it unconditionally is simpler than tracking who has what. */
    const auto progress_interval = milliseconds(1000);
    auto next_progress = steady_clock::now() + progress_interval;

    while (running_.load(std::memory_order_relaxed)) {
        if (is_host_.load(std::memory_order_relaxed)) {
            service_host();
        } else {
            service_client();
        }

        /* Before the rate-limited state broadcast: world events are reliable and rare, and a
         * frame of extra latency on "the note is gone" is worth avoiding. */
        drain_outbox();

        auto now = steady_clock::now();
        if (now >= next_send) {
            broadcast_state();
            broadcast_objects();
            next_send = now + interval;
        }
        if (now >= next_progress) {
            broadcast_progress();
            next_progress = now + progress_interval;
        }

        pump_sim();
        /* Push everything queued this iteration out now rather than waiting for the next
         * service(), keeping a frame of latency off reliable world events. */
        impl_->link->flush();
    }
}

void Transport::pump_sim() {
    std::vector<PendingPacket> due;
    {
        std::lock_guard<std::mutex> lock(sim_mutex_);
        sim_.release_due(due);
    }

    for (PendingPacket& pkt : due) {
        /* kBroadcast and the link's kLinkBroadcast are the same value, so the target passes
         * through untranslated. A stale handle is dropped by the link, not checked here. */
        impl_->link->send(pkt.target, pkt.channel, pkt.reliable, pkt.data.data(), pkt.data.size());
    }
}

void Transport::broadcast_state() {
    Status st = status_.load(std::memory_order_relaxed);
    if (st != Status::Hosting && st != Status::Connected) {
        return;
    }

    bc_player_state state;
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state = local_state_;
        id = local_player_id_.load(std::memory_order_relaxed);
    }

    Writer w(BCNET_MSG_STATE);
    w.u32(id);
    write_state(w, state);

    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_STATE, false);
}

void Transport::service_host() {
    std::vector<LinkEvent> events;
    impl_->link->service(1, events);
    for (LinkEvent& ev : events) {
        switch (ev.type) {
            case LinkEvent::Type::Connect:
                /* Nothing yet — a peer is only admitted once its HELLO validates. */
                break;

            case LinkEvent::Type::Receive: {
                Reader r(ev.data.data(), ev.data.size());
                uint8_t type = r.type();

                if (type == BCNET_MSG_HELLO) {
                    uint32_t proto = r.u32();
                    uint32_t mod_ver = r.u32();
                    std::array<uint8_t, ROM_HASH_LEN> hash{};
                    r.bytes(hash.data(), hash.size());
                    std::string name = r.name();

                    uint32_t reject = 0;
                    if (!r.ok || proto != BCNET_PROTOCOL_VERSION) {
                        reject = BCNET_REJECT_PROTOCOL;
                    } else if (mod_ver != identity_.mod_version) {
                        reject = BCNET_REJECT_MOD_VERSION;
                    } else if (hash != identity_.rom_hash) {
                        reject = BCNET_REJECT_ROM_HASH;
                    } else if (impl_->next_player_id >= BCNET_MAX_PLAYERS) {
                        reject = BCNET_REJECT_FULL;
                    }

                    if (reject != 0) {
                        /* Sent directly, not through the sim: a handshake must not be delayed, and
                         * flush() guarantees the reject reaches the peer before we drop it. */
                        Writer w(BCNET_MSG_REJECT);
                        w.u32(reject);
                        impl_->link->send(ev.peer, BCNET_CHANNEL_EVENT, true, w.buf.data(), w.buf.size());
                        impl_->link->flush();
                        impl_->link->disconnect(ev.peer);
                        break;
                    }

                    uint32_t pid = impl_->next_player_id++;
                    impl_->peer_ids[ev.peer] = pid;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        peers_[pid].active = true;
                        peers_[pid].player_id = pid;
                        peers_[pid].name = name;
                    }

                    Writer w(BCNET_MSG_WELCOME);
                    w.u32(pid);
                    impl_->link->send(ev.peer, BCNET_CHANNEL_EVENT, true, w.buf.data(), w.buf.size());

                    /* Straight after admitting them: the host's save is the session's save, and
                     * everything a client cannot otherwise learn — notes in maps nobody has
                     * visited yet, above all — is in it. */
                    send_save_file_to(ev.peer);
                    broadcast_roster();
                } else if (type == BCNET_MSG_STATE) {
                    uint32_t pid = r.u32();
                    bc_player_state state{};
                    if (read_state(r, state) && pid < BCNET_MAX_PLAYERS) {
                        auto it = impl_->peer_ids.find(ev.peer);
                        /* Only accept state for the id we assigned this peer, so a client
                         * cannot puppet another player by lying about its id. */
                        if (it != impl_->peer_ids.end() && it->second == pid) {
                            bool arrived = false;
                            {
                                std::lock_guard<std::mutex> lock(state_mutex_);
                                peers_[pid].state = state;
                                peers_[pid].has_state = true;
                                /* Detecting arrival from the state stream costs nothing and needs
                                 * no request protocol — the client is already telling us where it
                                 * is every frame. */
                                if (peers_[pid].snapshot_map != state.map_id) {
                                    peers_[pid].snapshot_map = state.map_id;
                                    arrived = true;
                                }
                            }
                            if (arrived) {
                                send_map_snapshot(pid, state.map_id, state.level_id);
                            }
                            /* Relay to everyone else. */
                            Writer w(BCNET_MSG_STATE);
                            w.u32(pid);
                            write_state(w, state);
                            std::lock_guard<std::mutex> lock(sim_mutex_);
                            for (auto& [peer, other_id] : impl_->peer_ids) {
                                if (peer == ev.peer) {
                                    continue;
                                }
                                std::vector<uint8_t> copy = w.buf;
                                sim_.submit(std::move(copy), peer, BCNET_CHANNEL_STATE, false);
                            }
                        }
                    }
                } else if (type == BCNET_MSG_OBJECTS) {
                    /* A client owns the enemies in any map the host is not in — ownership is the
                     * lowest player id *present*, not the host. Its frames arrive here and have
                     * to be passed on, or enemies and projectiles only ever sync in whatever map
                     * the host happens to be standing in.
                     *
                     * Relayed verbatim, including the sender's sequence number, so the receiver
                     * can still discard a frame that overtakes a newer one. Only peers in the map
                     * it describes are interested. */
                    Reader peek(ev.data.data(), ev.data.size());
                    peek.type();
                    uint32_t seq = peek.u32();
                    uint32_t frame_map = peek.u32();
                    auto sender = impl_->peer_ids.find(ev.peer);
                    if (peek.ok && sender != impl_->peer_ids.end()) {
                        std::array<bool, BCNET_MAX_PLAYERS> wants{};
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            for (uint32_t i = 0; i < BCNET_MAX_PLAYERS; i++) {
                                wants[i] = peers_[i].active && peers_[i].has_state &&
                                           peers_[i].state.map_id == frame_map;
                            }
                        }
                        (void)seq;
                        std::lock_guard<std::mutex> lock(sim_mutex_);
                        for (const auto& [peer, id] : impl_->peer_ids) {
                            if (peer == ev.peer || id >= BCNET_MAX_PLAYERS || !wants[id]) {
                                continue;
                            }
                            std::vector<uint8_t> copy = ev.data;
                            sim_.submit(std::move(copy), peer, BCNET_CHANNEL_STATE, false);
                        }
                    }
                } else if (type == BCNET_MSG_CHAT) {
                    bc_chat_line line{};
                    line.length = r.u32();
                    for (uint32_t i = 0; i < BCNET_CHAT_LEN / 4 && r.ok; i++) {
                        line.text[i] = r.u32();
                    }
                    auto sender = impl_->peer_ids.find(ev.peer);
                    if (r.ok && line.length > 0 && line.length <= BCNET_CHAT_LEN &&
                        sender != impl_->peer_ids.end()) {
                        /* Attributed from the connection, like every other fact a client reports. */
                        accept_chat(sender->second, line);
                        std::lock_guard<std::mutex> lock(sim_mutex_);
                        for (const auto& [peer, id] : impl_->peer_ids) {
                            if (peer == ev.peer) {
                                continue;
                            }
                            Writer relay(BCNET_MSG_CHAT);
                            relay.u32(sender->second);
                            relay.u32(line.length);
                            for (uint32_t i = 0; i < BCNET_CHAT_LEN / 4; i++) {
                                relay.u32(line.text[i]);
                            }
                            sim_.submit(std::move(relay.buf), peer, BCNET_CHANNEL_EVENT, true);
                        }
                    }
                } else if (type == BCNET_MSG_EVENT) {
                    bc_event world{};
                    auto it = impl_->peer_ids.find(ev.peer);
                    /* Events are attributed to the peer they arrived on, never to a player id in
                     * the packet — otherwise a client could claim notes on someone else's behalf
                     * and dodge the revoke. */
                    if (read_event(r, world) && it != impl_->peer_ids.end()) {
                        adjudicate(world, it->second);
                    }
                }
                break;
            }

            case LinkEvent::Type::Disconnect: {
                auto it = impl_->peer_ids.find(ev.peer);
                if (it != impl_->peer_ids.end()) {
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        peers_[it->second] = PeerSlot{};
                    }
                    impl_->peer_ids.erase(it);
                    /* Without this the other clients never find out. A departure is invisible in
                     * the state stream — it looks identical to a player who has simply gone quiet
                     * — so their puppet would stand in the world forever. */
                    broadcast_roster();
                }
                break;
            }

            default:
                break;
        }
    }
}

void Transport::service_client() {
    std::vector<LinkEvent> events;
    impl_->link->service(1, events);
    for (LinkEvent& ev : events) {
        switch (ev.type) {
            case LinkEvent::Type::Connect: {
                Writer w(BCNET_MSG_HELLO);
                w.u32(BCNET_PROTOCOL_VERSION);
                w.u32(identity_.mod_version);
                w.bytes(identity_.rom_hash.data(), identity_.rom_hash.size());
                w.name(identity_.name);
                /* Sent directly, not through the sim: the handshake must not be delayed. */
                impl_->link->send(ev.peer, BCNET_CHANNEL_EVENT, true, w.buf.data(), w.buf.size());
                impl_->link->flush();
                break;
            }

            case LinkEvent::Type::Receive: {
                Reader r(ev.data.data(), ev.data.size());
                uint8_t type = r.type();

                if (type == BCNET_MSG_WELCOME) {
                    uint32_t pid = r.u32();
                    if (r.ok && pid < BCNET_MAX_PLAYERS) {
                        local_player_id_.store(pid);
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            peers_[pid].active = true;
                            peers_[pid].player_id = pid;
                            peers_[pid].name = identity_.name;
                            /* The host always occupies slot 0. */
                            peers_[0].active = true;
                            peers_[0].player_id = 0;
                        }
                        status_.store(Status::Connected);
                    }
                } else if (type == BCNET_MSG_REJECT) {
                    reject_reason_.store(r.u32());
                    status_.store(Status::Rejected);
                } else if (type == BCNET_MSG_STATE) {
                    uint32_t pid = r.u32();
                    bc_player_state state{};
                    if (read_state(r, state) && pid < BCNET_MAX_PLAYERS &&
                        pid != local_player_id_.load(std::memory_order_relaxed)) {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        peers_[pid].active = true;
                        peers_[pid].player_id = pid;
                        peers_[pid].state = state;
                        peers_[pid].has_state = true;
                    }
                } else if (type == BCNET_MSG_EVENT) {
                    /* Anything the host sends has already been adjudicated, so a client applies
                     * it unconditionally. That is the whole point of the star topology: clients
                     * hold no opinion about world state. */
                    bc_event world{};
                    if (read_event(r, world)) {
                        accept_locally(world);
                    }
                } else if (type == BCNET_MSG_OBJECTS) {
                    bc_object_frame frame{};
                    uint32_t seq = r.u32();
                    frame.map_id = r.u32();
                    uint32_t n = r.u32();
                    if (r.ok && n <= BCNET_MAX_OBJECTS) {
                        for (uint32_t i = 0; i < n && r.ok; i++) {
                            read_object(r, frame.objects[i]);
                        }
                        if (r.ok) {
                            frame.count = n;
                            accept_objects(seq, frame);
                        }
                    }
                } else if (type == BCNET_MSG_SAVEFILE) {
                    uint32_t n = r.u32();
                    if (r.ok && n > 0 && n <= BCNET_MAX_SAVE_BYTES) {
                        std::vector<uint8_t> bytes(n);
                        if (r.bytes(bytes.data(), n)) {
                            store_save_file(bytes);
                        }
                    }
                } else if (type == BCNET_MSG_CHAT) {
                    bc_chat_line line{};
                    uint32_t from = r.u32();
                    line.length = r.u32();
                    for (uint32_t i = 0; i < BCNET_CHAT_LEN / 4 && r.ok; i++) {
                        line.text[i] = r.u32();
                    }
                    if (r.ok && line.length > 0 && line.length <= BCNET_CHAT_LEN) {
                        accept_chat(from, line);
                    }
                } else if (type == BCNET_MSG_PEERS) {
                    uint32_t count = r.u32();
                    if (r.ok && count <= BCNET_MAX_PLAYERS) {
                        std::array<bool, BCNET_MAX_PLAYERS> present{};
                        std::array<std::string, BCNET_MAX_PLAYERS> names{};
                        bool ok = true;
                        for (uint32_t i = 0; i < count && ok; i++) {
                            uint32_t id = r.u32();
                            std::string name = r.name();
                            if (!r.ok || id >= BCNET_MAX_PLAYERS) {
                                ok = false;
                                break;
                            }
                            present[id] = true;
                            names[id] = name;
                        }
                        if (ok) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            for (uint32_t i = 0; i < BCNET_MAX_PLAYERS; i++) {
                                if (present[i]) {
                                    peers_[i].active = true;
                                    peers_[i].player_id = i;
                                    peers_[i].name = names[i];
                                } else {
                                    /* Gone. Clearing has_state with it is the point: the mod stops
                                     * being told about them and despawns their puppet. */
                                    peers_[i] = PeerSlot{};
                                }
                            }
                        }
                    }
                } else if (type == BCNET_MSG_PROGRESS) {
                    bc_progress prog{};
                    prog.note_map = r.u32();
                    for (uint32_t i = 0; i < BC_PROGRESS_NOTE_WORDS && r.ok; i++) {
                        prog.note_bits[i] = r.u32();
                    }
                    for (uint32_t i = 0; i < BC_PROGRESS_WORDS && r.ok; i++) {
                        prog.words[i] = r.u32();
                    }
                    if (r.ok) {
                        prog.valid = 1;
                        accept_progress(prog);
                    }
                }
                break;
            }

            case LinkEvent::Type::Disconnect:
                if (status_.load() != Status::Rejected) {
                    status_.store(Status::Failed);
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    peers_ = {};
                }
                break;

            default:
                break;
        }
    }

    ping_ms_.store(impl_->link->ping_ms());
}

} // namespace bcnet
