#!/bin/sh
# Builds fhi_<version>_<arch>.deb into the project folder.  Usage: packaging/build-deb.sh
set -eu

VERSION=$(cat "$(dirname "$0")/VERSION")
# Shown to anyone who inspects the package. Override: FHI_MAINTAINER="Name <mail>" packaging/build-deb.sh
MAINTAINER=${FHI_MAINTAINER:-"ForceDev <forcedev@users.noreply.github.com>"}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$(dpkg --print-architecture)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cmake -S "$ROOT" -B "$WORK/build" -G Ninja -DFHI_PACKAGE=ON \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/build"
DESTDIR="$WORK/pkg" cmake --install "$WORK/build" --strip

mkdir -p "$WORK/pkg/DEBIAN"
cat > "$WORK/pkg/DEBIAN/control" <<CONTROL
Package: fhi
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: zsh, libgtk-4-1 (>= 4.8), libvte-2.91-gtk4-0, libglib2.0-0
Maintainer: $MAINTAINER
Description: Terminal with a file list that previews what your command will do
 FHI is a GTK4 terminal that always runs zsh. Next to the terminal it shows
 the current folder and highlights the files the command you are typing
 would touch, create or delete.
CONTROL

# the .deb must not be owned by the build user
fakeroot sh -c "chown -R root:root '$WORK/pkg' && dpkg-deb --build '$WORK/pkg' '$ROOT/fhi_${VERSION}_${ARCH}.deb'"
