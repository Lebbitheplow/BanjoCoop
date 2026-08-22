/* A byte-transport backend, sitting beneath the transport's handshake/relay/adjudication logic.
 *
 * That logic — service_host/service_client, the reliable event channel, note adjudication — speaks
 * only in framed messages (Writer/Reader) and opaque per-connection handles. It never mentions a
 * concrete transport, so the same code drives two very different ones:
 *
 *   - EnetLink: ENet over UDP. Direct/LAN/VPN play; the fast, already-tested path.
 *   - WsLink:   WebSocket. Rides a Cloudflare tunnel the way UDP cannot, so a host needs no port
 *               forwarding and a joiner needs no separate app — just a code that resolves to a
 *               wss:// URL.
 *
 * The split is what lets a single handshake/relay implementation serve both; without it the WS
 * path would be a second copy of ~500 lines of routing logic to keep in step.
 */

#ifndef BANJOCOOP_LINK_HPP
#define BANJOCOOP_LINK_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bcnet {

/* Send to every connected peer. Deliberately the same value as netsim.hpp's kBroadcast, so a
 * simulated packet's target can be handed to a Link untranslated. */
constexpr uint32_t kLinkBroadcast = 0xFFFFFFFFu;

struct LinkEvent {
    enum class Type { Connect, Disconnect, Receive };
    Type type;
    uint32_t peer = 0;         /* opaque per-connection handle, stable while connected */
    std::vector<uint8_t> data; /* payload for Receive; empty otherwise */
};

class Link {
public:
    virtual ~Link() = default;

    /* Host: bind and accept up to max_peers. Client: open one connection to `target`, whose form
     * is backend-specific ("1.2.3.4:34567" for enet, a "wss://..." URL for ws). Returns false and
     * sets `error` on failure. */
    virtual bool listen(uint16_t port, uint32_t max_peers, std::string& error) = 0;
    virtual bool connect(const std::string& target, std::string& error) = 0;

    /* Pump the backend for up to timeout_ms and append everything that happened to `out`. This is
     * where the net loop's pacing comes from, so it may block up to timeout_ms when idle. */
    virtual void service(int timeout_ms, std::vector<LinkEvent>& out) = 0;

    /* Send to one peer, or to all when peer == kLinkBroadcast. A handle that is no longer connected
     * is silently ignored, so callers never track peer liveness themselves. channel/reliable are
     * honoured by backends that have channels (enet) and ignored by those that do not (ws is always
     * reliable-ordered). */
    virtual void send(uint32_t peer, uint8_t channel, bool reliable,
                      const uint8_t* data, size_t len) = 0;

    /* Push anything queued out now. */
    virtual void flush() = 0;

    /* Host: drop one peer after refusing its handshake. */
    virtual void disconnect(uint32_t peer) = 0;

    /* Client round-trip time to the host in ms; 0 when hosting or not yet known. */
    virtual uint32_t ping_ms() const = 0;

    /* Tear the backend down. Safe to call more than once. */
    virtual void close() = 0;
};

} // namespace bcnet

#endif
