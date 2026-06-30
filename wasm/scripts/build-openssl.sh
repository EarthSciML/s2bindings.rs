#!/usr/bin/env bash
#
# Cross-compile OpenSSL's libcrypto (BIGNUM) for WebAssembly with Emscripten.
#
# s2geometry hard-requires OpenSSL: its arbitrary-precision `ExactFloat`
# (the exact-arithmetic fallback of the robust geometric predicates) is built
# directly on OpenSSL's BIGNUM API (`#include <openssl/bn.h>`). The browser has
# no system OpenSSL, so we cross-compile a static libcrypto/libssl here and feed
# it to the s2geometry CMake build (see ../build.sh).
#
# Only BIGNUM (libcrypto) is actually used; libssl is built solely so CMake's
# `find_package(OpenSSL REQUIRED)` is satisfied. Everything network/crypto-engine
# related is disabled (`no-engine`, `no-async`, `no-threads`, ...): we use none of
# it, and several of those subsystems (ucontext-based async, AF_ALG, dlopen) have
# no analogue under wasm anyway.
#
# The build is cached: if $PREFIX/lib/libcrypto.a already exists this is a no-op.
# Override the version with OPENSSL_VERSION; output lands in $PREFIX.
set -euo pipefail

# OpenSSL 1.1.1 is the last branch with the simple Configure recipe that
# cross-compiles cleanly under emscripten. It is used here purely as a BIGNUM
# math library (no TLS, no network), so its end-of-life status is irrelevant.
OPENSSL_VERSION="${OPENSSL_VERSION:-1.1.1w}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WASM_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CACHE_DIR="${CACHE_DIR:-$WASM_DIR/.cache}"
PREFIX="${OPENSSL_PREFIX:-$CACHE_DIR/openssl-$OPENSSL_VERSION-wasm}"

# Idempotent: skip a rebuild if the static libs are already present.
if [[ -f "$PREFIX/lib/libcrypto.a" && -f "$PREFIX/lib/libssl.a" ]]; then
  echo "[openssl] cached build present at $PREFIX -- skipping" >&2
  echo "OPENSSL_PREFIX=$PREFIX"
  exit 0
fi

command -v emcc >/dev/null 2>&1 || {
  echo "[openssl] error: emcc not on PATH; run 'source \$EMSDK/emsdk_env.sh' first" >&2
  exit 1
}

mkdir -p "$CACHE_DIR"
src_tgz="$CACHE_DIR/openssl-$OPENSSL_VERSION.tar.gz"
src_dir="$CACHE_DIR/openssl-$OPENSSL_VERSION"

if [[ ! -f "$src_tgz" ]]; then
  url="https://github.com/openssl/openssl/releases/download/OpenSSL_${OPENSSL_VERSION//./_}/openssl-$OPENSSL_VERSION.tar.gz"
  echo "[openssl] downloading $url" >&2
  curl -fsSL "$url" -o "$src_tgz"
fi

rm -rf "$src_dir"
mkdir -p "$src_dir"
tar -xzf "$src_tgz" -C "$CACHE_DIR"

cd "$src_dir"

# linux-generic32: wasm32 is an ILP32 target (32-bit long/pointer), so OpenSSL's
# generic 32-bit BIGNUM config (32-bit limbs, 64-bit products via BN_LLONG) is the
# correct fit; linux-generic64 would wrongly assume a 64-bit `long`.
#
# no-asm        : no hand-written assembly (none exists for wasm)
# no-threads    : we call BIGNUM single-threaded; avoids pthread requirement
# no-shared/dso : static only; wasm has no dlopen
# no-engine     : no dynamic crypto engines
# no-async      : OpenSSL async uses ucontext/fibers, unavailable under wasm
# no-afalgeng   : Linux AF_ALG kernel engine, N/A
# no-tests      : we never build or run the test suite
#
# --cross-compile-prefix= (empty) is essential: emconfigure passes emcc as an
# absolute path, from which OpenSSL's Configure would otherwise derive a bogus
# CROSS_COMPILE prefix and emit a doubled "<dir>/em<dir>/emcc" compiler command.
echo "[openssl] configuring (linux-generic32, static, no-asm) ..." >&2
emconfigure ./Configure linux-generic32 \
  no-asm no-threads no-shared no-dso no-engine no-async no-afalgeng no-tests \
  --cross-compile-prefix= \
  --prefix="$PREFIX" >/dev/null

# build_generated materializes the configured headers (opensslconf.h, bn_conf.h,
# ...) that ExactFloat's <openssl/bn.h> include chain needs.
echo "[openssl] generating headers ..." >&2
emmake make -j"${JOBS:-4}" build_generated >/dev/null

echo "[openssl] building libcrypto.a + libssl.a (this takes a few minutes) ..." >&2
emmake make -j"${JOBS:-4}" libcrypto.a libssl.a >/dev/null

# Install just the public headers + static libs (no docs, no executables).
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp libcrypto.a libssl.a "$PREFIX/lib/"
cp -R include/openssl "$PREFIX/include/"

# Sanity check: BIGNUM must actually be in the archive we just built.
if ! emar t "$PREFIX/lib/libcrypto.a" | grep -q '^bn_'; then
  echo "[openssl] error: libcrypto.a has no bn_*.o objects (BIGNUM missing)" >&2
  exit 1
fi

echo "[openssl] done -> $PREFIX" >&2
echo "OPENSSL_PREFIX=$PREFIX"
