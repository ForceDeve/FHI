#!/bin/sh
# Builds fhi-<version>-1-<arch>.pkg.tar.zst into the project folder WITHOUT needing makepkg
# (so it works on Debian too). On Arch itself, packaging/PKGBUILD + makepkg is the proper way.
# Usage: packaging/build-arch.sh
set -eu

VERSION=$(cat "$(dirname "$0")/VERSION")
MAINTAINER=${FHI_MAINTAINER:-"ForceDev <62995666+ForceDeve@users.noreply.github.com>"}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=$(uname -m)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cmake -S "$ROOT" -B "$WORK/build" -G Ninja -DFHI_PACKAGE=ON \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/build"
DESTDIR="$WORK/pkg" cmake --install "$WORK/build" --strip
if readelf -d "$WORK/pkg/usr/bin/FHI" | grep -q -E 'RPATH|RUNPATH'; then
  echo "error: binary still has an RPATH/RUNPATH" >&2; exit 1
fi
install -Dm644 "$ROOT/LICENSE" "$WORK/pkg/usr/share/licenses/fhi/LICENSE"
rm -rf "$WORK/pkg/usr/share/doc"   # Arch keeps licenses under share/licenses

SIZE=$(du -sb "$WORK/pkg" | cut -f1)
cat > "$WORK/pkg/.PKGINFO" <<INFO
pkgname = fhi
pkgbase = fhi
pkgver = $VERSION-1
pkgdesc = Terminal with a file list that previews what your command will do
url = https://github.com/ForceDeve
builddate = $(date +%s)
packager = $MAINTAINER
size = $SIZE
arch = $ARCH
license = custom:FHI
depend = gtk4
depend = vte4
depend = zsh
depend = fontconfig
depend = pango
depend = glib2
optdepend = git: preview of what git commit / git push will include
INFO

OUT="$ROOT/fhi-$VERSION-1-$ARCH.pkg.tar.zst"
# .PKGINFO must be the first entry; the rest is sorted. --no-recursion: the list below already
# names every directory and file, without it each file is stored once per parent folder.
(cd "$WORK/pkg" && tar --no-recursion --owner=0 --group=0 --numeric-owner -I zstd -cf "$OUT" .PKGINFO \
   $(find . -mindepth 1 ! -name .PKGINFO | sed 's|^\./||' | LC_ALL=C sort))
echo "wrote $OUT"
