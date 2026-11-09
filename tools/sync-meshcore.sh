#!/bin/sh
# Copies the subset of MeshCore (https://github.com/meshcore-dev/MeshCore, MIT) that HuckleBerry
# uses into lib/MeshCore. Rerun with a different MESHCORE_REF to update it, or set MESHCORE_SRC to
# use an existing checkout at that ref. That includes the companion radio firmware's node
# (examples/companion_radio), which the phone app talks to over Bluetooth; tools/meshcore/ holds
# HuckleBerry's target.h for it and a patch that stores its data on FFat instead of SPIFFS.
set -eu

MESHCORE_REF=${MESHCORE_REF:-81dbae592ffad40b4212bc0b37e82b38da08f4bc}
APP=$(cd "$(dirname "$0")/.." && pwd)
DEST="$APP/lib/MeshCore"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ -n "${MESHCORE_SRC:-}" ]; then
    SRC=$(cd "$MESHCORE_SRC" && pwd)
    [ "$(git -C "$SRC" rev-parse HEAD)" = "$MESHCORE_REF" ] || { echo "$SRC is not at $MESHCORE_REF"; exit 1; }
else
    git clone -q https://github.com/meshcore-dev/MeshCore.git "$TMP/MeshCore"
    git -C "$TMP/MeshCore" checkout -q "$MESHCORE_REF"
    SRC="$TMP/MeshCore"
fi

rm -rf "$DEST"
mkdir -p "$DEST/src/helpers/radiolib" "$DEST/src/helpers/sensors" "$DEST/src/companion_radio"
cp "$SRC/license.txt" "$DEST/"
for f in MeshCore.h Dispatcher.h Dispatcher.cpp Identity.h Identity.cpp Mesh.h Mesh.cpp \
         Packet.h Packet.cpp Utils.h Utils.cpp; do
    cp "$SRC/src/$f" "$DEST/src/"
done
for f in BaseChatMesh.h BaseChatMesh.cpp ContactInfo.h ChannelDetails.h AdvertDataHelpers.h \
         AdvertDataHelpers.cpp TxtDataHelpers.h TxtDataHelpers.cpp UTF8Helpers.h \
         StaticPoolPacketManager.h StaticPoolPacketManager.cpp SimpleMeshTables.h \
         IdentityStore.h IdentityStore.cpp ArduinoHelpers.h \
         BaseSerialInterface.h TransportKeyStore.h TransportKeyStore.cpp RegionMap.h RegionMap.cpp \
         ConfigSerializer.h ConfigSerializer.cpp DynamicConfigSerializer.h DynamicConfigSerializer.cpp \
         CommonRadioPrefs.h CommonRadioPrefs.cpp KeyValueStore.h SensorManager.h; do
    cp "$SRC/src/helpers/$f" "$DEST/src/helpers/"
done
cp "$SRC/src/helpers/sensors/LocationProvider.h" "$DEST/src/helpers/sensors/"
for f in MyMesh.h MyMesh.cpp DataStore.h DataStore.cpp NodePrefs.h; do
    cp "$SRC/examples/companion_radio/$f" "$DEST/src/companion_radio/"
done
for f in RadioLibWrappers.h RadioLibWrappers.cpp CustomSX1262.h CustomSX1262Wrapper.h SX126xReset.h; do
    cp "$SRC/src/helpers/radiolib/$f" "$DEST/src/helpers/radiolib/"
done
# Identity.cpp includes <ed_25519.h>, so the bundled ed25519 sources sit at the include root.
cp "$SRC"/lib/ed25519/*.c "$SRC"/lib/ed25519/*.h "$DEST/src/"
cp "$SRC/lib/ed25519/license.txt" "$DEST/license-ed25519.txt"
cp "$APP/tools/meshcore/target.h" "$DEST/src/"
patch -s -d "$DEST" -p1 < "$APP/tools/meshcore/huckleberry.patch"

cat > "$DEST/library.properties" <<EOF
name=MeshCore
version=0.0.0+$(echo "$MESHCORE_REF" | cut -c1-7)
author=Scott Powell / rippleradios.com
maintainer=HuckleBerry (vendored subset)
sentence=Subset of MeshCore used by HuckleBerry, synced from $MESHCORE_REF by tools/sync-meshcore.sh
paragraph=
category=Communication
url=https://github.com/meshcore-dev/MeshCore
architectures=esp32
EOF
echo "Synced MeshCore $MESHCORE_REF into $DEST"
