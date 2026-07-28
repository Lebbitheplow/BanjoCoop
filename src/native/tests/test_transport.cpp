/* Headless transport tests — no ROM, no game, no GPU.
 *
 * Runs a host and one or more clients as Transport objects in a single process talking over
 * loopback, which is exactly how the two-instance dev loop will work later, minus the game.
 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "banjocoop/byteorder.hpp"
#include "banjocoop/transport.hpp"

using namespace bcnet;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what);
    if (!cond) {
        g_failures++;
    }
}

/* Poll until `pred` holds or the timeout expires. Everything here is asynchronous, so a fixed
 * sleep would be both slower and flakier. */
template <typename F>
bool wait_for(F pred, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

Identity make_identity(const std::string& name, uint32_t mod_version = 1) {
    Identity id;
    id.mod_version = mod_version;
    id.name = name;
    id.rom_hash.fill(0xAB);
    return id;
}

/* These structs live in rdram, which is host-native (see protocol.h) — so plain assignment.
 * The big-endian conversion happens only on the wire, inside transport.cpp. */
bc_player_state build_state(float x, float y, float z, uint32_t map_id) {
    bc_player_state s{};
    s.map_id = map_id;
    s.flags = BCNET_STATE_ACTIVE | BCNET_STATE_INGAME;
    s.pos[0] = x;
    s.pos[1] = y;
    s.pos[2] = z;
    s.yaw = 90.0f;
    s.transform = 0;
    return s;
}

uint16_t pick_port() {
    /* Derived from the pid rather than fixed.
     *
     * A hardcoded port collides with two things: a real game instance hosting on the default
     * 34567, and this suite's own sockets from a run moments earlier still sitting in TIME_WAIT.
     * Both produce a wall of failures that look like a replication regression and are not — which
     * cost two separate false alarms before this changed. One port per process makes the suite
     * say something about the code and nothing about what else is on the machine. */
    return static_cast<uint16_t>(40000 + (static_cast<unsigned>(::getpid()) % 20000));
}

void test_connect_and_relay() {
    std::printf("test: connect and state relay\n");

    Transport host, client;
    std::string err;

    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(client.join("127.0.0.1", pick_port(), make_identity("client"), err), "client connects");

    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");
    check(client.local_player_id() == 1, "client assigned player id 1");
    check(host.local_player_id() == 0, "host is player id 0");

    host.set_local_state(build_state(100.0f, 200.0f, 300.0f, 0x1A));
    client.set_local_state(build_state(-50.0f, 25.0f, 75.0f, 0x1A));

    /* Client should see the host's state. */
    bc_incoming inc{};
    bool got = wait_for([&] {
        client.snapshot(inc);
        return inc.remote_count == 1 &&
               inc.remotes[0].state.pos[0] == 100.0f;
    });
    check(got, "client sees exactly one remote, with real state");

    if (got) {
        check(inc.remotes[0].player_id == 0, "remote is the host (id 0)");
        check(inc.remotes[0].state.pos[0] == 100.0f, "pos.x survives the round trip");
        check(inc.remotes[0].state.pos[1] == 200.0f, "pos.y survives the round trip");
        check(inc.remotes[0].state.pos[2] == 300.0f, "pos.z survives the round trip");
        check(inc.remotes[0].state.map_id == 0x1A, "map_id survives the round trip");
    }

    /* Host should see the client's state. */
    bc_incoming hinc{};
    bool hgot = wait_for([&] {
        host.snapshot(hinc);
        return hinc.remote_count == 1 &&
               hinc.remotes[0].state.pos[0] == -50.0f;
    });
    check(hgot, "host sees the client's state");

    client.shutdown();
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 0;
          }),
          "host drops the client on disconnect");

    host.shutdown();
}

/* The host relays client state to other clients. Clients never talk to each other directly, so
 * this path is only exercised with three or more players — and it is the one most likely to be
 * silently wrong, since with two players everything still works if relaying is broken. */
void test_host_relays_between_clients() {
    std::printf("test: host relays state between clients\n");

    Transport host, a, b;
    std::string err;

    host.host(pick_port(), make_identity("host"), err);
    a.join("127.0.0.1", pick_port(), make_identity("alice"), err);
    check(wait_for([&] { return a.status() == Status::Connected; }), "alice connects");
    b.join("127.0.0.1", pick_port(), make_identity("bob"), err);
    check(wait_for([&] { return b.status() == Status::Connected; }), "bob connects");

    check(a.local_player_id() == 1, "alice is player 1");
    check(b.local_player_id() == 2, "bob is player 2");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 5));
    a.set_local_state(build_state(11.0f, 12.0f, 13.0f, 5));
    b.set_local_state(build_state(21.0f, 22.0f, 23.0f, 5));

    /* Bob must see Alice's state, which can only have reached him via the host. */
    bc_incoming binc{};
    bool relayed = wait_for([&] {
        b.snapshot(binc);
        uint32_t n = binc.remote_count;
        for (uint32_t i = 0; i < n; i++) {
            if (binc.remotes[i].player_id == 1 &&
                binc.remotes[i].state.pos[0] == 11.0f) {
                return true;
            }
        }
        return false;
    });
    check(relayed, "bob receives alice's state via the host");

    check(wait_for([&] {
              bc_incoming s{};
              b.snapshot(s);
              return s.remote_count == 2;
          }),
          "bob sees both host and alice");

    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 2;
          }),
          "host sees both clients");

    a.shutdown();
    b.shutdown();
    host.shutdown();
}

/* Regression: the client must keep seeing the host's state CHANGE over time, not just receive
 * one snapshot. The original tests only ever checked that a single value arrived, which passes
 * even if updates stop flowing after the first packet. Both directions are checked, because the
 * reported symptom was asymmetric — host saw the client move, client saw the host frozen. */
void test_continuous_updates_both_directions() {
    std::printf("test: state keeps updating in both directions\n");

    Transport host, client;
    std::string err;

    host.host(pick_port(), make_identity("host"), err);
    client.join("127.0.0.1", pick_port(), make_identity("client"), err);
    check(wait_for([&] { return client.status() == Status::Connected; }), "client connects");

    /* Drive both ends through a sequence of distinct positions and record what the other side
     * observes. A frozen link shows up as only ever seeing the first value. */
    auto observed_distinct = [](Transport& sender, Transport& receiver, float base) {
        std::vector<float> seen;
        for (int step = 1; step <= 6; step++) {
            sender.set_local_state(build_state(base + step * 10.0f, 0.0f, 0.0f, 9));
            auto deadline = std::chrono::steady_clock::now() + 500ms;
            while (std::chrono::steady_clock::now() < deadline) {
                bc_incoming inc{};
                receiver.snapshot(inc);
                if (inc.remote_count == 1) {
                    float x = inc.remotes[0].state.pos[0];
                    if (seen.empty() || seen.back() != x) {
                        seen.push_back(x);
                    }
                    if (x == base + step * 10.0f) {
                        break;
                    }
                }
                std::this_thread::sleep_for(5ms);
            }
        }
        return seen.size();
    };

    size_t client_saw = observed_distinct(host, client, 1000.0f);
    std::printf("       client observed %zu distinct host positions (want >= 5)\n", client_saw);
    check(client_saw >= 5, "client keeps receiving host updates");

    size_t host_saw = observed_distinct(client, host, 2000.0f);
    std::printf("       host observed %zu distinct client positions (want >= 5)\n", host_saw);
    check(host_saw >= 5, "host keeps receiving client updates");

    client.shutdown();
    host.shutdown();
}

void test_rom_hash_mismatch_rejected() {
    std::printf("test: ROM hash mismatch is rejected\n");

    Transport host, client;
    std::string err;

    host.host(pick_port(), make_identity("host"), err);

    Identity bad = make_identity("client");
    bad.rom_hash.fill(0x00); /* different ROM */
    client.join("127.0.0.1", pick_port(), bad, err);

    check(wait_for([&] { return client.status() == Status::Rejected; }), "client is rejected");
    check(client.reject_reason() == BCNET_REJECT_ROM_HASH, "reason is ROM_HASH");

    client.shutdown();
    host.shutdown();
}

void test_mod_version_mismatch_rejected() {
    std::printf("test: mod version mismatch is rejected\n");

    Transport host, client;
    std::string err;

    host.host(pick_port(), make_identity("host", 1), err);
    client.join("127.0.0.1", pick_port(), make_identity("client", 2), err);

    check(wait_for([&] { return client.status() == Status::Rejected; }), "client is rejected");
    check(client.reject_reason() == BCNET_REJECT_MOD_VERSION, "reason is MOD_VERSION");

    client.shutdown();
    host.shutdown();
}

void test_latency_simulation() {
    std::printf("test: latency simulation delays delivery\n");

    Transport host, client;
    std::string err;

    NetSimConfig sim;
    sim.latency_ms = 150;

    host.host(pick_port(), make_identity("host"), err);
    client.join("127.0.0.1", pick_port(), make_identity("client"), err);
    check(wait_for([&] { return client.status() == Status::Connected; }), "client connects");

    host.configure_sim(sim);
    host.set_local_state(build_state(1.0f, 2.0f, 3.0f, 7));

    auto start = std::chrono::steady_clock::now();
    bool got = wait_for([&] {
        bc_incoming inc{};
        client.snapshot(inc);
        return inc.remote_count == 1 && inc.remotes[0].state.pos[0] == 1.0f;
    });
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    check(got, "state arrives despite simulated latency");
    check(elapsed.count() >= 140, "delivery was actually delayed (>=140ms)");
    std::printf("       observed delay: %lld ms\n", static_cast<long long>(elapsed.count()));

    client.shutdown();
    host.shutdown();
}

/* ---- world events (Phase 3) ---------------------------------------------------------------- */

bc_event_queue one_event(uint32_t kind, uint32_t map_id, uint32_t level_id, uint32_t a, uint32_t b) {
    bc_event_queue q{};
    q.count = 1;
    q.events[0] = bc_event{kind, map_id, level_id, 0, a, b, 0};
    return q;
}

/* snapshot() drains the event queue, so a single poll would race the arrival. Accumulate across
 * polls instead and let the caller assert on the total. */
void collect_events(Transport& t, std::vector<bc_event>& out,
                    std::chrono::milliseconds window = 600ms) {
    auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        bc_incoming inc{};
        t.snapshot(inc);
        for (uint32_t i = 0; i < inc.events.count; i++) {
            out.push_back(inc.events.events[i]);
        }
        std::this_thread::sleep_for(5ms);
    }
}

size_t count_kind(const std::vector<bc_event>& evs, uint32_t kind) {
    size_t n = 0;
    for (const bc_event& e : evs) {
        if (e.kind == kind) {
            n++;
        }
    }
    return n;
}

/* Both directions of the reliable event channel: a client's event reaching the host, and the
 * host's event reaching a client. */
void test_world_events_round_trip() {
    std::printf("test: world events travel both ways\n");

    Transport host, client;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(client.join("127.0.0.1", pick_port(), make_identity("client"), err), "client connects");
    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 1;
          }),
          "host has the client's state");

    /* Client -> host. */
    client.submit_events(one_event(BC_EV_JIGGY, 0x02, 0x01, 0x0A, 1));
    std::vector<bc_event> at_host;
    collect_events(host, at_host);
    check(count_kind(at_host, BC_EV_JIGGY) == 1, "host receives the client's jiggy event once");
    if (count_kind(at_host, BC_EV_JIGGY) == 1) {
        check(at_host[0].a == 0x0A, "jiggy id survives the round trip");
        /* Stamped by the host from the connection, not trusted from the packet — so a toast can
         * name who did it, and cannot be made to name somebody else. */
        check(at_host[0].origin == 1u, "the event is attributed to the client that sent it");
    }

    /* Host -> client. */
    host.submit_events(one_event(BC_EV_FLAG_FILEPROG, 0x02, 0x01, 0x2A, 1));
    std::vector<bc_event> at_client;
    collect_events(client, at_client);
    check(count_kind(at_client, BC_EV_FLAG_FILEPROG) == 1,
          "client receives the host's file-progress flag once");

    /* An event must never come back to the peer that raised it, or every collection would be
     * applied twice by whoever made it. */
    std::vector<bc_event> echo;
    collect_events(client, echo, 300ms);
    check(count_kind(echo, BC_EV_JIGGY) == 0, "client does not receive its own event back");

    client.shutdown();
    host.shutdown();
}

/* The exit criterion for Phase 3: two players grabbing the same note must not score it twice.
 *
 * Both clients claim note 7 in map 2. The host admits the first and silently drops the second, so
 * the note is relayed exactly once. Crucially the loser is NOT sent a correction — its own pickup
 * is the one count it should have, and deducting on top of that would leave it with nothing. */
void test_note_double_claim_is_dropped() {
    std::printf("test: a second claim on the same note is dropped, not reversed\n");

    Transport host, alice, bob;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(alice.join("127.0.0.1", pick_port(), make_identity("alice"), err), "alice connects");
    check(wait_for([&] { return alice.status() == Status::Connected; }), "alice reaches Connected");
    check(bob.join("127.0.0.1", pick_port(), make_identity("bob"), err), "bob connects");
    check(wait_for([&] { return bob.status() == Status::Connected; }), "bob reaches Connected");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    alice.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    bob.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 2;
          }),
          "host has both clients' state");

    alice.submit_events(one_event(BC_EV_NOTE_STATIC, 0x02, 0x01, 7, 0));
    /* Let alice's claim be adjudicated first, so the outcome under test is deterministic. A
     * genuine simultaneous claim resolves the same way, just with the winner decided by arrival
     * order rather than by the test. */
    std::this_thread::sleep_for(200ms);
    bob.submit_events(one_event(BC_EV_NOTE_STATIC, 0x02, 0x01, 7, 0));

    std::vector<bc_event> at_bob, at_host, at_alice;
    collect_events(bob, at_bob);
    collect_events(host, at_host, 100ms);
    collect_events(alice, at_alice, 100ms);

    check(count_kind(at_bob, BC_EV_NOTE_STATIC) == 1, "bob is told alice took note 7, once");
    check(count_kind(at_host, BC_EV_NOTE_STATIC) == 1,
          "the host counts the note exactly once, not twice");
    check(count_kind(at_alice, BC_EV_NOTE_STATIC) == 0, "alice is not told about her own note");
    /* Nothing may come back to the loser. Anything at all here would be the old revoke, which
     * deducted a point the player had legitimately collected. */
    check(at_bob.size() == 1, "bob gets nothing back for his losing claim");

    bob.shutdown();
    alice.shutdown();
    host.shutdown();
}

/* Map- and level-scoped flags must not reach players who are somewhere else. This is what makes
 * free-roam possible later, and it is much easier to verify here than in-game. */
void test_flag_routing_respects_scope() {
    std::printf("test: scoped flags only reach peers in the same map/level\n");

    Transport host, near, far;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(near.join("127.0.0.1", pick_port(), make_identity("near"), err), "near connects");
    check(wait_for([&] { return near.status() == Status::Connected; }), "near reaches Connected");
    check(far.join("127.0.0.1", pick_port(), make_identity("far"), err), "far connects");
    check(wait_for([&] { return far.status() == Status::Connected; }), "far reaches Connected");

    /* host and `near` share map 2 / level 1; `far` is in map 0x27 / level 4. */
    bc_player_state host_state = build_state(0.0f, 0.0f, 0.0f, 0x02);
    host_state.level_id = 1;
    bc_player_state near_state = build_state(0.0f, 0.0f, 0.0f, 0x02);
    near_state.level_id = 1;
    bc_player_state far_state = build_state(0.0f, 0.0f, 0.0f, 0x27);
    far_state.level_id = 4;

    host.set_local_state(host_state);
    near.set_local_state(near_state);
    far.set_local_state(far_state);

    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 2 && s.remotes[0].state.level_id != 0 &&
                     s.remotes[1].state.level_id != 0;
          }),
          "host knows where both clients are");

    host.submit_events(one_event(BC_EV_FLAG_MAP, 0x02, 1, 0x11, 1));
    host.submit_events(one_event(BC_EV_FLAG_LEVEL, 0x02, 1, 0x22, 1));
    host.submit_events(one_event(BC_EV_FLAG_FILEPROG, 0x02, 1, 0x33, 1));

    std::vector<bc_event> at_near, at_far;
    collect_events(near, at_near);
    collect_events(far, at_far, 100ms);

    check(count_kind(at_near, BC_EV_FLAG_MAP) == 1, "same-map peer gets the map flag");
    check(count_kind(at_near, BC_EV_FLAG_LEVEL) == 1, "same-level peer gets the level flag");
    check(count_kind(at_near, BC_EV_FLAG_FILEPROG) == 1, "same-map peer gets the progress flag");

    check(count_kind(at_far, BC_EV_FLAG_MAP) == 0, "distant peer does NOT get the map flag");
    check(count_kind(at_far, BC_EV_FLAG_LEVEL) == 0, "distant peer does NOT get the level flag");
    check(count_kind(at_far, BC_EV_FLAG_FILEPROG) == 1,
          "distant peer still gets file progress, which is world-wide");

    far.shutdown();
    near.shutdown();
    host.shutdown();
}

/* Events are the one thing that must survive a lossy link: a dropped "the note is gone" is a
 * permanent divergence, unlike a dropped position which the next packet supersedes. */
void test_events_survive_packet_loss() {
    std::printf("test: events survive 150ms latency with 3%% loss\n");

    Transport host, client;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(client.join("127.0.0.1", pick_port(), make_identity("client"), err), "client connects");
    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 1;
          }),
          "host has the client's state");

    NetSimConfig cfg;
    cfg.latency_ms = 150;
    cfg.jitter_ms = 20;
    cfg.loss = 0.03f;
    host.configure_sim(cfg);
    client.configure_sim(cfg);

    const uint32_t kNotes = 40;
    for (uint32_t i = 0; i < kNotes; i++) {
        client.submit_events(one_event(BC_EV_NOTE_STATIC, 0x02, 0x01, i, 0));
    }

    std::vector<bc_event> at_host;
    collect_events(host, at_host, 4000ms);
    check(count_kind(at_host, BC_EV_NOTE_STATIC) == kNotes,
          "every note claim arrives despite loss");
    std::printf("       %zu of %u claims arrived\n", count_kind(at_host, BC_EV_NOTE_STATIC), kNotes);

    client.shutdown();
    host.shutdown();
}

/* Phase 4. A player arriving in a map has missed everything that happened there while they were
 * elsewhere, and a player who just joined has missed everything everywhere. Arrival is detected
 * from the state stream, so this must fire without the client asking for anything. */
void test_map_entry_snapshot() {
    std::printf("test: arriving in a map replays that map's state\n");

    Transport host, client;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");

    /* The host collects three notes in map 2 with nobody else connected. */
    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    for (uint32_t note : {3u, 9u, 21u}) {
        host.submit_events(one_event(BC_EV_NOTE_STATIC, 0x02, 0x01, note, 0));
    }
    std::this_thread::sleep_for(300ms);

    /* A player joins afterwards and is in map 2. They were not there for any of it. */
    check(client.join("127.0.0.1", pick_port(), make_identity("late"), err), "client connects");
    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));

    std::vector<bc_event> at_client;
    collect_events(client, at_client);
    check(count_kind(at_client, BC_EV_NOTE_STATIC) == 3,
          "late joiner is sent all three notes for the map it is in");

    /* Moving to a map with no recorded state must not replay anything. */
    at_client.clear();
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x07));
    collect_events(client, at_client);
    check(count_kind(at_client, BC_EV_NOTE_STATIC) == 0, "an untouched map replays nothing");

    /* Coming back replays it again. That is intentional: the receiver ignores notes it already
     * has, so re-sending is harmless and means arrival never has to be tracked per client. */
    at_client.clear();
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    collect_events(client, at_client);
    check(count_kind(at_client, BC_EV_NOTE_STATIC) == 3, "returning replays the map again");

    client.shutdown();
    host.shutdown();
}

/* Phase 5 foundation. Enemies are a continuous signal, not a discrete fact: the owner publishes
 * their state every tick and a lost packet is superseded rather than missed. So unlike events,
 * only the newest frame is kept and nothing is queued or retransmitted. */
void test_object_frames_flow_from_owner() {
    std::printf("test: the owner's object frames reach other players\n");

    Transport host, client;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(client.join("127.0.0.1", pick_port(), make_identity("client"), err), "client connects");
    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    client.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));

    bc_object_frame frame{};
    frame.map_id = 0x02;
    frame.count = 2;
    frame.objects[0] =
        bc_object_state{101u, BC_OBJ_ACTIVE, 0u, {10.0f, 20.0f, 30.0f}, 45.0f, 7u, 1.5f};
    /* actor_id set: something the receiver must spawn for itself, like a projectile. */
    frame.objects[1] =
        bc_object_state{102u, BC_OBJ_ACTIVE, 0x2Du, {-5.0f, 0.0f, 5.0f}, 90.0f, 8u, 0.25f};
    host.submit_objects(frame);

    bc_incoming inc{};
    bool got = wait_for([&] {
        client.snapshot(inc);
        return inc.objects.count == 2;
    });
    check(got, "client receives the host's object frame");
    if (got) {
        check(inc.objects.map_id == 0x02, "frame carries the map it describes");
        check(inc.objects.objects[0].net_id == 101u, "net id survives the round trip");
        check(inc.objects.objects[0].pos[1] == 20.0f, "position survives the round trip");
        check(inc.objects.objects[1].yaw == 90.0f, "yaw survives the round trip");
        check(inc.objects.objects[1].anim_frame == 0.25f, "animation frame survives the round trip");
        check(inc.objects.objects[1].actor_id == 0x2Du,
              "actor id survives, so a receiver can spawn what it has never seen");
        check(inc.objects.objects[0].actor_id == 0u, "objects that need no spawn carry none");
    }

    /* Superseding, not accumulating: a second frame replaces the first rather than queueing. */
    frame.count = 1;
    frame.objects[0].pos[1] = 999.0f;
    host.submit_objects(frame);
    check(wait_for([&] {
              bc_incoming s{};
              client.snapshot(s);
              return s.objects.count == 1 && s.objects.objects[0].pos[1] == 999.0f;
          }),
          "a newer frame replaces the previous one");

    /* A peer that owns nothing must not publish, or it would fight the real owner. */
    bc_incoming hinc{};
    host.snapshot(hinc);
    check(hinc.objects.count == 0, "host receives nothing from a client that owns no objects");

    client.shutdown();
    host.shutdown();
}

/* Phase 6. The host's save is authoritative and clients mirror it. Unlike the object stream this
 * is reliable and resent on a slow cadence, because a mirror that goes missing leaves a client
 * unable to enter a world with no way to recover — and nothing arriving later supersedes it. */
void test_progression_mirror() {
    std::printf("test: the host's progression mirror reaches clients and repeats\n");

    Transport host, client;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(client.join("127.0.0.1", pick_port(), make_identity("client"), err), "client connects");
    check(wait_for([&] { return client.status() == Status::Connected; }), "client reaches Connected");

    bc_progress prog{};
    prog.valid = 1;
    prog.words[0] = 0xDEADBEEFu;
    prog.words[5] = 0x00000001u;
    prog.words[BC_PROGRESS_WORDS - 1] = 0xFFFFFFFFu;
    host.submit_progress(prog);

    bc_incoming inc{};
    bool got = wait_for([&] {
        client.snapshot(inc);
        return inc.progress.valid != 0;
    });
    check(got, "client receives the mirror");
    if (got) {
        check(inc.progress.words[0] == 0xDEADBEEFu, "first word survives the round trip");
        check(inc.progress.words[5] == 0x00000001u, "middle word survives the round trip");
        check(inc.progress.words[BC_PROGRESS_WORDS - 1] == 0xFFFFFFFFu,
              "last word survives — the whole array is carried, not a prefix");
    }

    /* Self-healing is the point: a client that missed one must be corrected by the next, so the
     * host has to keep sending rather than publishing once on join. */
    prog.words[0] = 0x12345678u;
    host.submit_progress(prog);
    check(wait_for([&] {
              bc_incoming s{};
              client.snapshot(s);
              return s.progress.words[0] == 0x12345678u;
          }),
          "an updated mirror is resent and replaces the old one");

    /* Clients are not authoritative and must never publish one back. */
    bc_incoming hinc{};
    host.snapshot(hinc);
    check(hinc.progress.valid == 0, "the host receives no mirror from a client");

    client.shutdown();
    host.shutdown();
}

/* Ownership is the lowest player id *present in a map*, not the host — so whenever the host is
 * somewhere else, a client owns the enemies and projectiles, and its frames have to reach the
 * other players in that map. They pass through the host, which is not in that map and has no
 * interest in them, so it is purely a relay. Without it, enemies and projectiles only ever sync
 * in whichever map the host happens to be standing in. */
void test_client_owned_objects_reach_other_clients() {
    std::printf("test: a client's object frames reach other clients when the host is elsewhere\n");

    Transport host, alice, bob;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(alice.join("127.0.0.1", pick_port(), make_identity("alice"), err), "alice connects");
    check(wait_for([&] { return alice.status() == Status::Connected; }), "alice reaches Connected");
    check(bob.join("127.0.0.1", pick_port(), make_identity("bob"), err), "bob connects");
    check(wait_for([&] { return bob.status() == Status::Connected; }), "bob reaches Connected");

    /* The host is in the lair; both clients are in Mumbo's Mountain, so alice (id 1) owns it. */
    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x69));
    alice.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    bob.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 2 && s.remotes[0].state.map_id != 0 &&
                     s.remotes[1].state.map_id != 0;
          }),
          "host knows where both clients are");

    bc_object_frame frame{};
    frame.map_id = 0x02;
    frame.count = 1;
    frame.objects[0] =
        bc_object_state{7u, BC_OBJ_ACTIVE, 0x2Du, {1.0f, 2.0f, 3.0f}, 12.0f, 0u, 0.0f};
    alice.submit_objects(frame);

    check(wait_for([&] {
              bc_incoming s{};
              bob.snapshot(s);
              return s.objects.count == 1 && s.objects.objects[0].net_id == 7u &&
                     s.objects.objects[0].actor_id == 0x2Du;
          }),
          "bob receives alice's frame even though the host is in another map");

    /* And the host, which is not in that map, must not be driving its own world from it. */
    bc_incoming hinc{};
    host.snapshot(hinc);
    check(hinc.objects.count == 0 || hinc.objects.map_id == 0x02,
          "the host relays without adopting another map's objects as its own");

    bob.shutdown();
    alice.shutdown();
    host.shutdown();
}

/* The mirror of the test above: the host's *own* frames must be routed the same way it routes a
 * client's. A peer in another map discards an object frame on arrival, so sending it there is
 * bytes spent to be thrown away — and under free-roam "everyone else is somewhere else" is the
 * common case, not the edge case.
 *
 * The negative half is the point. Alice receiving it only proves the send still works; bob *not*
 * receiving it is what proves the filter exists at all. */
void test_host_objects_skip_other_maps() {
    std::printf("test: the host's object frames go only to players in that map\n");

    Transport host, alice, bob;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(alice.join("127.0.0.1", pick_port(), make_identity("alice"), err), "alice connects");
    check(wait_for([&] { return alice.status() == Status::Connected; }), "alice reaches Connected");
    check(bob.join("127.0.0.1", pick_port(), make_identity("bob"), err), "bob connects");
    check(wait_for([&] { return bob.status() == Status::Connected; }), "bob reaches Connected");

    /* The host and alice are in Mumbo's Mountain; bob is off in the lair. */
    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    alice.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    bob.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x69));
    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 2 && s.remotes[0].state.map_id != 0 &&
                     s.remotes[1].state.map_id != 0;
          }),
          "host knows where both clients are");

    bc_object_frame frame{};
    frame.map_id = 0x02;
    frame.count = 1;
    frame.objects[0] =
        bc_object_state{55u, BC_OBJ_ACTIVE, 0u, {4.0f, 5.0f, 6.0f}, 33.0f, 0u, 0.0f};
    host.submit_objects(frame);

    check(wait_for([&] {
              bc_incoming s{};
              alice.snapshot(s);
              return s.objects.count == 1 && s.objects.objects[0].net_id == 55u;
          }),
          "alice, in the same map, receives the host's frame");

    /* Checked after alice's arrival, so the frame has demonstrably been sent and this is not
     * merely a race that bob would have lost anyway. */
    bc_incoming binc{};
    bob.snapshot(binc);
    check(binc.objects.count == 0, "bob, in another map, is never sent it");

    bob.shutdown();
    alice.shutdown();
    host.shutdown();
}

/* A player leaving has to reach the *other* clients, not just the host.
 *
 * From the state stream a departure is indistinguishable from someone going quiet, so without an
 * explicit roster the remaining clients keep the leaver forever — and the mod, still being told
 * about them, leaves their puppet standing in the world. The host knew all along; nobody told
 * anyone else. Only visible with three players, which is why it survived so long. */
void test_departure_reaches_other_clients() {
    std::printf("test: a player leaving is seen by the other clients, not just the host\n");

    Transport host, alice, bob;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(alice.join("127.0.0.1", pick_port(), make_identity("alice"), err), "alice connects");
    check(wait_for([&] { return alice.status() == Status::Connected; }), "alice reaches Connected");
    check(bob.join("127.0.0.1", pick_port(), make_identity("bob"), err), "bob connects");
    check(wait_for([&] { return bob.status() == Status::Connected; }), "bob reaches Connected");

    host.set_local_state(build_state(0.0f, 0.0f, 0.0f, 0x02));
    alice.set_local_state(build_state(1.0f, 0.0f, 0.0f, 0x02));
    bob.set_local_state(build_state(2.0f, 0.0f, 0.0f, 0x02));

    check(wait_for([&] {
              bc_incoming s{};
              bob.snapshot(s);
              return s.remote_count == 2;
          }),
          "bob sees the host and alice");

    alice.shutdown();

    /* The bug: bob kept alice forever, so her puppet never went away. */
    check(wait_for([&] {
              bc_incoming s{};
              bob.snapshot(s);
              return s.remote_count == 1;
          }),
          "bob stops seeing alice once she leaves");

    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.remote_count == 1;
          }),
          "the host drops her too");

    bob.shutdown();
    host.shutdown();
}

/* Chat has to reach the other clients, not just the host — the same relay shape that OBJECTS and
 * PEERS both got wrong, so it is worth asserting rather than assuming. */
void test_chat_reaches_everyone() {
    std::printf("test: chat from a client reaches the host and the other clients\n");

    Transport host, alice, bob;
    std::string err;
    check(host.host(pick_port(), make_identity("host"), err), "host binds");
    check(alice.join("127.0.0.1", pick_port(), make_identity("alice"), err), "alice connects");
    check(wait_for([&] { return alice.status() == Status::Connected; }), "alice reaches Connected");
    check(bob.join("127.0.0.1", pick_port(), make_identity("bob"), err), "bob connects");
    check(wait_for([&] { return bob.status() == Status::Connected; }), "bob reaches Connected");

    bc_chat_line line{};
    line.length = 5;
    line.text[0] = 0x68656C6Cu; /* arbitrary packed bytes; the mod does the packing */
    line.text[1] = 0x6F000000u;
    alice.submit_chat(line);

    check(wait_for([&] {
              bc_incoming s{};
              host.snapshot(s);
              return s.chat.count == 1 && s.chat.lines[0].from == 1u &&
                     s.chat.lines[0].text[0] == 0x68656C6Cu;
          }),
          "the host receives alice's line, attributed to her");

    check(wait_for([&] {
              bc_incoming s{};
              bob.snapshot(s);
              return s.chat.count == 1 && s.chat.lines[0].from == 1u;
          }),
          "bob receives it too, relayed, still attributed to alice");

    /* And the host's own line reaches the clients. */
    bc_chat_line hline{};
    hline.length = 3;
    hline.text[0] = 0x796F21u;
    host.submit_chat(hline);
    check(wait_for([&] {
              bc_incoming s{};
              bob.snapshot(s);
              return s.chat.count == 2 && s.chat.lines[1].from == 0u;
          }),
          "the host's own line reaches the clients");

    bob.shutdown();
    alice.shutdown();
    host.shutdown();
}

} // namespace

int main() {
    test_connect_and_relay();
    test_host_relays_between_clients();
    test_continuous_updates_both_directions();
    test_rom_hash_mismatch_rejected();
    test_mod_version_mismatch_rejected();
    test_latency_simulation();
    test_world_events_round_trip();
    test_note_double_claim_is_dropped();
    test_flag_routing_respects_scope();
    test_events_survive_packet_loss();
    test_map_entry_snapshot();
    test_object_frames_flow_from_owner();
    test_progression_mirror();
    test_client_owned_objects_reach_other_clients();
    test_host_objects_skip_other_maps();
    test_departure_reaches_other_clients();
    test_chat_reaches_everyone();

    if (g_failures == 0) {
        std::printf("\nall transport tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
