/* Wire byte-order conversion.
 *
 * Scope note, because this is the easy thing to get backwards: these helpers are for the WIRE
 * only. Values living in rdram are host-native 32-bit words (see the header comment in
 * protocol.h) and must NOT be passed through here — the native library reads and writes those
 * as ordinary uint32_t/float.
 *
 * The wire is big-endian so the protocol does not quietly depend on every peer being
 * little-endian.
 */

#ifndef BANJOCOOP_BYTEORDER_HPP
#define BANJOCOOP_BYTEORDER_HPP

#include <cstdint>
#include <cstring>

namespace bcnet {

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr bool host_is_big_endian = true;
#else
inline constexpr bool host_is_big_endian = false;
#endif

/* Written out by hand rather than with __builtin_bswap32, which is a GCC/Clang extension MSVC
 * does not have — it has _byteswap_ulong instead, behind <intrin.h>. Branching on the compiler
 * would mean a third branch the next time somebody builds this somewhere new.
 *
 * This costs nothing: every compiler recognises the pattern and emits a single bswap (x86) or rev
 * (ARM). It is also constexpr, which no intrinsic here is. */
inline constexpr uint32_t byteswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

inline constexpr uint32_t swap_wire_u32(uint32_t v) {
    return host_is_big_endian ? v : byteswap32(v);
}

/* host -> wire and wire -> host are the same operation; named separately for readability at
 * call sites, where getting the direction wrong is otherwise invisible. */
inline uint32_t hton_u32(uint32_t v) { return swap_wire_u32(v); }
inline uint32_t ntoh_u32(uint32_t v) { return swap_wire_u32(v); }

/* Floats are IEEE-754 on both ends, so only byte order differs. memcpy rather than a union or a
 * pointer cast keeps this strictly well-defined. */
inline uint32_t float_bits(float f) {
    uint32_t raw;
    std::memcpy(&raw, &f, sizeof(raw));
    return raw;
}

inline float bits_float(uint32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof(f));
    return f;
}

inline uint32_t hton_f32(float f) { return swap_wire_u32(float_bits(f)); }
inline float ntoh_f32(uint32_t raw) { return bits_float(swap_wire_u32(raw)); }

} // namespace bcnet

#endif
