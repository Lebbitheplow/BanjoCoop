cmake_minimum_required(VERSION 3.16)
project(banjocoop_net CXX)

set(CMAKE_CXX_STANDARD 20)  # librecomp/helpers.hpp uses `requires` clauses
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor")

# ENet ships its own CMake project. Vendored rather than system-installed so the .so we ship
# alongside the mod does not depend on whatever libenet the user happens to have.
add_subdirectory("${VENDOR_DIR}/enet" enet EXCLUDE_FROM_ALL)

# Transport core, deliberately free of any game or recomp dependency so it can be unit tested
# headless with no ROM and no running game.
add_library(bcnet_core STATIC
    src/transport.cpp
    src/transport_events.cpp
    src/transport_session.cpp
)
target_include_directories(bcnet_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${VENDOR_DIR}/enet/include"
)
target_link_libraries(bcnet_core PUBLIC enet)
target_compile_options(bcnet_core PRIVATE -Wall -Wextra)

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
target_compile_options(banjocoop_net PRIVATE -Wall -Wextra)

# Headless transport test: two Transports in one process, no game involved.
add_executable(bcnet_test tests/test_transport.cpp)
target_link_libraries(bcnet_test PRIVATE bcnet_core)
target_compile_options(bcnet_test PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME transport COMMAND bcnet_test)
