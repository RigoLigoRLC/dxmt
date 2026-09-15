#!/bin/bash
set -euo pipefail
NATIVE_ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
mkdir -p "$NATIVE_ROOT/out/Reference.app/Contents/MacOS"
xcrun clang++ -std=c++17 -fobjc-arc -O2 -framework Cocoa -framework QuartzCore -framework Metal "$NATIVE_ROOT/reference.mm" -o "$NATIVE_ROOT/out/Reference.app/Contents/MacOS/reference"
cp "$NATIVE_ROOT/Info.plist" "$NATIVE_ROOT/out/Reference.app/Contents/Info.plist"
