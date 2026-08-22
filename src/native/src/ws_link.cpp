/* WebSocket backend. Bridges IXWebSocket's threaded, callback-driven model onto Link's polled
 * service()/send() by funnelling every callback into one mutex-guarded event queue that service()
 * drains on the net thread. See ws_link.hpp for why this backend exists. */

#include "banjocoop/ws_link.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "banjocoop/ca_bundle.hpp"

namespace bcnet {

namespace {

/* IXWebSocket needs Winsock started on Windows; a no-op elsewhere. Once per process is enough. */
void ensure_net_system() {
    static bool once = ix::initNetSystem();
    (void)once;
}

std::string to_payload(const uint8_t* data, size_t len) {
    return std::string(reinterpret_cast<const char*>(data), len);
}

} // namespace

struct WsLink::Impl {
    bool is_host = false;

    /* Host side. */
    std::unique_ptr<ix::WebSocketServer> server;
    std::mutex peers_mu;
    uint32_t next_handle = 1; /* never kLinkBroadcast; small ints like enet's peer indices */
    std::unordered_map<uint32_t, std::shared_ptr<ix::WebSocket>> peers;

    /* Client side. A client has a single connection to the host; this handle names it. */
    std::unique_ptr<ix::WebSocket> client;
    static constexpr uint32_t kClientPeer = 1;

    /* Events waiting for service() to collect, filled from IXWebSocket's callback threads. */
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<LinkEvent> incoming;

    void push(LinkEvent&& ev) {
        {
            std::lock_guard<std::mutex> lock(q_mu);
            incoming.push_back(std::move(ev));
        }
        q_cv.notify_one();
    }
};

WsLink::WsLink() : impl_(new Impl()) {}

WsLink::~WsLink() {
    close();
}

bool WsLink::listen(uint16_t port, uint32_t max_peers, std::string& error) {
    close();
    ensure_net_system();
    impl_->is_host = true;

    /* Bind loopback only: the sole thing that connects to this server is cloudflared running on
     * the same machine, so there is no reason to expose it on the network. */
    impl_->server = std::make_unique<ix::WebSocketServer>(
        static_cast<int>(port), "127.0.0.1", ix::SocketServer::kDefaultTcpBacklog, max_peers);
    impl_->server->disablePerMessageDeflate();

    impl_->server->setOnConnectionCallback(
        [this](std::weak_ptr<ix::WebSocket> weak, std::shared_ptr<ix::ConnectionState>) {
            auto ws = weak.lock();
            if (!ws) {
                return;
            }
            uint32_t handle;
            {
                std::lock_guard<std::mutex> lock(impl_->peers_mu);
                handle = impl_->next_handle++;
                impl_->peers[handle] = ws;
            }
            impl_->push({LinkEvent::Type::Connect, handle, {}});

            ws->setOnMessageCallback([this, handle](const ix::WebSocketMessagePtr& msg) {
                switch (msg->type) {
                    case ix::WebSocketMessageType::Message: {
                        LinkEvent ev;
                        ev.type = LinkEvent::Type::Receive;
                        ev.peer = handle;
                        ev.data.assign(msg->str.begin(), msg->str.end());
                        impl_->push(std::move(ev));
                        break;
                    }
                    case ix::WebSocketMessageType::Close:
                    case ix::WebSocketMessageType::Error: {
                        {
                            std::lock_guard<std::mutex> lock(impl_->peers_mu);
                            impl_->peers.erase(handle);
                        }
                        impl_->push({LinkEvent::Type::Disconnect, handle, {}});
                        break;
                    }
                    default:
                        break;
                }
            });
        });

    if (!impl_->server->listenAndStart()) {
        error = "failed to start WebSocket server on port " + std::to_string(port);
        impl_->server.reset();
        return false;
    }
    return true;
}

bool WsLink::connect(const std::string& target, std::string& error) {
    close();
    ensure_net_system();
    impl_->is_host = false;

    if (target.rfind("ws://", 0) != 0 && target.rfind("wss://", 0) != 0) {
        error = "join code did not resolve to a ws:// or wss:// URL: '" + target + "'";
        return false;
    }

    impl_->client = std::make_unique<ix::WebSocket>();
    impl_->client->setUrl(target);
    impl_->client->disablePerMessageDeflate();

    /* For wss:// (the Cloudflare path) verify the server against our embedded CA bundle. mbedTLS
     * has no trust store of its own and this backend does not read the system one, so without this
     * a wss:// handshake fails outright. Passing the PEM text (not a path) makes IXWebSocket treat
     * it as an in-memory CA. Harmless on a plain ws:// connection, which never negotiates TLS. */
    ix::SocketTLSOptions tls;
    tls.caFile = ca_bundle_pem();
    impl_->client->setTLSOptions(tls);

    impl_->client->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                impl_->push({LinkEvent::Type::Connect, Impl::kClientPeer, {}});
                break;
            case ix::WebSocketMessageType::Message: {
                LinkEvent ev;
                ev.type = LinkEvent::Type::Receive;
                ev.peer = Impl::kClientPeer;
                ev.data.assign(msg->str.begin(), msg->str.end());
                impl_->push(std::move(ev));
                break;
            }
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error:
                impl_->push({LinkEvent::Type::Disconnect, Impl::kClientPeer, {}});
                break;
            default:
                break;
        }
    });

    /* Non-blocking: the connection is established on IXWebSocket's own thread and surfaces as an
     * Open (-> Connect) or Error (-> Disconnect) event, so this never stalls the game thread. */
    impl_->client->start();
    return true;
}

void WsLink::service(int timeout_ms, std::vector<LinkEvent>& out) {
    std::unique_lock<std::mutex> lock(impl_->q_mu);
    if (impl_->incoming.empty() && timeout_ms > 0) {
        impl_->q_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return !impl_->incoming.empty(); });
    }
    while (!impl_->incoming.empty()) {
        out.push_back(std::move(impl_->incoming.front()));
        impl_->incoming.pop_front();
    }
}

void WsLink::send(uint32_t peer, uint8_t /*channel*/, bool /*reliable*/,
                  const uint8_t* data, size_t len) {
    std::string payload = to_payload(data, len);

    if (!impl_->is_host) {
        /* One connection; both a broadcast and a send to the server peer go to it. */
        if (impl_->client) {
            impl_->client->sendBinary(payload);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->peers_mu);
    if (peer == kLinkBroadcast) {
        for (auto& [handle, ws] : impl_->peers) {
            ws->sendBinary(payload);
        }
        return;
    }
    auto it = impl_->peers.find(peer);
    if (it != impl_->peers.end()) {
        it->second->sendBinary(payload);
    }
}

void WsLink::flush() {
    /* IXWebSocket sends on its own thread as soon as data is queued; nothing to force here. */
}

void WsLink::disconnect(uint32_t peer) {
    if (!impl_->is_host || peer == kLinkBroadcast) {
        return;
    }
    std::shared_ptr<ix::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(impl_->peers_mu);
        auto it = impl_->peers.find(peer);
        if (it == impl_->peers.end()) {
            return;
        }
        ws = it->second;
    }
    ws->close();
}

uint32_t WsLink::ping_ms() const {
    /* IXWebSocket does not surface an RTT; the overlay shows 0 rather than a guess. */
    return 0;
}

void WsLink::close() {
    if (impl_->server) {
        impl_->server->stop();
        impl_->server.reset();
    }
    if (impl_->client) {
        impl_->client->stop();
        impl_->client.reset();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->peers_mu);
        impl_->peers.clear();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->q_mu);
        impl_->incoming.clear();
    }
    impl_->q_cv.notify_all();
}

} // namespace bcnet
