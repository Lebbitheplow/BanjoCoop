/* The reliable world-event channel: the mod's queued events going out, and the host's decision
 * about what happens to each one coming back.
 *
 * The split from transport.cpp is by concern, not by convenience — everything here is about
 * discrete world changes, where losing one is permanent, while transport.cpp handles the
 * connection and the continuous player-state stream, where losing a packet is harmless.
 */

#include "transport_internal.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace bcnet {

/* bc_event is 6 four-byte fields. Serialised explicitly so the wire stays big-endian regardless
 * of host endianness, and so adding a field without bumping BCNET_PROTOCOL_VERSION shows up as a
 * size mismatch here rather than as garbled world state in-game. */
constexpr size_t kEventWireSize = 7 * 4;

static_assert(sizeof(bc_event) == kEventWireSize,
              "bc_event gained a field or padding; update the wire format and bump "
              "BCNET_PROTOCOL_VERSION");

void write_event(Writer& w, const bc_event& e) {
    w.u32(e.kind);
    w.u32(e.map_id);
    w.u32(e.level_id);
    w.u32(e.origin);
    w.u32(e.a);
    w.u32(e.b);
    w.u32(e.c);
}

bool read_event(Reader& r, bc_event& e) {
    e.kind = r.u32();
    e.map_id = r.u32();
    e.level_id = r.u32();
    e.origin = r.u32();
    e.a = r.u32();
    e.b = r.u32();
    e.c = r.u32();
    return r.ok;
}

/* bc_object_state is 8 four-byte fields. Same explicit big-endian treatment as everything else. */
constexpr size_t kObjectWireSize = 9 * 4;

static_assert(sizeof(bc_object_state) == kObjectWireSize,
              "bc_object_state gained a field or padding; update the wire format and bump "
              "BCNET_PROTOCOL_VERSION");

void write_object(Writer& w, const bc_object_state& o) {
    w.u32(o.net_id);
    w.u32(o.flags);
    w.u32(o.actor_id);
    for (int i = 0; i < 3; i++) {
        w.u32(float_bits(o.pos[i]));
    }
    w.u32(float_bits(o.yaw));
    w.u32(o.anim_id);
    w.u32(float_bits(o.anim_frame));
}

bool read_object(Reader& r, bc_object_state& o) {
    o.net_id = r.u32();
    o.flags = r.u32();
    o.actor_id = r.u32();
    for (int i = 0; i < 3; i++) {
        o.pos[i] = bits_float(r.u32());
    }
    o.yaw = bits_float(r.u32());
    o.anim_id = r.u32();
    o.anim_frame = bits_float(r.u32());
    return r.ok;
}

void Transport::submit_events(const bc_event_queue& out) {
    if (out.count == 0) {
        return;
    }
    Status st = status_.load(std::memory_order_relaxed);
    if (st != Status::Hosting && st != Status::Connected) {
        return;
    }

    std::lock_guard<std::mutex> lock(outbox_mutex_);
    uint32_t n = out.count < BCNET_EVENT_QUEUE ? out.count : BCNET_EVENT_QUEUE;
    for (uint32_t i = 0; i < n; i++) {
        outbox_.push_back(out.events[i]);
    }
}

void Transport::drain_outbox() {
    std::deque<bc_event> pending;
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        pending.swap(outbox_);
    }

    const bool hosting = is_host_.load(std::memory_order_relaxed);
    for (const bc_event& ev : pending) {
        if (hosting) {
            /* We are the authority, so our own events are adjudicated here and now — same path a
             * client's events take when they arrive, just with no round trip. Passing our own
             * player id as the origin is what stops the host re-applying its own event to
             * itself. */
            adjudicate(ev, local_player_id_.load(std::memory_order_relaxed));
        } else {
            /* Clients never decide anything; they report and wait to be told. */
            Writer w(BCNET_MSG_EVENT);
            write_event(w, ev);
            std::lock_guard<std::mutex> lock(sim_mutex_);
            sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_EVENT, true);
        }
    }
}

void Transport::accept_locally(const bc_event& ev) {
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    if (inbox_.size() >= kMaxInbox) {
        inbox_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    inbox_.push_back(ev);
}

void Transport::send_event_to(const bc_event& ev, uint32_t player_id) {
    if (player_id == local_player_id_.load(std::memory_order_relaxed)) {
        accept_locally(ev);
        return;
    }

    for (const auto& [peer, id] : impl_->peer_ids) {
        if (id != player_id) {
            continue;
        }
        Writer w(BCNET_MSG_EVENT);
        write_event(w, ev);
        std::lock_guard<std::mutex> lock(sim_mutex_);
        sim_.submit(std::move(w.buf), peer, BCNET_CHANNEL_EVENT, true);
        return;
    }
}

/* Object frames are a snapshot, not a change: only the newest matters, and a lost one is
 * superseded rather than missed. That is why they go unreliable, alongside player state, and why
 * only the latest is kept on each side instead of queued. */
void Transport::submit_objects(const bc_object_frame& out) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_objects_ = out;
}

/* Only peers standing in the map a frame describes can use it: `accept_objects` below throws
 * away anything for another map, so sending it there is bytes spent to be discarded on arrival.
 *
 * The host already applies exactly this filter when relaying somebody else's frame (the
 * BCNET_MSG_OBJECTS branch of service_host). Its own frames went out by broadcast instead, which
 * left the asymmetry that a client's objects were routed and the host's were not — and the host is
 * the peer most likely to be alone in a map while everyone else is elsewhere. */
void Transport::broadcast_objects() {
    Status st = status_.load(std::memory_order_relaxed);
    if (st != Status::Hosting && st != Status::Connected) {
        return;
    }

    bc_object_frame frame;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        frame = local_objects_;
    }
    if (frame.count == 0) {
        return;
    }
    uint32_t count = frame.count < BCNET_MAX_OBJECTS ? frame.count : BCNET_MAX_OBJECTS;

    Writer w(BCNET_MSG_OBJECTS);
    w.u32(++object_seq_tx_);
    w.u32(frame.map_id);
    w.u32(count);
    for (uint32_t i = 0; i < count; i++) {
        write_object(w, frame.objects[i]);
    }

    /* A client has exactly one peer — the host — and it must always receive this, map or not,
     * because it is the one that relays to everybody else. Filtering here would silence the
     * client's objects entirely whenever the host stood somewhere else. */
    if (!is_host_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(sim_mutex_);
        sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_STATE, false);
        return;
    }

    std::array<bool, BCNET_MAX_PLAYERS> wants{};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (uint32_t i = 0; i < BCNET_MAX_PLAYERS; i++) {
            wants[i] = peers_[i].active && peers_[i].has_state &&
                       peers_[i].state.map_id == frame.map_id;
        }
    }

    std::lock_guard<std::mutex> lock(sim_mutex_);
    for (const auto& [peer, id] : impl_->peer_ids) {
        if (id >= BCNET_MAX_PLAYERS || !wants[id]) {
            continue;
        }
        std::vector<uint8_t> copy = w.buf;
        sim_.submit(std::move(copy), peer, BCNET_CHANNEL_STATE, false);
    }
}

/* Applied only when it describes the map we are actually in. Without that check a frame arriving
 * just after a transition would drive whatever happens to be standing in the new map to another
 * map's coordinates. */
/* Stale frames are dropped rather than applied. Object state goes out unsequenced, so ENet is
 * free to deliver an older frame after a newer one — and applying it would drag every enemy back
 * to where it was. CoopDX solves this by discarding anything older than the last event id seen;
 * same idea, one counter per sender. */
void Transport::accept_objects(uint32_t seq, const bc_object_frame& frame) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    /* A different map means a different owner, counting from its own zero. Comparing sequence
     * numbers across owners would drop everything until the new one happened to overtake the old
     * — so walking into a map somebody else owns would leave its enemies frozen. */
    if (frame.map_id != remote_objects_.map_id) {
        object_seq_rx_ = 0;
    }
    if (object_seq_rx_ != 0 && seq <= object_seq_rx_) {
        return;
    }
    object_seq_rx_ = seq;
    remote_objects_ = frame;
}

void Transport::submit_progress(const bc_progress& out) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    local_progress_ = out;
}

/* Reliable, unlike object frames: a mirror that goes missing leaves a client's world locked, and
 * unlike a position it is not superseded by anything arriving a frame later. Resent on a slow
 * cadence rather than every tick because it is ~70 bytes of state that rarely changes. */
void Transport::broadcast_progress() {
    if (!is_host_.load(std::memory_order_relaxed)) {
        return;
    }
    if (status_.load(std::memory_order_relaxed) != Status::Hosting) {
        return;
    }

    bc_progress prog;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        prog = local_progress_;
    }
    if (prog.valid == 0) {
        return;
    }

    Writer w(BCNET_MSG_PROGRESS);
    w.u32(prog.note_map);
    for (uint32_t i = 0; i < BC_PROGRESS_NOTE_WORDS; i++) {
        w.u32(prog.note_bits[i]);
    }
    for (uint32_t i = 0; i < BC_PROGRESS_WORDS; i++) {
        w.u32(prog.words[i]);
    }

    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_EVENT, true);
}

void Transport::accept_progress(const bc_progress& prog) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    remote_progress_ = prog;
}

void Transport::set_save_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(save_mutex_);
    save_path_ = path;
}

std::string Transport::take_pending_save_name() {
    std::lock_guard<std::mutex> lock(save_mutex_);
    std::string out;
    out.swap(pending_save_name_);
    return out;
}

/* The host's save, verbatim, to a peer that has just been admitted.
 *
 * Nothing in memory can answer "which notes has the host collected in a map nobody has visited
 * this session" — BanjoRecomp keeps per-note state in a save extension with no accessor on 1.0.1,
 * and the world only knows about the map that is loaded. The save file knows all of it, so the
 * save file is what travels. This is what makes the host's save genuinely the session's save
 * rather than a set of values that happen to be copied. */
void Transport::send_save_file_to(uint32_t peer_index) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(save_mutex_);
        path = save_path_;
    }
    if (path.empty()) {
        return;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("[banjocoop] could not read save file '%s' to send\n", path.c_str());
        std::fflush(stdout);
        return;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > BCNET_MAX_SAVE_BYTES) {
        std::printf("[banjocoop] save file is %zu bytes; not sending\n", bytes.size());
        std::fflush(stdout);
        return;
    }

    Writer w(BCNET_MSG_SAVEFILE);
    w.u32(static_cast<uint32_t>(bytes.size()));
    w.bytes(bytes.data(), bytes.size());

    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.submit(std::move(w.buf), peer_index, BCNET_CHANNEL_EVENT, true);
    std::printf("[banjocoop] sent %zu-byte save file to player\n", bytes.size());
    std::fflush(stdout);
}

/* Written beside our own save, under the mod's own subfolder, so the player's original file is
 * never overwritten — they get it back by disconnecting and reloading it. */
void Transport::store_save_file(const std::vector<uint8_t>& bytes) {
    std::string own;
    {
        std::lock_guard<std::mutex> lock(save_mutex_);
        own = save_path_;
    }
    if (own.empty()) {
        return;
    }

    std::filesystem::path dir = std::filesystem::path(own).parent_path() / "banjocoop";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::filesystem::path out = dir / "hostsave.bin";
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::printf("[banjocoop] could not write host save to '%s'\n", out.string().c_str());
        std::fflush(stdout);
        return;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();

    {
        std::lock_guard<std::mutex> lock(save_mutex_);
        pending_save_name_ = "hostsave";
    }
    std::printf("[banjocoop] stored host save (%zu bytes) at '%s'\n", bytes.size(),
                out.string().c_str());
    std::fflush(stdout);
}

std::string Transport::player_name(uint32_t player_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (player_id >= BCNET_MAX_PLAYERS || !peers_[player_id].active) {
        return {};
    }
    return peers_[player_id].name;
}

void Transport::submit_chat(const bc_chat_line& line) {
    if (line.length == 0) {
        return;
    }
    Status st = status_.load(std::memory_order_relaxed);
    if (st != Status::Hosting && st != Status::Connected) {
        return;
    }

    Writer w(BCNET_MSG_CHAT);
    w.u32(line.length);
    for (uint32_t i = 0; i < BCNET_CHAT_LEN / 4; i++) {
        w.u32(line.text[i]);
    }

    if (is_host_.load(std::memory_order_relaxed)) {
        /* Ours: record it and pass it on, the same path a client's takes. */
        accept_chat(local_player_id_.load(std::memory_order_relaxed), line);
        std::lock_guard<std::mutex> lock(sim_mutex_);
        for (const auto& [peer, id] : impl_->peer_ids) {
            Writer relay(BCNET_MSG_CHAT);
            relay.u32(local_player_id_.load(std::memory_order_relaxed));
            relay.u32(line.length);
            for (uint32_t i = 0; i < BCNET_CHAT_LEN / 4; i++) {
                relay.u32(line.text[i]);
            }
            sim_.submit(std::move(relay.buf), peer, BCNET_CHANNEL_EVENT, true);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_EVENT, true);
}

void Transport::accept_chat(uint32_t from, const bc_chat_line& line) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    bc_chat_line stored = line;
    stored.from = from;
    chat_.push_back(stored);
    while (chat_.size() > BCNET_CHAT_HISTORY) {
        chat_.pop_front();
    }
}

void Transport::chat_log(bc_chat_log& out) const {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    out.count = 0;
    for (const bc_chat_line& l : chat_) {
        if (out.count >= BCNET_CHAT_HISTORY) {
            break;
        }
        out.lines[out.count++] = l;
    }
}

void Transport::broadcast_roster() {
    if (!is_host_.load(std::memory_order_relaxed)) {
        return;
    }

    Writer w(BCNET_MSG_PEERS);
    uint32_t count = 0;
    std::vector<std::pair<uint32_t, std::string>> entries;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (uint32_t i = 0; i < BCNET_MAX_PLAYERS; i++) {
            if (peers_[i].active) {
                entries.emplace_back(peers_[i].player_id, peers_[i].name);
                count++;
            }
        }
    }
    w.u32(count);
    for (const auto& [id, name] : entries) {
        w.u32(id);
        w.name(name);
    }

    std::lock_guard<std::mutex> lock(sim_mutex_);
    sim_.submit(std::move(w.buf), kBroadcast, BCNET_CHANNEL_EVENT, true);
}

void Transport::send_map_snapshot(uint32_t player_id, uint32_t map_id, uint32_t level_id) {
    std::vector<uint32_t> notes;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        for (uint64_t key : collected_notes_) {
            if (static_cast<uint32_t>(key >> 32) == map_id) {
                notes.push_back(static_cast<uint32_t>(key));
            }
        }
    }
    if (notes.empty()) {
        return;
    }

    /* Sent as ordinary NOTE_STATIC events. A note the receiver has already recorded is ignored by
     * its own duplicate check, so replaying the whole map is safe even for a player who was
     * already up to date — which is what lets this fire unconditionally on arrival. */
    for (uint32_t index : notes) {
        bc_event ev{};
        ev.kind = BC_EV_NOTE_STATIC;
        ev.map_id = map_id;
        ev.level_id = level_id;
        ev.a = index;
        send_event_to(ev, player_id);
    }
}

void Transport::relay_event(const bc_event& ev, uint32_t skip_player_id) {
    /* Which peers care depends on the event's scope. Sending a map-specific switch flag to a
     * player three worlds away would apply it to a map they are not in — and under free-roam
     * (Phase 4) that is the normal case, not an edge case, so the routing rule is enforced here
     * from the start rather than retrofitted. */
    /* A custom message says its own scope by which of these it filled in — see banjocoop_send.
     * Handled here rather than as a special case so somebody else's mod inherits exactly the
     * routing BanjoCoop's own events get. */
    const bool custom_map = (ev.kind == BC_EV_CUSTOM && ev.map_id != 0);
    const bool custom_level = (ev.kind == BC_EV_CUSTOM && ev.map_id == 0 && ev.level_id != 0);

    const bool level_scoped = (ev.kind == BC_EV_FLAG_LEVEL || ev.kind == BC_EV_FLAG_LEVEL_N ||
                               ev.kind == BC_EV_JINJO || custom_level);
    const bool map_scoped = (ev.kind == BC_EV_FLAG_MAP || ev.kind == BC_EV_ENEMY_DEAD ||
                             ev.kind == BC_EV_ENEMY_HIT || ev.kind == BC_EV_BOSS_STATE ||
                             ev.kind == BC_EV_FIGHT || ev.kind == BC_EV_CARRY || custom_map);

    auto wants = [&](const bc_player_state& st) {
        if (level_scoped) {
            return st.level_id == ev.level_id;
        }
        if (map_scoped) {
            return st.map_id == ev.map_id;
        }
        return true;
    };

    uint32_t local_id = local_player_id_.load(std::memory_order_relaxed);
    bool local_wants;
    std::array<bool, BCNET_MAX_PLAYERS> peer_wants{};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        local_wants = wants(local_state_);
        for (uint32_t i = 0; i < BCNET_MAX_PLAYERS; i++) {
            peer_wants[i] = peers_[i].active && peers_[i].has_state && wants(peers_[i].state);
        }
    }

    if (skip_player_id != local_id && local_wants) {
        accept_locally(ev);
    }
    for (const auto& [peer, id] : impl_->peer_ids) {
        if (id == skip_player_id || id >= BCNET_MAX_PLAYERS || !peer_wants[id]) {
            continue;
        }
        Writer w(BCNET_MSG_EVENT);
        write_event(w, ev);
        std::lock_guard<std::mutex> lock(sim_mutex_);
        sim_.submit(std::move(w.buf), peer, BCNET_CHANNEL_EVENT, true);
    }
}

bool Transport::adjudicate(const bc_event& in_ev, uint32_t origin) {
    /* Stamped here, from the connection it arrived on, rather than trusted from the packet. */
    bc_event ev = in_ev;
    ev.origin = origin;

    switch (ev.kind) {
        case BC_EV_NOTE_STATIC: {
            /* Where double-collection is decided. Two players touching the same note produce two
             * claims; the second finds the key already present and is dropped here, so the note is
             * only ever relayed once.
             *
             * The loser is NOT told to give the note back. Notes are shared progress — every
             * player's counter shows the level total — so each of them should end up having
             * counted this note exactly once, and the pickup the game already gave the loser IS
             * that one count. Peers who did not touch it get it from the single relay, and a peer
             * who did touch it ignores that relay because it has already recorded the note.
             * Deducting from the loser on top of that just destroys a point. */
            bool first;
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                first = collected_notes_.insert(note_key(ev.map_id, ev.a)).second;
            }
            if (!first) {
                return false;
            }
            relay_event(ev, origin);
            return true;
        }

        default:
            /* Everything else is an idempotent assignment — a flag set to a value, a jiggy bit
             * set. Re-applying one is harmless, so there is nothing to adjudicate and the host
             * only has to decide who hears about it. */
            relay_event(ev, origin);
            return true;
    }
}


} // namespace bcnet
