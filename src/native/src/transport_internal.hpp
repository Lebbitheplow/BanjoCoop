/* Shared between transport.cpp (connection, handshake, player state) and transport_events.cpp
 * (the reliable world-event channel). Not a public header — nothing outside src/ includes it.
 *
 * The wire is big-endian. Structs in rdram are host-native (see protocol.h), so everything is
 * converted field by field rather than memcpy'd — a raw copy would work only for as long as every
 * peer happened to share an endianness.
 */

#ifndef BANJOCOOP_TRANSPORT_INTERNAL_HPP
#define BANJOCOOP_TRANSPORT_INTERNAL_HPP

#include <enet/enet.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "banjocoop/byteorder.hpp"
#include "banjocoop/transport.hpp"

namespace bcnet {

struct Writer {
    std::vector<uint8_t> buf;

    explicit Writer(uint8_t type) { buf.push_back(type); }

    void u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>(v >> 24));
        buf.push_back(static_cast<uint8_t>(v >> 16));
        buf.push_back(static_cast<uint8_t>(v >> 8));
        buf.push_back(static_cast<uint8_t>(v));
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void name(const std::string& s) {
        char fixed[BCNET_NAME_LEN] = {};
        std::strncpy(fixed, s.c_str(), BCNET_NAME_LEN - 1);
        bytes(fixed, BCNET_NAME_LEN);
    }
};

struct Reader {
    const uint8_t* p;
    size_t remaining;
    bool ok = true;

    Reader(const uint8_t* data, size_t len) : p(data), remaining(len) {}

    uint8_t type() {
        if (remaining < 1) {
            ok = false;
            return 0;
        }
        remaining--;
        return *p++;
    }
    uint32_t u32() {
        if (remaining < 4) {
            ok = false;
            return 0;
        }
        uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        p += 4;
        remaining -= 4;
        return v;
    }
    bool bytes(void* out, size_t n) {
        if (remaining < n) {
            ok = false;
            return false;
        }
        std::memcpy(out, p, n);
        p += n;
        remaining -= n;
        return true;
    }
    std::string name() {
        char fixed[BCNET_NAME_LEN] = {};
        if (!bytes(fixed, BCNET_NAME_LEN)) {
            return {};
        }
        fixed[BCNET_NAME_LEN - 1] = '\0';
        return std::string(fixed);
    }
};

void write_state(Writer& w, const bc_player_state& s);
bool read_state(Reader& r, bc_player_state& s);
void write_event(Writer& w, const bc_event& e);
bool read_event(Reader& r, bc_event& e);
void write_object(Writer& w, const bc_object_state& o);
bool read_object(Reader& r, bc_object_state& o);

/* Registry key for a static note. Notes are identified by (map, note_index) — note_index comes
 * from BanjoRecomp's own note bookkeeping, which counts notes in fixed static-level-data order at
 * map load and is therefore identical on every machine. */
inline uint64_t note_key(uint32_t map_id, uint32_t note_index) {
    return (static_cast<uint64_t>(map_id) << 32) | note_index;
}

struct Transport::Impl {
    ENetHost* host = nullptr;
    ENetPeer* server_peer = nullptr; /* client side only */
    /* Host side: peer -> player id. Player ids are stable for a session; the host is always 0. */
    std::unordered_map<ENetPeer*, uint32_t> peer_ids;
    uint32_t next_player_id = 1;
};

} // namespace bcnet

#endif
