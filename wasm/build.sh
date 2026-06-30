#!/usr/bin/env bash
#
# Build the s2bindings WebAssembly module for the browser.
#
# Produces a self-contained ES module (wasm/dist/s2bindings.mjs +
# s2bindings.wasm) that statically absorbs abseil + s2geometry + the FFI shim and
# exports the C ABI from csrc/shim.h. The JS/TS wrapper in wasm/src wraps that
# module into SphericalPolygon / Delaunay classes.
#
# Prerequisites:
#   - Emscripten on PATH (run `source "$EMSDK/emsdk_env.sh"` first), and
#   - the vendored C++ submodules checked out:
#       git submodule update --init --recursive
#
# Steps: (1) cross-compile libcrypto for wasm (cached, see scripts/build-openssl.sh),
#        (2) configure the CMake superbuild through emcmake, (3) build the module,
#        (4) copy the artifacts into wasm/dist.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SYS_CRATE="$REPO_ROOT/crates/s2bindings-sys"
CSRC="$SYS_CRATE/csrc"
VENDOR="$SYS_CRATE/vendor"
BUILD_DIR="$SCRIPT_DIR/.build"
DIST_DIR="$SCRIPT_DIR/dist"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

command -v emcmake >/dev/null 2>&1 || {
  echo "error: emcmake not on PATH. Run: source \"\$EMSDK/emsdk_env.sh\"" >&2
  exit 1
}
[[ -f "$VENDOR/s2geometry/CMakeLists.txt" && -f "$VENDOR/abseil-cpp/CMakeLists.txt" ]] || {
  echo "error: vendored submodules missing. Run: git submodule update --init --recursive" >&2
  exit 1
}

# 1. libcrypto (BIGNUM) for wasm -- s2geometry's ExactFloat needs it.
echo ">> [1/4] OpenSSL (libcrypto) for wasm"
eval "$(JOBS="$JOBS" bash "$SCRIPT_DIR/scripts/build-openssl.sh" | grep '^OPENSSL_PREFIX=')"
: "${OPENSSL_PREFIX:?build-openssl.sh did not report OPENSSL_PREFIX}"

# 2. Configure the superbuild for Emscripten.
#
# The OpenSSL_* paths are passed as full-path cache entries so s2geometry's
# `find_package(OpenSSL REQUIRED)` resolves to our wasm libcrypto instead of
# searching the (sysroot-restricted) emscripten find paths.
echo ">> [2/4] configure (emcmake)"
emcmake cmake -S "$CSRC" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DVENDOR_DIR="$VENDOR" \
  -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX" \
  -DOPENSSL_USE_STATIC_LIBS=ON \
  -DOPENSSL_INCLUDE_DIR="$OPENSSL_PREFIX/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_PREFIX/lib/libcrypto.a" \
  -DOPENSSL_SSL_LIBRARY="$OPENSSL_PREFIX/lib/libssl.a"

# 3. Build the wasm module.
echo ">> [3/4] build (target: s2bindings)"
cmake --build "$BUILD_DIR" --target s2bindings -j"$JOBS"

# 4. Collect artifacts. The generated emscripten factory (s2bindings.core.*) is
# the low-level module; the hand-written wrapper ships as the package entry
# point `s2bindings.mjs` and imports `./s2bindings.core.mjs`.
echo ">> [4/4] stage dist/"
mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/s2bindings.core.mjs" "$BUILD_DIR/s2bindings.core.wasm" "$DIST_DIR/"
cp "$SCRIPT_DIR/src/s2bindings.mjs" "$DIST_DIR/s2bindings.mjs"
cp "$SCRIPT_DIR/src/s2bindings.d.ts" "$DIST_DIR/s2bindings.d.ts"

echo
echo "Built:"
ls -la "$DIST_DIR"/*.mjs "$DIST_DIR"/*.wasm "$DIST_DIR"/*.d.ts
echo
echo "Smoke-test with:   node wasm/test/smoke.mjs"
echo "Browser demo:      serve wasm/ and open wasm/example/index.html"
