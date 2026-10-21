#!/bin/sh
. /usr/share/libubox/jshn.sh

case "$1" in
list)
	printf '%s\n' '{"status":{},"logs":{},"action":{"action":""}}'
	;;
call)
	case "$2" in
	status)
		. /lib/functions.sh
		. /lib/functions/network.sh
		config_load radaer
		config_get network main network lan
		config_get port main port 8765
		config_get output main output_directory /opt/radaer/captures
		config_get_bool enabled main enabled 1
		network_get_ipaddr address "$network"
		network_get_device device "$network"
		pid="$(ubus call service list '{"name":"radaer"}' | jsonfilter -e '@.radaer.instances.instance1.pid')"
		running=0; rss=0
		case "$pid" in ''|*[!0-9]*) pid=0;; *)
			[ -d "/proc/$pid" ] && running=1
			rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)"
		;; esac
		boot=0; /etc/init.d/radaer enabled && boot=1
		json_init
		json_add_boolean running "$running"
		json_add_boolean enabled "$enabled"
		json_add_boolean bootEnabled "$boot"
		json_add_int pid "$pid"
		json_add_int memoryKB "${rss:-0}"
		json_add_string address "${address:-}"
		json_add_string device "${device:-}"
		json_add_string network "$network"
		json_add_string port "$port"
		json_add_string output "$output"
		json_add_string hardwareAcceleration "$(/usr/libexec/radaer-hnat status 2>/dev/null || echo unknown)"
		json_dump
		;;
	logs)
		logs="$(logread -e radaer 2>/dev/null | tail -n 60)"
		json_init; json_add_string text "$logs"; json_dump
		;;
	action)
		read -r request
		json_load "$request" || exit 1
		json_get_var action action
		case "$action" in
			start|stop|restart|enable|disable)
				result="$(/etc/init.d/radaer "$action" 2>&1)"; code=$?
				;;
			*) result='Unsupported Radaer action'; code=1;;
		esac
		json_init; json_add_int code "$code"; json_add_string message "$result"; json_dump
		;;
	*) exit 1;;
	esac
	;;
*) exit 1;;
esac
