/* The MIPS <-> native boundary.
 *
 * Every function here is a recomp export: the runtime resolves it by name from the .so listed in
 * the mod's `native_libraries` manifest entry, and calls it with the signature
 *
 *     void f(uint8_t* rdram, recomp_context* ctx)
 *
 * Arguments arrive in ctx registers and are unpacked with _arg<N, T>; results go back through
 * _return<T>. Pointers arriving from the mod are MIPS addresses and must be translated with
 * TO_PTR before dereferencing.
 *
 * Design rule from the plan: the boundary is crossed once per frame, not once per object. All
 * per-frame traffic goes through bcnet_pump, which exchanges two staging structs. Everything
 * expensive (sockets, threading, serialisation) happens on the transport's own thread.
 */

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "librecomp/helpers.hpp"
#include "recomp.h"

#include "banjocoop/protocol.h"
#include "banjocoop/transport.hpp"

using namespace bcnet;

namespace {

/* The runtime refuses to load a native library that does not declare a supported ABI version.
 * Without this the library loads but every export silently fails to resolve. */
std::unique_ptr<Transport> g_transport;

/* Captured by bcnet_init and reused for every subsequent host()/join(), so the mod does not have
 * to re-marshal the ROM hash and name across the boundary each time. */
Identity g_identity;

Transport& transport() {
    if (!g_transport) {
        g_transport = std::make_unique<Transport>();
    }
    return *g_transport;
}

/* Read a fixed-length, NUL-terminated string out of rdram.
 *
 * Cannot use TO_PTR + strlen directly: char is a sub-word type, so rdram's word-swapped layout
 * means byte N of a string lives at index N^3 (see protocol.h). MEM_BU applies that XOR.
 */
std::string read_mips_string(uint8_t* rdram, PTR(char) addr, size_t max_len = 128) {
    std::string out;
    for (size_t i = 0; i < max_len; i++) {
        char c = static_cast<char>(MEM_BU(i, static_cast<gpr>(addr)));
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

extern "C" {

/* Checked by the runtime before any export is resolved. Must be 1. */
uint32_t recomp_api_version = 1;

/* bcnet_init(u32 mod_version, PTR(char) player_name, PTR(u8) rom_hash) -> u32 ok
 *
 * Identity is supplied by the mod rather than discovered here so that the native library stays
 * free of game knowledge and remains unit-testable headless.
 */
void bcnet_init(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mod_version = _arg<0, uint32_t>(rdram, ctx);
    PTR(char) name_ptr = _arg<1, PTR(char)>(rdram, ctx);
    PTR(char) hash_ptr = _arg<2, PTR(char)>(rdram, ctx);

    /* Make the log usable.
     *
     * stdout through a pipe is block-buffered at 4 KB, so a handful of diagnostic lines sit in the
     * buffer indefinitely while the game runs — and are lost entirely if it crashes. That made
     * every sync problem undiagnosable from a log, which is why so much of this was guessed at
     * rather than measured. Line buffering costs nothing at these volumes. */
    static bool buffering_fixed = false;
    if (!buffering_fixed) {
        buffering_fixed = true;
        setvbuf(stdout, nullptr, _IOLBF, 0);
    }

    Identity id;
    id.mod_version = mod_version;
    id.name = read_mips_string(rdram, name_ptr, BCNET_NAME_LEN);
    for (size_t i = 0; i < ROM_HASH_LEN; i++) {
        id.rom_hash[i] = MEM_BU(i, static_cast<gpr>(hash_ptr));
    }

    /* Echo the decoded name. Strings crossing this boundary are the one thing rdram's word-swapped
     * layout can silently mangle (see read_mips_string), and a mangled name here means a mangled
     * address in bcnet_join too — which is far easier to spot as garbage in the log than as a
     * connection that never happens. */
    std::printf("[banjocoop] init: mod v%u, name '%s'\n", mod_version, id.name.c_str());
    std::fflush(stdout);

    /* Stash identity for the next host()/join() call. */
    g_identity = id;
    _return<uint32_t>(ctx, 1);
}

/* bcnet_host(u32 port) -> u32 ok */
void bcnet_host(uint8_t* rdram, recomp_context* ctx) {
    uint32_t port = _arg<0, uint32_t>(rdram, ctx);
    std::string err;
    bool ok = transport().host(static_cast<uint16_t>(port), g_identity, err);
    if (!ok) {
        std::printf("[banjocoop] host failed: %s\n", err.c_str());
    }
    std::fflush(stdout);
    _return<uint32_t>(ctx, ok ? 1u : 0u);
}

/* bcnet_join(PTR(char) address, u32 port) -> u32 ok */
void bcnet_join(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) addr_ptr = _arg<0, PTR(char)>(rdram, ctx);
    uint32_t port = _arg<1, uint32_t>(rdram, ctx);

    std::string address = read_mips_string(rdram, addr_ptr);

    /* Logged before the call, and flushed, because join() runs on the game thread: if anything in
     * there blocks, this is the last thing that gets out, and it names the culprit. */
    std::printf("[banjocoop] join: connecting to '%s':%u\n", address.c_str(), port);
    std::fflush(stdout);

    std::string err;
    bool ok = transport().join(address, static_cast<uint16_t>(port), g_identity, err);
    if (!ok) {
        std::printf("[banjocoop] join failed: %s\n", err.c_str());
    } else {
        std::printf("[banjocoop] join: socket ready, handshaking\n");
    }
    std::fflush(stdout);
    _return<uint32_t>(ctx, ok ? 1u : 0u);
}

/* bcnet_shutdown() */
void bcnet_shutdown(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    if (g_transport) {
        g_transport->shutdown();
    }
}

/* bcnet_pump(PTR(bc_player_state) local, PTR(bc_outgoing) outgoing, PTR(bc_incoming) incoming)
 *
 * The single per-frame crossing. Reads the local player's state and the world events the mod's
 * hooks queued during the frame, and fills the incoming buffer with every remote player plus the
 * world events that arrived for us.
 *
 * All three structs are all-4-byte-field, so they can be accessed through TO_PTR as ordinary
 * native structs: rdram stores 32-bit words natively and nothing here straddles a word boundary.
 */
void bcnet_pump(uint8_t* rdram, recomp_context* ctx) {
    PTR(void) local_ptr = _arg<0, PTR(void)>(rdram, ctx);
    PTR(void) outgoing_ptr = _arg<1, PTR(void)>(rdram, ctx);
    PTR(void) incoming_ptr = _arg<2, PTR(void)>(rdram, ctx);

    if (!g_transport) {
        return;
    }

    if (local_ptr != 0) {
        const bc_player_state* local = TO_PTR(const bc_player_state, local_ptr);
        g_transport->set_local_state(*local);
    }
    if (outgoing_ptr != 0) {
        bc_outgoing* outgoing = TO_PTR(bc_outgoing, outgoing_ptr);
        g_transport->submit_events(outgoing->queue);
        /* Consumed — reset here rather than in the mod so the queue cannot be sent twice if the
         * mod's frame hook is ever re-entered. */
        outgoing->queue.count = 0;

        /* Objects are a snapshot rather than a queue, so this is NOT cleared: the mod rewrites it
         * wholesale each frame, and the transport keeps only the newest. */
        g_transport->submit_objects(outgoing->objects);
        g_transport->submit_progress(outgoing->progress);
        if (outgoing->outgoing_chat.length != 0) {
            g_transport->submit_chat(outgoing->outgoing_chat);
            outgoing->outgoing_chat.length = 0;
        }
    }
    if (incoming_ptr != 0) {
        bc_incoming* incoming = TO_PTR(bc_incoming, incoming_ptr);
        g_transport->snapshot(*incoming);
    }
}

/* bcnet_set_save_path(PTR(char) path)
 *
 * Only the game knows where its save lives (recomp_get_save_file_path), and only the native side
 * can read or write a file, so the path has to cross the boundary. */
void bcnet_set_save_path(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) path_ptr = _arg<0, PTR(char)>(rdram, ctx);
    std::string path = read_mips_string(rdram, path_ptr, 512);
    transport().set_save_path(path);
    std::printf("[banjocoop] save file: %s\n", path.c_str());
    std::fflush(stdout);
}

/* bcnet_take_host_save(PTR(char) out, u32 max) -> u32 length
 *
 * Non-zero once the host's save has been written locally; the mod then hands the name to
 * recomp_change_save_file. Returned as a string because that is what the runtime wants, and
 * written a byte at a time because rdram's layout makes anything else wrong (see protocol.h). */
void bcnet_take_host_save(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) out_ptr = _arg<0, PTR(char)>(rdram, ctx);
    uint32_t max = _arg<1, uint32_t>(rdram, ctx);

    std::string name = g_transport ? g_transport->take_pending_save_name() : std::string{};
    if (name.empty() || out_ptr == 0 || max == 0) {
        _return<uint32_t>(ctx, 0);
        return;
    }
    uint32_t n = static_cast<uint32_t>(name.size());
    if (n >= max) {
        n = max - 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        MEM_B(i, static_cast<gpr>(out_ptr)) = name[i];
    }
    MEM_B(n, static_cast<gpr>(out_ptr)) = '\0';
    _return<uint32_t>(ctx, n);
}

/* bcnet_player_name(u32 player_id, PTR(char) out, u32 max) -> u32 length
 *
 * Written a byte at a time through MEM_B, which applies the ^3 swizzle rdram needs. A plain
 * struct copy would produce scrambled text (docs/symbols.md §11), which is why names are fetched
 * here rather than carried in bc_incoming with everything else. */
void bcnet_player_name(uint8_t* rdram, recomp_context* ctx) {
    uint32_t player_id = _arg<0, uint32_t>(rdram, ctx);
    PTR(char) out_ptr = _arg<1, PTR(char)>(rdram, ctx);
    uint32_t max = _arg<2, uint32_t>(rdram, ctx);

    std::string name = g_transport ? g_transport->player_name(player_id) : std::string{};
    if (out_ptr == 0 || max == 0) {
        _return<uint32_t>(ctx, 0);
        return;
    }
    uint32_t n = static_cast<uint32_t>(name.size());
    if (n >= max) {
        n = max - 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        MEM_B(i, static_cast<gpr>(out_ptr)) = name[i];
    }
    MEM_B(n, static_cast<gpr>(out_ptr)) = '\0';
    _return<uint32_t>(ctx, n);
}

/* bcnet_status() -> u32 (bcnet::Status) */
void bcnet_status(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    uint32_t st = g_transport ? static_cast<uint32_t>(g_transport->status())
                              : static_cast<uint32_t>(Status::Offline);
    _return<uint32_t>(ctx, st);
}

/* bcnet_reject_reason() -> u32 (BCNET_REJECT_*) */
void bcnet_reject_reason(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    _return<uint32_t>(ctx, g_transport ? g_transport->reject_reason() : 0u);
}

/* bcnet_set_sim(u32 latency_ms, u32 jitter_ms, u32 loss_permille)
 *
 * Loss is per-mille rather than a float because passing floats across this boundary is fiddlier
 * than it is worth for a debug control.
 */
void bcnet_set_sim(uint8_t* rdram, recomp_context* ctx) {
    NetSimConfig cfg;
    cfg.latency_ms = _arg<0, uint32_t>(rdram, ctx);
    cfg.jitter_ms = _arg<1, uint32_t>(rdram, ctx);
    cfg.loss = static_cast<float>(_arg<2, uint32_t>(rdram, ctx)) / 1000.0f;
    if (g_transport) {
        g_transport->configure_sim(cfg);
    }
}

} // extern "C"
