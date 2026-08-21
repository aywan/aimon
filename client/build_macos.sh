#!/bin/sh
# macOS requires an app bundle with an Info.plist (NSBluetoothAlwaysUsageDescription)
# for a process to be granted Bluetooth access — a bare binary gets killed by
# tccd. This builds the Go binary straight into aiboard-client.app.
set -e
cd "$(dirname "$0")"
go build -o aiboard-client.app/Contents/MacOS/aiboard-client .
echo "built aiboard-client.app/Contents/MacOS/aiboard-client"
