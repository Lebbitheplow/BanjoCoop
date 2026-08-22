/* WebSocket backend for Link. This is the path that rides a Cloudflare tunnel: a host has no port
 * to forward and a joiner needs no separate app, because the connection is an ordinary wss:// the
 * tunnel can carry (which UDP/ENet cannot).
 *
 * IXWebSocket is callback- and thread-driven, while Link is polled (service()); WsLink bridges the
 * two by pushing callback events onto an internal queue that service() drains. See link.hpp.
 *
 * Channel and reliability hints are ignored: a WebSocket is a single reliable-ordered stream, so
 * the unreliable state channel collapses into reliable-ordered here. That is the accepted trade of
 * the tunnelled path — worse under loss than ENet, but able to traverse where ENet cannot.
 */

#ifndef BANJOCOOP_WS_LINK_HPP
#define BANJOCOOP_WS_LINK_HPP

#include <memory>

#include "banjocoop/link.hpp"

namespace bcnet {

class WsLink : public Link {
public:
    WsLink();
    ~WsLink() override;

    WsLink(const WsLink&) = delete;
    WsLink& operator=(const WsLink&) = delete;

    bool listen(uint16_t port, uint32_t max_peers, std::string& error) override;
    bool connect(const std::string& target, std::string& error) override;
    void service(int timeout_ms, std::vector<LinkEvent>& out) override;
    void send(uint32_t peer, uint8_t channel, bool reliable,
              const uint8_t* data, size_t len) override;
    void flush() override;
    void disconnect(uint32_t peer) override;
    uint32_t ping_ms() const override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bcnet

#endif
