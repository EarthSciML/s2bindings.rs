# Superbuild for the s2bindings FFI shim.
#
# Builds the vendored C++ dependencies from source and links them, together
# with the FFI shim, into a single self-contained shared library:
#
#   abseil-cpp (static, PIC)  ->  s2geometry (static, PIC)  ->  shim (shared)
#
# Because everything below the shim is built static and absorbed into the one
# shared object, the Rust link line only needs `-ls2bindings_shim`.
cmake_minimum_required(VERSION 3.16)
project(s2bindings_shim CXX)

# We consume the shim shared library directly from the build tree (build.rs
# links it from there), so no installation is needed. Disabling install rules
# also neutralizes s2geometry's unconditional `install(EXPORT s2Targets)`,
# which would otherwise fail at generate time because the vendored abseil
# targets it references are not part of any export set.
set(CMAKE_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)

# Force C++17 across the whole superbuild. abseil and s2geometry MUST be built
# with the same C++ standard (abseil ABI depends on it); s2geometry otherwise
# defaults its cache to C++14, so we pin the cache here before adding it.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "" FORCE)

# Static vendored libs, with PIC so they can be absorbed into the shared shim.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)

# Default build type if the caller did not set one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Emscripten / WebAssembly target ----------------------------------------
# When configured through emcmake (CMAKE_SYSTEM_NAME=Emscripten), build the whole
# C++ stack for wasm and emit a browser module instead of a native shared object.
# Exceptions are enabled across abseil + s2geometry: both can throw internally
# (e.g. absl::throw_delegate, std::bad_alloc), and emscripten otherwise turns an
# uncaught throw into a hard abort. Add it before the add_subdirectory() calls so
# the vendored libraries inherit it. (libcrypto for BIGNUM is supplied by the
# caller via OPENSSL_* cache variables; see wasm/build.sh.)
if(EMSCRIPTEN)
  string(APPEND CMAKE_CXX_FLAGS " -fexceptions")
endif()

# Location of the vendored submodules (overridable by build.rs).
if(NOT DEFINED VENDOR_DIR)
  set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor")
endif()

# --- abseil-cpp -------------------------------------------------------------
# Propagate our C++ standard onto abseil's targets; do not build abseil's tests
# or install rules (we only consume it through s2geometry).
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${VENDOR_DIR}/abseil-cpp "${CMAKE_BINARY_DIR}/_abseil" EXCLUDE_FROM_ALL)

# --- s2geometry -------------------------------------------------------------
# s2geometry guards its abseil lookup with `if(NOT TARGET absl::base)`, so the
# abseil targets created above are used directly -- no system abseil required,
# no version-skew risk. OpenSSL and Threads are still found from the system.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory(${VENDOR_DIR}/s2geometry "${CMAKE_BINARY_DIR}/_s2geometry" EXCLUDE_FROM_ALL)

# --- FFI shim ---------------------------------------------------------------
# The `s2` target propagates its abseil / OpenSSL / Threads dependencies and
# its public include directories, so linking it is all the shim needs.
if(EMSCRIPTEN)
  # Browser target: link the shim + the (statically absorbed) s2geometry stack
  # into a single self-contained wasm module with an ES6-module JS loader. The C
  # ABI declared in shim.h is exported verbatim, plus malloc/free so the JS
  # wrapper can marshal coordinate arrays across the boundary. No native shared
  # library is produced in this configuration.
  #
  # The exported-function list is kept in csrc/wasm_exports.txt (a JSON array of
  # C symbols, each with the leading underscore emscripten expects) so it lives
  # next to shim.h and the two stay in sync.
  add_executable(s2bindings shim.cc)
  target_include_directories(s2bindings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(s2bindings PRIVATE s2)

  # shim.cc includes s2's exactfloat.h, which pulls in <openssl/bn.h>. s2geometry
  # exposes OpenSSL only through a directory-scoped include_directories(), so it
  # does NOT propagate to this (parent-scope) target. Natively that is hidden by
  # OpenSSL living on the default system include path; for our wasm libcrypto in
  # a custom prefix we must add it explicitly.
  target_include_directories(s2bindings PRIVATE "${OPENSSL_INCLUDE_DIR}")

  # Emit `s2bindings.core.mjs` (ES module factory) + `s2bindings.core.wasm`.
  # The hand-written wrapper ships as `s2bindings.mjs` and imports this core
  # module, so the generated factory gets the `.core` infix to avoid a clash.
  set_target_properties(s2bindings PROPERTIES OUTPUT_NAME "s2bindings.core" SUFFIX ".mjs")

  set(_exports_file "${CMAKE_CURRENT_SOURCE_DIR}/wasm_exports.txt")
  target_link_options(s2bindings PRIVATE
    "-fexceptions"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createS2Bindings"
    "-sENVIRONMENT=web,worker,node"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sFILESYSTEM=0"
    "--no-entry"
    "-sEXPORTED_FUNCTIONS=@${_exports_file}"
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,HEAPF64,HEAP32,HEAPU8"
  )
  # Re-link when the export list changes.
  set_property(TARGET s2bindings APPEND PROPERTY LINK_DEPENDS "${_exports_file}")
else()
  add_library(s2bindings_shim SHARED shim.cc)
  target_include_directories(s2bindings_shim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(s2bindings_shim PRIVATE s2)

  # No install() rule: build.rs links libs2bindings_shim directly from the CMake
  # build directory (see CMAKE_SKIP_INSTALL_RULES above).
endif()
