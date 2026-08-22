/* Session lifecycle: standing a host or client up, and tearing it down again.
 *
 * Kept apart from the running loop in transport.cpp because these are the only functions that
 * create and destroy the Link backend, and because everything here runs on the *game* thread
 * while the loop runs on the net thread. This is also the one place that names a concrete backend
 * (EnetLink today); the running loop speaks only to the Link interface.
 */

#include "transport_internal.hpp"

#include "banjocoop/enet_link.hpp"
#include "banjocoop/ws_link.hpp"

namespace bcnet {

Transport::Transport() : impl_(new Impl()) {}

Transport::~Transport() {
    shutdown();
    delete impl_;
}

/* The only difference between the two host paths (and the two join paths) is which Link they stand
 * up; everything after the transport is bound is identical, so it lives here once. `link` is
 * already listen()'d / connect()'d by the caller. */
bool Transport::start_host(std::unique_ptr<Link> link, const Identity& id) {
    impl_->link = std::move(link);
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

bool Transport::start_join(std::unique_ptr<Link> link, const Identity& id) {
    impl_->link = std::move(link);
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

bool Transport::host(uint16_t port, const Identity& id, std::string& error) {
    shutdown();
    auto link = std::make_unique<EnetLink>();
    if (!link->listen(port, BCNET_MAX_PLAYERS, error)) {
        return false;
    }
    return start_host(std::move(link), id);
}

bool Transport::join(const std::string& address, uint16_t port, const Identity& id, std::string& error) {
    shutdown();
    /* connect() runs on the GAME thread, called straight out of the frame hook, so a blocking name
     * resolution there would freeze the game — the backend handles that (see EnetLink::connect).
     * The address is passed as "address:port"; the backend parses it. */
    auto link = std::make_unique<EnetLink>();
    if (!link->connect(address + ":" + std::to_string(port), error)) {
        return false;
    }
    return start_join(std::move(link), id);
}

/* WebSocket host: binds a local ws:// server that a Cloudflare tunnel (cloudflared) points at, so
 * clients reach it over wss:// without any port forwarding. */
bool Transport::host_ws(uint16_t port, const Identity& id, std::string& error) {
    shutdown();
    auto link = std::make_unique<WsLink>();
    if (!link->listen(port, BCNET_MAX_PLAYERS, error)) {
        return false;
    }
    return start_host(std::move(link), id);
}

/* WebSocket join: `url` is a full ws:// or wss:// address, produced by expanding a join code. */
bool Transport::join_ws(const std::string& url, const Identity& id, std::string& error) {
    shutdown();
    auto link = std::make_unique<WsLink>();
    if (!link->connect(url, error)) {
        return false;
    }
    return start_join(std::move(link), id);
}

void Transport::stop_thread() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void Transport::shutdown() {
    stop_thread();

    /* Tearing the link down says goodbye to peers (client) and releases the socket and the global
     * enet reference. Done after the net thread has joined, so nothing else is touching it. */
    if (impl_->link) {
        impl_->link->close();
        impl_->link.reset();
    }
    impl_->peer_ids.clear();

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
