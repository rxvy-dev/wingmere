#!/bin/sh
# WINGMERE installer - works on any Linux distro.
#   ./install.sh            build and install for your user (~/.local, no root needed)
#   ./install.sh --system   install for everyone (/usr/local, uses sudo)
#   ./install.sh --no-deps  skip installing build dependencies
set -e
cd "$(dirname "$0")"

PREFIX="$HOME/.local"; SUDO=""; DEPS=1
for a in "$@"; do
  case "$a" in
    --system) PREFIX="/usr/local"; SUDO="sudo" ;;
    --no-deps) DEPS=0 ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
  esac
done

have() { command -v "$1" >/dev/null 2>&1; }
ROOT=""; [ "$(id -u)" -ne 0 ] && ROOT="sudo"

if [ "$DEPS" = 1 ]; then
  echo "==> Installing build dependencies (C compiler, CMake, SDL2, OpenGL headers, libcurl)"
  if   have apt-get; then $ROOT apt-get update && $ROOT apt-get install -y build-essential cmake pkg-config libsdl2-dev libgl-dev libcurl4-openssl-dev
  elif have dnf;     then $ROOT dnf install -y gcc cmake pkgconf-pkg-config SDL2-devel mesa-libGL-devel libcurl-devel
  elif have pacman;  then $ROOT pacman -S --needed --noconfirm base-devel cmake sdl2 mesa curl
  elif have xbps-install; then $ROOT xbps-install -Sy gcc make cmake pkg-config SDL2-devel MesaLib-devel libcurl-devel
  elif have zypper;  then $ROOT zypper --non-interactive install gcc cmake pkg-config libSDL2-devel Mesa-libGL-devel libcurl-devel
  elif have apk;     then $ROOT apk add build-base cmake pkgconf sdl2-dev mesa-dev curl-dev
  elif have emerge;  then $ROOT emerge --noreplace dev-build/cmake media-libs/libsdl2 virtual/opengl net-misc/curl
  elif have eopkg;   then $ROOT eopkg install -y -c system.devel && $ROOT eopkg install -y cmake sdl2-devel libglvnd-devel curl-devel
  elif have nix-shell; then echo "    NixOS: run  nix-shell -p cmake pkg-config SDL2 libGL curl  and re-run with --no-deps"
  else echo "    Unknown package manager. Install these yourself, then re-run with --no-deps:"
       echo "    a C compiler, cmake, pkg-config, SDL2, OpenGL and libcurl (development files)"
  fi || echo "    (dependency step reported a problem - trying to build anyway)"
fi

echo "==> Building"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

echo "==> Installing to $PREFIX"
$SUDO cmake --install build

echo
echo "Done. Run it with:  wingmere"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "($PREFIX/bin is not on your PATH - run $PREFIX/bin/wingmere, or add it to PATH)";; esac
echo "Scenery streams from the internet the first time you fly somewhere and is cached in ~/.cache/wingmere."
