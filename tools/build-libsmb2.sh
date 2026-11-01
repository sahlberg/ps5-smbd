#!/usr/bin/env bash
# Builds libsmb2 (LGPL-2.1) as a static library:
#   tools/build-libsmb2.sh ps5    for the payload, with the ps5-payload-dev SDK  -> .deps/ps5
#   tools/build-libsmb2.sh host   for the host build the tests run against       -> .deps/host
#   tools/build-libsmb2.sh ps3    for the PS3, with PSL1GHT's ppu-gcc             -> .deps/ps3
# The source is GitHub's archive of one commit of master, pinned by sha256, with patches/libsmb2-*.patch
# applied. Not the 6.3 release: its tag sits on a side branch 537 commits behind master, without master's
# server and decoder fixes (docs/decision-log.md D2).
set -euo pipefail

LIBSMB2_COMMIT=565460de50d97c451d2692d9fab48314b47e453a  # master, 2026-10-31
LIBSMB2_SHA256=f439041a248539e0c99e6581cdcf740c773700b4b25180f5862b5dba96147d30

target="${1:-}"
root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/.deps/src"
build="$root/.deps/build-$target"
prefix="$root/.deps/$target"

case "$target" in
  ps5)
    sdk="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
    [[ -x "$sdk/bin/prospero-cmake" ]] || { echo "no payload SDK at $sdk (set PS5_PAYLOAD_SDK)" >&2; exit 1; }
    cmake=("$sdk/bin/prospero-cmake")
    ;;
  host)
    cmake=(cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
    ;;
  ps3)
    ;;
  *)
    echo "usage: $0 ps5|host|ps3" >&2
    exit 2
    ;;
esac

[[ -f "$prefix/lib/libsmb2.a" && -f "$prefix/lib/libdcerpc.a" ]] && exit 0
mkdir -p "$src" "$build"
archive="$src/libsmb2-$LIBSMB2_COMMIT.tar.gz"
if [[ ! -f "$archive" ]]; then
  curl -fsSL -o "$archive.part" "https://github.com/sahlberg/libsmb2/archive/$LIBSMB2_COMMIT.tar.gz"
  mv "$archive.part" "$archive"
fi
echo "$LIBSMB2_SHA256  $archive" | sha256sum -c - >/dev/null

rm -rf "$build"
mkdir -p "$build"
tar -xzf "$archive" -C "$build"
# Fixes to libsmb2's server, see each patch's file (patches/*.patch).
for patch in "$root"/patches/libsmb2-*.patch; do
  [[ -f "$patch" ]] && patch -s -p1 -d "$build/libsmb2-$LIBSMB2_COMMIT" < "$patch"
done

# The PS3 port of libsmb2 is its own makefile (the same one its CI builds). libdcerpc's cmake refuses
# anything but Linux, so the coders are generated with idlcomp.py and compiled with ppu-gcc.
if [[ "$target" == ps3 ]]; then
  PS3DEV="${PS3DEV:-/data/sahlberg/PS3/ps3dev}"
  PSL1GHT="${PSL1GHT:-$PS3DEV}"
  export PS3DEV PSL1GHT
  export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH"
  cc="$PS3DEV/ppu/bin/ppu-gcc"
  ar="$PS3DEV/ppu/bin/ppu-ar"
  [[ -x "$cc" ]] || { echo "no ppu-gcc at $cc (set PS3DEV)" >&2; exit 1; }
  tree="$build/libsmb2-$LIBSMB2_COMMIT"
  (cd "$tree/lib" && make -f Makefile.PS3_PPU -j"$(nproc)")
  mkdir -p "$prefix/lib" "$prefix/include/smb2" "$prefix/include"
  cp "$tree/lib/libsmb2.a" "$prefix/lib/"
  cp "$tree/include/smb2/"*.h "$prefix/include/smb2/"
  cp "$tree/include/libsmb2-private.h" "$prefix/include/"

  dc="$tree/libdcerpc"
  gen="$build/dcerpc-gen"
  cfg="$build/dcerpc-config"
  objdir="$build/dcerpc-obj"
  mkdir -p "$cfg" "$objdir" "$prefix/include/dcerpc"
  # No HAVE_POLL_H and no HAVE_LIBSMB2: the server pipe does not use the socket transport.
  cat > "$cfg/config.h" << 'EOF'
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UNISTD_H 1
#define HAVE_UNISTD_H 1
EOF
  ifaces="srvsvc wkssvc winreg lsa samr dssetup netlogon epm"
  idl_includes=""
  for iface in $ifaces; do
    mkdir -p "$gen/$iface/dcerpc"
    python3 "$dc/idlcomp.py" "$dc/$iface/$iface.idl" --libdcerpc \
      -o "$gen/$iface/${iface}_coder.c" \
      --header "$gen/$iface/dcerpc/${iface}.h"
    cp "$gen/$iface/dcerpc/${iface}.h" "$prefix/include/dcerpc/"
    idl_includes="$idl_includes -I$gen/$iface"
  done
  cp "$dc/include/dcerpc/"*.h "$prefix/include/dcerpc/"
  # newlib has no O_CLOEXEC; schannel's open of /dev/urandom falls back without it.
  cflags=(-mcpu=cell -std=gnu11 -O2 -Wall -DPS3_PPU_PLATFORM -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64
          "-D_U_=__attribute__((unused))" -DO_CLOEXEC=0
          -I"$cfg" $idl_includes -I"$dc/include" -I"$dc" -I"$root/compat" -I"$PSL1GHT/ppu/include")
  objs=()
  for src in dcerpc.c dcerpc-dtyp.c dcerpc-epm.c dcerpc-server.c dcerpc-unicode.c schannel.c; do
    obj="$objdir/${src%.c}.o"
    "$cc" "${cflags[@]}" -c "$dc/$src" -o "$obj"
    objs+=("$obj")
  done
  for iface in $ifaces; do
    obj="$objdir/${iface}_coder.o"
    "$cc" "${cflags[@]}" -c "$gen/$iface/${iface}_coder.c" -o "$obj"
    objs+=("$obj")
  done
  "$ar" rcs "$prefix/lib/libdcerpc.a" "${objs[@]}"
  echo "libsmb2 and libdcerpc ${LIBSMB2_COMMIT:0:8} built for $target in $prefix"
  exit 0
fi

"${cmake[@]}" -S "$build/libsmb2-$LIBSMB2_COMMIT" -B "$build/out" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF \
  -DENABLE_LIBQUIC=OFF -DENABLE_RDMA=OFF -DENABLE_LIBDCERPC=OFF \
  -DENABLE_EXAMPLES=OFF >/dev/null
cmake --build "$build/out" --parallel 8 >/dev/null
cmake --install "$build/out" >/dev/null

# libdcerpc (BSD-2-Clause) from the same tree, without its libsmb2 transport: its coders and server runtime
# for the srvsvc pipe (src/rpc.c). compat/ holds the one glibc header the PS5 lacks.
"${cmake[@]}" -S "$build/libsmb2-$LIBSMB2_COMMIT/libdcerpc" -B "$build/dcerpc" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DWITH_LIBSMB2=OFF -DENABLE_UTILS=OFF \
  -DENABLE_EXAMPLES=OFF -DCMAKE_C_FLAGS="-I$root/compat" >/dev/null
cmake --build "$build/dcerpc" --parallel 8 >/dev/null
cmake --install "$build/dcerpc" >/dev/null
echo "libsmb2 and libdcerpc ${LIBSMB2_COMMIT:0:8} built for $target in $prefix"
