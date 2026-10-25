#!/bin/sh
# Builds the distributable macOS app (Intel and Apple silicon, macOS 12+, no
# game data inside) and zips it with its read-me for sharing:
# build/TheMinishCap-macos-universal.zip. Run from anywhere in the checkout.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
build=$root/build/dist
package=$root/build/package/The\ Minish\ Cap
zip=$root/build/TheMinishCap-macos-universal.zip

cmake -S "$root/port" -B "$build" -DTMC_BAKE_DATA=OFF -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" --target tmc_app -j4

rm -rf "$package" "$zip"
mkdir -p "$package"
ditto "$build/The Minish Cap.app" "$package/The Minish Cap.app"
cp "$root/port/dist/Read me first.txt" "$package/"
codesign --verify --deep --strict "$package/The Minish Cap.app"
ditto -c -k --norsrc --noextattr --keepParent "$package" "$zip"
echo "$zip ($(stat -f %z "$zip") bytes): $(lipo -archs "$package/The Minish Cap.app/Contents/MacOS/The Minish Cap")"
