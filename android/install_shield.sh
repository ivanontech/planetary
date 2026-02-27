#!/bin/bash
# Deploy Planetary APK to NVIDIA Shield TV
# Run this when Shield is on with ADB/Network debugging enabled.
# Shield IP: 10.0.0.37 | Settings > Device Prefs > Developer Options > Network debugging

SHIELD_IP="10.0.0.37"
SHIELD_PORT="5555"
APK="app/build/outputs/apk/debug/app-debug.apk"

export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$PATH"

echo "==> Connecting to Shield TV at ${SHIELD_IP}:${SHIELD_PORT}..."
adb connect "${SHIELD_IP}:${SHIELD_PORT}"
sleep 2

if adb -s "${SHIELD_IP}:${SHIELD_PORT}" get-state 2>/dev/null | grep -q "device"; then
    echo "==> Shield connected. Installing APK..."
    adb -s "${SHIELD_IP}:${SHIELD_PORT}" install -r "$APK"
    echo "==> Done! Launch via: adb shell am start -n com.kawkaw.planetary/.PlanetaryActivity"
else
    echo "ERROR: Shield not connected."
    echo "Make sure:"
    echo "  1. Shield TV is powered on"
    echo "  2. Settings > Device Preferences > Developer Options > Network debugging: ON"
    echo "  3. Shield is on the same LAN (10.0.0.x)"
    echo ""
    echo "APK is ready at: $(pwd)/$APK"
    echo "APK size: $(ls -lh $APK 2>/dev/null | awk '{print $5}')"
fi
