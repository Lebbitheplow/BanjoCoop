/* Drives a bundled `cloudflared` quick tunnel: the piece that lets a host expose its local ws://
 * server to the internet with no port forwarding and no Cloudflare account.
 *
 * `cloudflared tunnel --url http://localhost:<port>` prints a generated
 * https://<random>.trycloudflare.com URL to its output; this class launches it, watches that
 * output, and hands back the hostname, which becomes the join code a client expands to a wss:// URL.
 *
 * Everything is asynchronous: cloudflared takes a few seconds to report its URL, so join_host()
 * returns empty until it is ready and the mod polls it each frame — nothing here blocks the game.
 */

#ifndef BANJOCOOP_TUNNEL_HPP
#define BANJOCOOP_TUNNEL_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace bcnet {

class TunnelProcess {
public:
    TunnelProcess();
    ~TunnelProcess();

    TunnelProcess(const TunnelProcess&) = delete;
    TunnelProcess& operator=(const TunnelProcess&) = delete;

    /* Spawn `cloudflared` pointed at http://localhost:<local_port>. Returns false and sets `error`
     * only if the process could not be launched at all (e.g. the binary is missing); the URL
     * arriving is reported later through join_host(). */
    bool start(uint16_t local_port, std::string& error);

    /* The tunnel hostname (e.g. "brave-tiger.trycloudflare.com"), or empty until cloudflared has
     * reported it. This is the join code. */
    std::string join_host() const;

    /* True while the child process is live. */
    bool running() const;

    /* Terminate the child and stop watching it. Safe to call when not started. */
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bcnet

#endif
