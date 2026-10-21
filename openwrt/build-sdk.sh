#!/bin/sh
# Usage: ./openwrt/build-sdk.sh /absolute/path/to/immortalwrt-sdk [jobs]
set -eu
project="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sdk="$(CDPATH= cd -- "${1:?Pass the extracted OpenWrt/ImmortalWrt SDK directory}" && pwd)"
jobs="${2:-2}"
test -f "$sdk/include/cmake.mk"
package="$sdk/package/radaer-native"
mkdir -p "$package/src" "$package/files"
cp "$project/openwrt/Makefile" "$package/Makefile"
cp "$project/openwrt/files/"* "$package/files/"
# Copy only source inputs. Windows builds, logs and captures never enter the package.
for dir in src include third_party webui; do
	mkdir -p "$package/src/$dir"
	rsync -a --delete "$project/native/$dir/" "$package/src/$dir/"
done
cp "$project/native/CMakeLists.txt" "$package/src/CMakeLists.txt"
cd "$sdk"
printf '\nCONFIG_PACKAGE_radaer-native=m\n' >> .config
make defconfig
# Local source trees have no download hash; force preparation after every source update.
make package/radaer-native/clean
make "-j$jobs" package/radaer-native/compile V=s
find bin/packages -name 'radaer-native_*.ipk' -print
