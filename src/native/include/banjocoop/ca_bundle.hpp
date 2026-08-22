/* The Mozilla CA root bundle, embedded in the library.
 *
 * mbedTLS ships no trust store, and IXWebSocket's mbedTLS backend does not read the system one on
 * Linux (it returns no certificates there), so a wss:// client with peer verification on would
 * have nothing to verify Cloudflare's certificate against and every tunnelled connection would
 * fail. Rather than ship a separate .pem next to the mod — one more file to lose — the bundle is
 * compiled in and handed to the client as an in-memory CA. Cross-platform and self-contained, the
 * same reasoning as vendoring ENet and mbedTLS.
 *
 * Source: https://curl.se/ca/cacert.pem (Mozilla's set, as extracted by the curl project). Refresh
 * src/native/certs/cacert.pem periodically so newly-trusted roots keep verifying.
 */

#ifndef BANJOCOOP_CA_BUNDLE_HPP
#define BANJOCOOP_CA_BUNDLE_HPP

namespace bcnet {

/* PEM text of the trusted roots, null-terminated. Contains the "-----BEGIN CERTIFICATE-----"
 * marker IXWebSocket uses to recognise an in-memory CA rather than a file path. */
const char* ca_bundle_pem();

} // namespace bcnet

#endif
