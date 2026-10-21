#!/bin/sh
# OpenWrt's sourced helpers intentionally reference unset optional variables.
set -e
. /lib/functions.sh
. /lib/functions/network.sh
config_load radaer
config_get network main network lan
config_get port main port 8765
config_get output main output_directory /opt/radaer/captures
network_get_ipaddr address "$network" || { echo "Radaer: no IPv4 on $network" >&2; exit 1; }
network_get_device device "$network" || exit 1
index="$(cat "/sys/class/net/$device/ifindex")"
umask 077
mkdir -p "$output"
# Repair a stale redirect if a previous process was killed before its destructor ran.
if nft list table ip radaer_native >/dev/null 2>&1; then
	nft delete table ip radaer_native
fi
# Reassert on procd respawn as well as an explicit start/restart.
/usr/libexec/radaer-hnat disable
exec /usr/bin/radaer-native webui --lan true --lan-ip "$address" \
	--lan-if-index "$index" --port "$port" \
	--web-root /usr/share/radaer/webui --output-directory "$output"
