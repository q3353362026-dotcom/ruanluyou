#!/bin/sh
# Build just the LuCI add-on in the SDK previously prepared for build-sdk.sh.
set -eu
project="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sdk="$(CDPATH= cd -- "${1:?Pass the prepared SDK directory}" && pwd)"
test -f "$sdk/package/radaer-native/Makefile"
mkdir -p "$sdk/package/luci-app-radaer"
rsync -a --delete "$project/openwrt/luci-app-radaer/" "$sdk/package/luci-app-radaer/"
cd "$sdk"
printf '\nCONFIG_PACKAGE_luci-app-radaer=m\n' >> .config
make defconfig
make package/luci-app-radaer/clean
make "-j${2:-4}" package/luci-app-radaer/compile V=s
find bin/packages -name 'luci-app-radaer_*.ipk' -print
