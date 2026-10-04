#!/usr/bin/env bash
set -euo pipefail

# Build 32-bit setupapi.dll proxy for Wine / CrossOver running Nord Modular G2 Editor v1.62.
# Requires: i686-w64-mingw32-g++ (e.g. from `brew install mingw-w64`)

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

i686-w64-mingw32-g++ -shared -O2 -std=c++17 \
	-static -static-libgcc -static-libstdc++ \
	-I"$DIR" -I"$DIR/../g2Lib" \
	"$DIR/setupapi_wine.cpp" "$DIR/g2usb.cpp" "$DIR/setupapi.def" \
	-lws2_32 -lsetupapi \
	-o "$DIR/setupapi.dll"

echo "Successfully built $DIR/setupapi.dll"
