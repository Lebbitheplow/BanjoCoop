/* Session lifecycle: standing a host or client up, and tearing it down again.
 *
 * Kept apart from the running loop in transport.cpp because these are the only functions that
 * touch enet_host_create/destroy and the enet global init refcount, and because everything here
 * runs on the *game* thread while the loop runs on the net thread.
 */

#include "transport_internal.hpp"

#include <cstdio>

namespace bcnet {

namespace {

/* enet_initialize is global and refcounted, so several Transports (the tests run three at once)
 * can come and go without one shutting the library out from under another. */
std::atomic<int> g_enet_refs{0};

bool enet_acquire(std::string& error) {
    if (g_enet_refs.fetch_add(1) == 0) {
        if (enet_initialize() != 0) {
            g_enet_refs.fetch_sub(1);
            error = "enet_initialize failed";
            return false;
        }
    }
    return true;
}

void enet_release() {
    if (g_enet_refs.fetch_sub(1) == 1) {
        enet_deinitialize();
    }
}

} // namespace

Transport::Transport() : impl_(new Impl()) {}

Transport::~Transport() {
    shutdown();
    delete impl_;
}

bool Transport::host(uint16_t port, const Identity& id, std::string& error) {
    shutdown();
    if (!enet_acquire(error)) {
        return false;
    }

    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    impl_->host = enet_host_create(&addr, BCNET_MAX_PLAYERS, BCNET_CHANNEL_COUNT, 0, 0);
    if (impl_->host == nullptr) {
        enet_release();
        error = "failed to bind port " + std::to_string(port);
        return false;
    }

    identity_ = id;
    impl_->peer_ids.clear();
    impl_->next_player_id = 1;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        peers_ = {};
        /* The host occupies slot 0 so that local and remote players share one id space. */
        peers_[0].active = true;
        peers_[0].player_id = 0;
        peers_[0].name = id.name;
    }

    local_player_id_.store(0);
    is_host_.store(true);
    status_.store(Status::Hosting);
    running_.store(true);
    thread_ = std::thread(&Transport::run, this);
    return true;
}

bool Transport::join(const std::string& address, uint16_t port, const Identity& id, std::string& error) {
    shutdown();
    if (!enet_acquire(error)) {
        return false;
    }

    impl_->host = enet_host_create(nullptr, 1, BCNET_CHANNEL_COUNT, 0, 0);
    if (impl_->host == nullptr) {
        enet_release();
        error = "failed to create client host";
        return false;
    }

    /* join() runs on the GAME thread, called straight out of the frame hook — so anything that
     * blocks here freezes the game.
     *
     * enet_address_set_host is a blocking getaddrinfo. For a numeric address that is pure
     * overhead; for a malformed one it stalls for the length of a DNS timeout, which presents as
     * the game hanging on a black screen rather than as a connection error. Parse numerically
     * first (inet_pton, no network involved) and only fall back to name resolution when the
     * address really is a hostname.
     *
     * This is the only host/client asymmetry in session setup, which is why a broken address
     * looks like "the host is fine but the client is dead". */
    ENetAddress addr{};
    if (enet_address_set_host_ip(&addr, address.c_str()) != 0) {
        std::printf("[banjocoop] '%s' is not a numeric address; resolving by name (this blocks)\n",
                    address.c_str());
        std::fflush(stdout);
        if (enet_address_set_host(&addr, address.c_str()) != 0) {
            enet_host_destroy(impl_->host);
            impl_->host = nullptr;
            enet_release();
            error = "could not resolve '" + address + "'";
            return false;
        }
    }
    addr.port = port;

    impl_->server_peer = enet_host_connect(impl_->host, &addr, BCNET_CHANNEL_COUNT, 0);
    if (impl_->server_peer == nullptr) {
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
        enet_release();
        error = "no available peers for connection";
        return false;
    }

    identity_ = id;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        peers_ = {};
    }

    is_host_.store(false);
    status_.store(Status::Connecting);
    running_.store(true);
    thread_ = std::thread(&Transport::run, this);
    return true;
}

void Transport::stop_thread() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void Transport::shutdown() {
    bool had_host = impl_->host != nullptr;
    stop_thread();

    if (impl_->host != nullptr) {
        /* Give peers a moment to see the disconnect so they show "left" rather than "timed out". */
        if (impl_->server_peer != nullptr) {
            enet_peer_disconnect(impl_->server_peer, 0);
            ENetEvent ev;
            while (enet_host_service(impl_->host, &ev, 100) > 0) {
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    break;
                }
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
        enet_host_destroy(impl_->host);
        impl_->host = nullptr;
        impl_->server_peer = nullptr;
        impl_->peer_ids.clear();
    }

    if (had_host) {
        enet_release();
    }

    status_.store(Status::Offline);
    local_player_id_.store(0);
    ping_ms_.store(0);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        peers_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(sim_mutex_);
        sim_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(outbox_mutex_);
        outbox_.clear();
    }
    {
        /* The registry is per-session. Keeping it across a disconnect would refuse notes that the
         * next session's players have not collected yet. */
        std::lock_guard<std::mutex> lock(registry_mutex_);
        collected_notes_.clear();
    }
    inbox_dropped_.store(0);
}

} // namespace bcnet
