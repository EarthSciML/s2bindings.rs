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
add_library(s2bindings_shim SHARED shim.cc)
target_include_directories(s2bindings_shim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(s2bindings_shim PRIVATE s2)

# No install() rule: build.rs links libs2bindings_shim directly from the CMake
# build directory (see CMAKE_SKIP_INSTALL_RULES above).
