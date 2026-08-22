/* ENet-over-UDP backend for Link. The original transport spoke ENet directly; that code now lives
 * here behind the Link interface, unchanged in behaviour, so the WebSocket backend can share the
 * routing logic above it. See link.hpp for the contract.
 *
 * A peer handle is the peer's index into the ENetHost's peer array (peer - host->peers), which is
 * stable for as long as that peer is connected — the same value the old code computed inline.
 */

#ifndef BANJOCOOP_ENET_LINK_HPP
#define BANJOCOOP_ENET_LINK_HPP

#include <enet/enet.h>

#include "banjocoop/link.hpp"

namespace bcnet {

class EnetLink : public Link {
public:
    EnetLink() = default;
    ~EnetLink() override { close(); }

    EnetLink(const EnetLink&) = delete;
    EnetLink& operator=(const EnetLink&) = delete;

    bool listen(uint16_t port, uint32_t max_peers, std::string& error) override;
    bool connect(const std::string& target, std::string& error) override;
    void service(int timeout_ms, std::vector<LinkEvent>& out) override;
    void send(uint32_t peer, uint8_t channel, bool reliable,
              const uint8_t* data, size_t len) override;
    void flush() override;
    void disconnect(uint32_t peer) override;
    uint32_t ping_ms() const override { return ping_ms_; }
    void close() override;

private:
    ENetHost* host_ = nullptr;
    ENetPeer* server_peer_ = nullptr; /* client side only */
    bool acquired_ = false;           /* whether this link holds a global enet init reference */
    uint32_t ping_ms_ = 0;
};

} // namespace bcnet

#endif
