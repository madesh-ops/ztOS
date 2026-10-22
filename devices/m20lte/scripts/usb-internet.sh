#!/bin/sh
# Internet for the Galaxy M20 through the computer it is plugged into, with no
# admin rights on the computer: a reverse SSH tunnel gives the phone a SOCKS
# proxy at 127.0.0.1:1080 that leaves through this computer's connection.
# The phone sends everything there: /etc/environment sets all_proxy, and sudo
# keeps it (/etc/sudoers.d/10-proxy), so pacman, curl and git just work.
#
#   devices/m20lte/scripts/usb-internet.sh [key]   (runs until Ctrl-C)
#
# The key must be in the phone's ~ztos/.ssh/authorized_keys; without one,
# ssh asks for the password (ztos). The phone is 172.16.42.1 over USB.
set -e
KEY=${1:-$HOME/.ssh/ztos_m20}
PHONE=ztos@172.16.42.1
[ -f "$KEY" ] && ID="-i $KEY" || ID=
echo "internet for the phone through this computer; Ctrl-C to stop"
# shellcheck disable=SC2086
exec ssh $ID -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 \
	-o ServerAliveCountMax=3 -N -R 127.0.0.1:1080 "$PHONE"
