cmake_minimum_required(VERSION 3.16)
project(banjocoop_net CXX)

set(CMAKE_CXX_STANDARD 20)  # librecomp/helpers.hpp uses `requires` clauses
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor")

# Warning flags, per compiler. MSVC rejects -Wall/-Wextra outright ("invalid numeric argument
# '/Wextra'") and never gets as far as compiling anything, so these cannot be spelled once.
# /W4 is MSVC's rough equivalent of -Wall -Wextra.
if(MSVC)
    set(BCNET_WARNINGS /W4)
else()
    set(BCNET_WARNINGS -Wall -Wextra)
endif()

# ENet ships its own CMake project. Vendored rather than system-installed so the .so we ship
# alongside the mod does not depend on whatever libenet the user happens to have.
add_subdirectory("${VENDOR_DIR}/enet" enet EXCLUDE_FROM_ALL)

# mbedTLS, vendored and built from source, so the WebSocket client can speak wss:// to Cloudflare
# without depending on a system OpenSSL — the same "ship our own" reasoning as ENet. Programs,
# tests and fatal warnings off: we only want the three libraries (mbedtls/mbedx509/mbedcrypto).
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
# Use mbedTLS's committed pre-generated PSA driver wrappers rather than regenerating them, so the
# build needs no Python/jinja2 (GEN_FILES already defaults OFF; set explicitly so a future default
# flip cannot silently add a build-time Python dependency in CI).
set(GEN_FILES OFF CACHE BOOL "" FORCE)
add_subdirectory("${VENDOR_DIR}/mbedtls" mbedtls EXCLUDE_FROM_ALL)

# IXWebSocket, for the WebSocket backend that rides a Cloudflare tunnel. Vendored for the same
# reason as ENet. TLS on, using the vendored mbedTLS above — we pre-set the variables IXWebSocket
# would otherwise discover via find_package(MbedTLS), so it uses ours. zlib/deflate is off: the
# wire is small binary frames, not text worth compressing, and it trims the dependency surface.
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
set(MBEDTLS_FOUND TRUE)
set(MBEDTLS_INCLUDE_DIRS "${VENDOR_DIR}/mbedtls/include")
set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
add_subdirectory("${VENDOR_DIR}/IXWebSocket" ixwebsocket EXCLUDE_FROM_ALL)
# IXWebSocket only adds this define on the find_package path, which we bypassed; mbedTLS 3.x needs
# it for IXSocketMbedTLS.cpp to compile against the v3 API.
target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)

# Transport core, deliberately free of any game or recomp dependency so it can be unit tested
# headless with no ROM and no running game.
# Embed the CA bundle: read the .pem and bake it into a generated source as an in-memory PEM
# string. Regenerated whenever the .pem changes (its content drives the configure_file output).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/certs/cacert.pem" BCNET_CA_BUNDLE)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/certs/ca_bundle.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.cpp"
    @ONLY)

add_library(bcnet_core STATIC
    src/transport.cpp
    src/transport_events.cpp
    src/transport_session.cpp
    src/enet_link.cpp
    src/ws_link.cpp
    src/tunnel.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/ca_bundle.cpp"
)
target_include_directories(bcnet_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${VENDOR_DIR}/enet/include"
)
target_link_libraries(bcnet_core PUBLIC enet ixwebsocket ${CMAKE_DL_LIBS})
target_compile_options(bcnet_core PRIVATE ${BCNET_WARNINGS})

find_package(Threads REQUIRED)
target_link_libraries(bcnet_core PUBLIC Threads::Threads)

# The shared library the game loads. Named to match `native_libraries` in config/mod.toml; the
# runtime appends the platform extension and looks for it NEXT TO the .nrm, not inside it.
add_library(banjocoop_net SHARED src/mod_abi.cpp)
target_link_libraries(banjocoop_net PRIVATE bcnet_core)
target_include_directories(banjocoop_net PRIVATE
    "${VENDOR_DIR}/N64Recomp/include"
    "${VENDOR_DIR}/N64ModernRuntime/librecomp/include"
    "${VENDOR_DIR}/N64ModernRuntime/ultramodern/include"
)
# The runtime resolves exports via dlsym by exact name, so nothing may be hidden or renamed.
set_target_properties(banjocoop_net PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET default)
target_compile_options(banjocoop_net PRIVATE ${BCNET_WARNINGS})

# Headless transport test: two Transports in one process, no game involved.
add_executable(bcnet_test tests/test_transport.cpp)
target_link_libraries(bcnet_test PRIVATE bcnet_core)
target_compile_options(bcnet_test PRIVATE ${BCNET_WARNINGS})

enable_testing()
add_test(NAME transport COMMAND bcnet_test)
