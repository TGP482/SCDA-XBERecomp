#!/usr/bin/env bash
# Builds SCDA-XBERecomp-x86_64.AppImage in out/appimage: the launcher, both game executables, and the compiler that
# prepares the game's code on the first start (xbe_recomp, clang and lld from this system with their libraries).
# No game code or data goes in it: the player's own disc image is pointed at from the launcher.
# Built on this machine it runs on systems with this glibc or newer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/out/build/linux-release"
OUT="$ROOT/out/appimage"
APPDIR="$OUT/AppDir"
TOOLS="$OUT/tools"
HERE="$ROOT/tools/linux/appimage"

echo "[1/5] Build"
cmake --preset linux-release > /dev/null
cmake --build "$BUILD" --target SCDA SCDA_Versus SCDA_Launcher xbe_recomp compiler_files

echo "[2/5] AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/compiler/bin" "$APPDIR/usr/bin/compiler/lib" "$APPDIR/usr/lib" \
    "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp "$BUILD/SCDA" "$BUILD/SCDA_Versus" "$BUILD/SCDA_Launcher" "$APPDIR/usr/bin/"
strip --strip-debug "$APPDIR/usr/bin/SCDA" "$APPDIR/usr/bin/SCDA_Versus" "$APPDIR/usr/bin/SCDA_Launcher"
# the compiler folder without its work: no game code
cp "$BUILD/compiler/xbe_recomp" "$BUILD/compiler/LLVM-LICENSE.txt" "$APPDIR/usr/bin/compiler/"
cp -r "$BUILD/compiler/config" "$BUILD/compiler/include" "$APPDIR/usr/bin/compiler/"
cp "$HERE/AppRun" "$APPDIR/AppRun"
cp "$HERE/scda-xberecomp.desktop" "$APPDIR/"
cp "$HERE/scda-xberecomp.desktop" "$APPDIR/usr/share/applications/"
cp "$HERE/scda-xberecomp.svg" "$APPDIR/"
cp "$HERE/scda-xberecomp.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/"
ln -sf scda-xberecomp.svg "$APPDIR/.DirIcon"

# the libraries a program needs past the C runtime, into dir (the system's glibc, libstdc++ and drivers stay)
copy_deps() {
    local bin="$1" dir="$2"
    ldd "$bin" | awk '/=> \//{print $3}' | while read -r lib; do
        case "$(basename "$lib")" in
            libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|ld-linux*|libstdc++.so*|libgcc_s.so*|libvulkan.so*) ;;
            *) cp -L -n "$lib" "$dir/" ;;
        esac
    done
}

echo "[3/5] Libraries"
copy_deps "$BUILD/SCDA" "$APPDIR/usr/lib"
copy_deps "$BUILD/SCDA_Versus" "$APPDIR/usr/lib"

echo "[4/5] Compiler"
CLANG="$(readlink -f "$(command -v clang++)")"
LLD="$(readlink -f "$(command -v ld.lld)")"
cp "$CLANG" "$APPDIR/usr/bin/compiler/bin/clang"
cp "$LLD" "$APPDIR/usr/bin/compiler/bin/ld.lld"  # lld picks its flavour by its name
copy_deps "$CLANG" "$APPDIR/usr/bin/compiler/lib"
copy_deps "$LLD" "$APPDIR/usr/bin/compiler/lib"
strip --strip-unneeded "$APPDIR/usr/bin/compiler/lib/"*.so* 2>/dev/null || true
# a quick check that the carried compiler runs from where it is
LD_LIBRARY_PATH="$APPDIR/usr/bin/compiler/lib" "$APPDIR/usr/bin/compiler/bin/clang" --version | head -1
LD_LIBRARY_PATH="$APPDIR/usr/bin/compiler/lib" "$APPDIR/usr/bin/compiler/bin/ld.lld" --version

echo "[5/5] AppImage"
# appimagetool is not downloaded here: install it (AUR: appimagetool-bin, or the release from
# github.com/AppImage/appimagetool), or name it with APPIMAGETOOL=/path/to/appimagetool
TOOL="${APPIMAGETOOL:-}"
[ -z "$TOOL" ] && TOOL="$(command -v appimagetool || true)"
[ -z "$TOOL" ] && [ -x "$TOOLS/appimagetool" ] && TOOL="$TOOLS/appimagetool"
if [ -z "$TOOL" ]; then
    echo "appimagetool not found. The AppDir is ready in $APPDIR (run its AppRun to try it)."
    echo "To make the AppImage: install appimagetool (AUR: appimagetool-bin) or set APPIMAGETOOL, then run this again."
    exit 0
fi
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$OUT/SCDA-XBERecomp-x86_64.AppImage"
ls -la "$OUT/SCDA-XBERecomp-x86_64.AppImage"
