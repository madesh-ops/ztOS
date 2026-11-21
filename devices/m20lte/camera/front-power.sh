#!/bin/sh
# Power the Galaxy M20's front camera (S5K4HA) on or off in Samsung's order
# (fimc-is-device-module-4ha.c, exynos7904-m20_camera.dtsi): reset low (gpg3-5),
# AVDD 2.8 V (S2MPB03 LDO5), DVDD 1.2 V (LDO1), VDDIO 1.8 V (LDO3), MCLK1 26 MHz
# on gpc0-1, reset high. The sensor is on camera I2C 0, gpc1-0/1. No actuator.
#   sudo front-power.sh on|off
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
	hold gpiochip22 5 0 freset			# gpg3-5 reset low
	$P w 0x07 0xa8					# LDO5 AVDD 2.8 V on
	$P w 0x03 0xa8					# LDO1 DVDD 1.2 V on
	$P w 0x05 0xac; sleep 0.002			# LDO3 VDDIO 1.8 V on
	python3 -I ./mclk.py on 1; sleep 0.001		# MCLK1 26 MHz
	hold gpiochip22 5 1 freset; sleep 0.003		# reset high
	$P r 0x03 7
	;;
off)
	hold gpiochip22 5 0 freset; sleep 0.002
	python3 -I ./mclk.py off 1
	$P w 0x05 0x2c; $P w 0x03 0x1c; $P w 0x07 0x0e	# LDO3, LDO1, LDO5 off (boot values)
	release freset
	$P r 0x03 7
	;;
*)
	echo "usage: front-power.sh on|off" >&2; exit 2 ;;
esac
