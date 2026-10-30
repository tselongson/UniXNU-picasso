#!/bin/sh
# Build the owner-root policy and the verified-root archive this board's OS image allows.
#
# The owner-root stack is what sheng enables and this board did not: OWNER_ROOT_TRUST and
# VERIFIED_ROOT_PROBE want build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a, and that archive
# is not in any repository. It is produced here, from the original OS image, signed with a key this
# project generates for itself -- "owner" means the device owner, not Apple.
#
# Requires macOS: hdiutil to attach the original image read-only, xcrun to build the host library.
#
#   sh scripts/picasso-owner-root.sh <tree> <ipsw-dmg>
set -eu

TREE=${1:?usage: picasso-owner-root.sh <tree> <ipsw-dmg>}
DMG=${2:?usage: picasso-owner-root.sh <tree> <ipsw-dmg>}

KIT="$TREE/kits/VerifiedRoot"
HOST="$TREE/build/verified-root-host"
KEYS="$HOST/keys"
VOLUMES="$TREE/.cache/firmware/target-volumes"
IMAGE="$VOLUMES/044-76040-038.dmg"

mkdir -p "$KEYS" "$VOLUMES"

# 1. The owner key pair. 3072-bit RSA, as the kit's own test uses.
if [ ! -f "$KEYS/owner.pem" ]; then
  ( umask 077 && openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$KEYS/owner.pem" )
fi
openssl pkey -in "$KEYS/owner.pem" -pubout -out "$KEYS/owner-public.pem"

# 2. The host library the policy step loads (vr_host_open / vr_host_block).
OPENSSL_PREFIX=$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl)
xcrun clang -std=c11 -Wall -Wextra -dynamiclib \
  -I "$KIT/include" -I "$OPENSSL_PREFIX/include" \
  "$KIT/src/verified_root.c" "$KIT/tests/openssl_provider.c" \
  -L "$OPENSSL_PREFIX/lib" -lcrypto -o "$HOST/verified-root.dylib"

# 3. The original image, at the exact path and with the exact digest the policy step checks.
if [ ! -f "$IMAGE" ]; then
  cp "$DMG" "$IMAGE"
fi
ACTUAL=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
EXPECTED=3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d
if [ "$ACTUAL" != "$EXPECTED" ]; then
  echo "OS image digest $ACTUAL is not the supplied iPad7,1 member" >&2
  exit 1
fi

# The policy step inspects hdiutil's attachment list, so the image has to be attached read-only first.
# The policy step reads the volume through this exact mount point, so attach there rather than
# letting hdiutil choose one under /Volumes.
MOUNT="$VOLUMES/os-mount"
rmdir "$MOUNT" 2>/dev/null || true
hdiutil attach "$IMAGE" -readonly -nobrowse -noautoopen -mountpoint "$MOUNT" >/dev/null
trap 'hdiutil detach "$MOUNT" -force >/dev/null 2>&1 || true' EXIT INT TERM
python3 "$TREE/scripts/build-owner-os-policy.py"

# 4. The archive the kernel link wants. --component os is the single-volume form; the two-volume form
#    is what refused earlier, and it is not needed here.
python3 "$TREE/scripts/build-verified-root-probe.py" --component os

ls -l "$TREE/build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a"
