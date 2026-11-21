#!/bin/sh
# Power the Galaxy M20's rear main camera (S5K3L6) on or off in Samsung's order
# (fimc-is-device-module-3l6.c, exynos7904-m20_camera.dtsi): reset low, AVDD
# 2.8 V (S2MPB03 LDO6), DVDD 1.05 V (LDO2), VDDIO 1.8 V (LDO3), AF 2.8 V enable
# (gpg1-5), reset high (gpg3-4). The S2MPB03 is at 0x56 on gpc2-0/1 (SDA/SCL).
# MCLK2 (26 MHz) on gpc0-2 through mclk.py, before reset rises. GPIOs are held by background gpioset processes (pid files).
#   sudo rear-power.sh on|off
set -eu
cd "$(dirname "$0")"
P="./bbi2c /dev/gpiochip26 0 1 0x56"
hold() {	# hold CHIP LINE VALUE NAME
	pf=/run/m20cam-$4.pid
	[ -f "$pf" ] && kill "$(cat "$pf")" 2>/dev/null || true
	setsid gpioset -c "$1" "$2=$3" </dev/null >/dev/null 2>&1 & echo $! > "$pf"
	sleep 0.05
}
release() {
	pf=/run/m20cam-$1.pid
	[ -f "$pf" ] && kill "$(cat "$pf")" 2>/dev/null || true
	rm -f "$pf"
}
case "${1:-}" in
on)
	hold gpiochip22 4 0 reset			# gpg3-4 RCAM1_RST_N low
	$P w 0x08 0xa8; sleep 0.001			# LDO6 AVDD 2.8 V on
	$P w 0x04 0x9c; sleep 0.005			# LDO2 DVDD 1.05 V on
	$P w 0x05 0xac; sleep 0.005			# LDO3 VDDIO 1.8 V on
	hold gpiochip20 5 1 af; sleep 0.012		# gpg1-5 AF 2.8 V enable
	python3 -I ./mclk.py on 2; sleep 0.010		# MCLK2 26 MHz
	hold gpiochip22 4 1 reset; sleep 0.005		# reset high
	$P r 0x03 7
	;;
off)
	hold gpiochip22 4 0 reset; sleep 0.005
	python3 -I ./mclk.py off 2
	release af
	$P w 0x05 0x2c; $P w 0x04 0x1c; $P w 0x08 0x28	# LDO3, LDO2, LDO6 off
	release reset
	$P r 0x03 7
	;;
*)
	echo "usage: rear-power.sh on|off" >&2; exit 2 ;;
esac
