#!/bin/sh
# Runtime-only MediaTek HNAT switch; TurboACC's persistent settings stay intact.
hook=/sys/kernel/debug/hnat/hook_toggle
case "$1" in
	status) if [ -e "$hook" ]; then cat "$hook"; else echo unavailable; fi; exit $?;;
	disable) value=0; expected=disabled;;
	enable) value=1; expected=enabled;;
	*) echo 'Usage: radaer-hnat disable|enable|status' >&2; exit 1;;
esac
[ -e "$hook" ] || exit 0
[ "$(cat "$hook")" = "$expected" ] && exit 0
if ! printf '%s\n' "$value" > "$hook" || [ "$(cat "$hook")" != "$expected" ]; then
	logger -t radaer "Failed to set MediaTek HNAT to $expected"
	echo "Radaer: failed to set MediaTek HNAT to $expected" >&2
	exit 1
fi
logger -t radaer "MediaTek HNAT $expected"
