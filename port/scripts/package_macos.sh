#!/bin/sh
# Builds the distributable macOS app (Intel and Apple silicon, macOS 12+, no
# game data inside) and zips it with its read-me for sharing:
# build/TheMinishCap-VERSION-macos-universal.zip, VERSION from port/CMakeLists.txt.
# Run from anywhere in the checkout.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
build=$root/build/dist
package=$root/build/package/The\ Minish\ Cap
version=$(sed -n 's/^project(tmc_native VERSION \([0-9.]*\).*/\1/p' "$root/port/CMakeLists.txt")
zip=$root/build/TheMinishCap-$version-macos-universal.zip

cmake -S "$root/port" -B "$build" -DTMC_BAKE_DATA=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target tmc_app -j4

rm -rf "$package" "$zip"
mkdir -p "$package"
ditto "$build/The Minish Cap.app" "$package/The Minish Cap.app"
cp "$root/port/dist/Read me first.txt" "$package/"
# The build signs only when it relinks, so a change to Info.plist alone (the version) would leave a stale signature.
codesign --force --sign - "$package/The Minish Cap.app"
codesign --verify --deep --strict "$package/The Minish Cap.app"
ditto -c -k --norsrc --noextattr --keepParent "$package" "$zip"
echo "$zip ($(stat -f %z "$zip") bytes): $(lipo -archs "$package/The Minish Cap.app/Contents/MacOS/The Minish Cap")"
