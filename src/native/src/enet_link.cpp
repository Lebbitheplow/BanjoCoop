/* ENet backend. This is the transport's original enet_* code, moved verbatim in behaviour behind
 * the Link interface: the global-init refcount, the numeric-first/name-fallback address parse, and
 * the graceful client disconnect on shutdown all live here now rather than in the transport. */

#include "banjocoop/enet_link.hpp"

#include "banjocoop/protocol.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bcnet {

namespace {

/* enet_initialize is global and refcounted, so several links (the tests run three at once) can
 * come and go without one shutting the library out from under another. */
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

bool EnetLink::listen(uint16_t port, uint32_t max_peers, std::string& error) {
    close();
    if (!enet_acquire(error)) {
        return false;
    }
    acquired_ = true;

    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    host_ = enet_host_create(&addr, max_peers, BCNET_CHANNEL_COUNT, 0, 0);
    if (host_ == nullptr) {
        enet_release();
        acquired_ = false;
        error = "failed to bind port " + std::to_string(port);
        return false;
    }
    return true;
}

bool EnetLink::connect(const std::string& target, std::string& error) {
    close();
    if (!enet_acquire(error)) {
        return false;
    }
    acquired_ = true;

    host_ = enet_host_create(nullptr, 1, BCNET_CHANNEL_COUNT, 0, 0);
    if (host_ == nullptr) {
        enet_release();
        acquired_ = false;
        error = "failed to create client host";
        return false;
    }

    /* target is "address:port". Split on the last colon so a hostname is handled the same as a
     * dotted quad; IPv6 literals would need brackets, which this transport never emits. */
    std::string::size_type colon = target.rfind(':');
    if (colon == std::string::npos) {
        enet_host_destroy(host_);
        host_ = nullptr;
        enet_release();
        acquired_ = false;
        error = "malformed address '" + target + "'";
        return false;
    }
    std::string address = target.substr(0, colon);
    uint16_t port = static_cast<uint16_t>(std::strtoul(target.c_str() + colon + 1, nullptr, 10));

    /* connect() runs on the GAME thread, called straight out of the frame hook — so anything that
     * blocks here freezes the game.
     *
     * enet_address_set_host is a blocking getaddrinfo. For a numeric address that is pure overhead;
     * for a malformed one it stalls for the length of a DNS timeout, which presents as the game
     * hanging on a black screen rather than as a connection error. Parse numerically first
     * (inet_pton, no network involved) and only fall back to name resolution when the address
     * really is a hostname. */
    ENetAddress addr{};
    if (enet_address_set_host_ip(&addr, address.c_str()) != 0) {
        std::printf("[banjocoop] '%s' is not a numeric address; resolving by name (this blocks)\n",
                    address.c_str());
        std::fflush(stdout);
        if (enet_address_set_host(&addr, address.c_str()) != 0) {
            enet_host_destroy(host_);
            host_ = nullptr;
            enet_release();
            acquired_ = false;
            error = "could not resolve '" + address + "'";
            return false;
        }
    }
    addr.port = port;

    server_peer_ = enet_host_connect(host_, &addr, BCNET_CHANNEL_COUNT, 0);
    if (server_peer_ == nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
        enet_release();
        acquired_ = false;
        error = "no available peers for connection";
        return false;
    }
    return true;
}

void EnetLink::service(int timeout_ms, std::vector<LinkEvent>& out) {
    if (host_ == nullptr) {
        return;
    }
    ENetEvent ev;
    /* Block only on the first poll: that is where the loop's pacing comes from. Once events are
     * flowing, subsequent polls return immediately so a burst drains in one pass. */
    int wait = timeout_ms;
    while (enet_host_service(host_, &ev, wait) > 0) {
        wait = 0;
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                out.push_back({LinkEvent::Type::Connect,
                               static_cast<uint32_t>(ev.peer - host_->peers), {}});
                break;
            case ENET_EVENT_TYPE_RECEIVE: {
                LinkEvent le;
                le.type = LinkEvent::Type::Receive;
                le.peer = static_cast<uint32_t>(ev.peer - host_->peers);
                le.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                out.push_back(std::move(le));
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                out.push_back({LinkEvent::Type::Disconnect,
                               static_cast<uint32_t>(ev.peer - host_->peers), {}});
                break;
            default:
                break;
        }
    }

    if (server_peer_ != nullptr) {
        ping_ms_ = server_peer_->roundTripTime;
    }
}

void EnetLink::send(uint32_t peer, uint8_t channel, bool reliable,
                    const uint8_t* data, size_t len) {
    if (host_ == nullptr) {
        return;
    }
    ENetPacket* pkt = enet_packet_create(
        data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
    if (pkt == nullptr) {
        return;
    }
    if (peer == kLinkBroadcast) {
        /* enet_host_broadcast destroys the packet itself if no peer references it. */
        enet_host_broadcast(host_, channel, pkt);
        return;
    }
    if (peer >= host_->peerCount) {
        enet_packet_destroy(pkt);
        return;
    }
    ENetPeer* p = &host_->peers[peer];
    if (p->state == ENET_PEER_STATE_CONNECTED) {
        enet_peer_send(p, channel, pkt);
    } else {
        enet_packet_destroy(pkt);
    }
}

void EnetLink::flush() {
    if (host_ != nullptr) {
        enet_host_flush(host_);
    }
}

void EnetLink::disconnect(uint32_t peer) {
    if (host_ == nullptr || peer == kLinkBroadcast || peer >= host_->peerCount) {
        return;
    }
    enet_peer_disconnect_later(&host_->peers[peer], 0);
}

void EnetLink::close() {
    if (host_ != nullptr) {
        /* Give the host a moment to see the disconnect so it shows "left" rather than "timed
         * out". Only the client has a server_peer to say goodbye to. */
        if (server_peer_ != nullptr) {
            enet_peer_disconnect(server_peer_, 0);
            ENetEvent ev;
            while (enet_host_service(host_, &ev, 100) > 0) {
                if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                    break;
                }
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                }
            }
        }
        enet_host_destroy(host_);
        host_ = nullptr;
        server_peer_ = nullptr;
    }
    if (acquired_) {
        enet_release();
        acquired_ = false;
    }
    ping_ms_ = 0;
}

} // namespace bcnet
