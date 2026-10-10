# Bitcoin Core's node as one shared library with a C ABI. EXCLUDE_FROM_ALL builds only what
# bitcoind_c needs from Core's tree: no wallet, GUI, ZMQ, IPC or executables.
cmake_minimum_required(VERSION 3.22)
project(bitcoind_c C CXX)

if(NOT DEFINED BITCOIN_SRC)
  message(FATAL_ERROR "BITCOIN_SRC must point at the patched Bitcoin Core tree")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Core's static libraries are linked into a shared object.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Global and before Core's targets exist: a per-target setting broke dynamic_cast in the
# Monero work, hidden typeinfo on one side of a cast and default on the other.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Source paths in __FILE__ would put the source trees in every consumer's runtime closure.
set(_logos_prefix_map "-ffile-prefix-map=${BITCOIN_SRC}=/bitcoin -ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=/shim")
foreach(_map IN LISTS LOGOS_EXTRA_PREFIX_MAPS)
  string(APPEND _logos_prefix_map " -ffile-prefix-map=${_map}")
endforeach()
string(APPEND CMAKE_C_FLAGS " ${_logos_prefix_map}")
string(APPEND CMAKE_CXX_FLAGS " ${_logos_prefix_map}")

foreach(_off BUILD_BITCOIN_BIN BUILD_CLI BUILD_TESTS BUILD_TX BUILD_UTIL BUILD_WALLET_TOOL
             BUILD_BENCH BUILD_FUZZ_BINARY BUILD_GUI BUILD_KERNEL_LIB BUILD_UTIL_CHAINSTATE
             ENABLE_WALLET ENABLE_IPC WITH_ZMQ WITH_USDT WITH_QRENCODE WITH_DBUS
             ENABLE_EXTERNAL_SIGNER WITH_CCACHE WITH_EMBEDDED_ASMAP INSTALL_MAN)
  set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
# BUILD_DAEMON is what makes Core look for libevent, which bitcoin_node links. The bitcoind
# executable itself is never built: nothing in ALL depends on it.
set(BUILD_DAEMON ON CACHE BOOL "" FORCE)

add_subdirectory("${BITCOIN_SRC}" bitcoin_build EXCLUDE_FROM_ALL)

add_library(bitcoind_c SHARED bitcoind_c.cpp)
target_compile_definitions(bitcoind_c PRIVATE BITCOIND_C_BUILDING)
target_include_directories(bitcoind_c PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${BITCOIN_SRC}/src"
  "${CMAKE_BINARY_DIR}/bitcoin_build/src")
# core_interface carries Core's own compile definitions and hardening flags, so our TU sees
# the same ABI as Core's.
target_link_libraries(bitcoind_c PRIVATE core_interface bitcoin_node bitcoin_common bitcoin_util
  univalue Boost::headers)

if(APPLE)
  target_link_options(bitcoind_c PRIVATE
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.exp")
  set_target_properties(bitcoind_c PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.exp")
elseif(WIN32)
  # PE exports come from the .def, which names exactly the C ABI.
  target_link_options(bitcoind_c PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.def"
    "-Wl,--exclude-libs,ALL")
  set_target_properties(bitcoind_c PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.def")
else()
  # Without -z noexecstack an object lacking .note.GNU-stack makes the library RWE, and
  # glibc >= 2.41 refuses to dlopen it.
  target_link_options(bitcoind_c PRIVATE
    "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.map"
    "-Wl,--exclude-libs,ALL"
    "-Wl,-z,noexecstack")
  set_target_properties(bitcoind_c PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bitcoind_c.map")
endif()

# Its own component: Core's install rules name executables this build never makes.
install(TARGETS bitcoind_c COMPONENT bitcoind_c
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib
  ARCHIVE DESTINATION lib)
install(FILES bitcoind_c.h DESTINATION include COMPONENT bitcoind_c)
