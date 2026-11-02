#!/bin/sh
# Builds fhi-<version>.tar.gz (source) into the project folder, after checking
# that it really builds and installs from a clean copy.  Usage: packaging/build-tarball.sh
set -eu

VERSION=$(cat "$(dirname "$0")/VERSION")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir "$WORK/fhi-$VERSION"
cd "$ROOT"
cp -r CMakeLists.txt LICENSE README.md main.c preview.c preview.h filetype.c filetype.h gitinfo.c gitinfo.h sshinfo.c sshinfo.h theme.c theme.h zsh icons fonts themes "$WORK/fhi-$VERSION/"
mkdir "$WORK/fhi-$VERSION/packaging"
cp packaging/VERSION packaging/dev.forcedev.myterm.desktop packaging/PKGBUILD packaging/INSTALL.txt "$WORK/fhi-$VERSION/packaging/"
cp packaging/INSTALL.txt "$WORK/fhi-$VERSION/"

# self-test: build + install from the copy
cmake -S "$WORK/fhi-$VERSION" -B "$WORK/b" -G Ninja -DFHI_PACKAGE=ON -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$WORK/b" >/dev/null
DESTDIR="$WORK/dest" cmake --install "$WORK/b" >/dev/null
test -x "$WORK/dest/usr/bin/FHI" && test -f "$WORK/dest/usr/share/fhi/zsh/.zshrc"

tar -C "$WORK" --owner=0 --group=0 -czf "$ROOT/fhi-$VERSION.tar.gz" "fhi-$VERSION"
echo "wrote $ROOT/fhi-$VERSION.tar.gz"
