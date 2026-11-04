#!/bin/sh
# Build padbridge and install it to ~/.local:
#   lib/padbridge.dylib    the bridge, inserted into the game's Wine processes
#   bin/padbridge          the Steam launch option wrapper
#   bin/padbridge-rumble   motor test, for tuning rumble settings
set -e
cd "$(dirname "$0")"
PREFIX="${PREFIX:-$HOME/.local}"
mkdir -p build "$PREFIX/lib" "$PREFIX/bin"
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-nonnull -mmacosx-version-min=11.0"
# Wine runs as x86_64 under Rosetta; arm64 is there for native tools.
clang -dynamiclib -arch x86_64 -arch arm64 $CFLAGS -framework CoreFoundation -framework IOKit \
      -install_name @rpath/padbridge.dylib -o build/padbridge.dylib src/padbridge.c
clang -arch arm64 -arch x86_64 $CFLAGS -framework CoreFoundation -framework IOKit \
      -o build/padbridge-rumble src/padbridge-rumble.c
codesign -f -s - build/padbridge.dylib build/padbridge-rumble 2>/dev/null
[ "${1:-}" = "--no-install" ] && exit 0
install -m 0644 build/padbridge.dylib "$PREFIX/lib/padbridge.dylib"
install -m 0755 scripts/padbridge-launch "$PREFIX/bin/padbridge"
install -m 0755 build/padbridge-rumble "$PREFIX/bin/padbridge-rumble"
echo "installed $PREFIX/lib/padbridge.dylib"
echo "installed $PREFIX/bin/padbridge"
echo "installed $PREFIX/bin/padbridge-rumble"
echo
echo "Steam launch option for the game (Properties > General > Launch Options):"
echo "  $PREFIX/bin/padbridge %command%"
