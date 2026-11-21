# The Galaxy M20 (m20lte) device layer, run by the installer inside the build
# namespace after the GSI part, with $R (the root), $DEVSRC (this folder),
# $DEVROOT (the source tree), $SRC, $P, step and ch (see the S9's layer.sh).
# Everything here is the M20's own: the GSI carries none of it.
D=$DEVSRC

step "adding the Galaxy M20 device layer"

# ztOSIAL: the manifest (every interface on its generic backend)
install -d "$R/vendor/ztos"
cp -a "$D/vendor/ztos/." "$R/vendor/ztos/"
chown -R 0:0 "$R/vendor"

# the S9's system files that are not about its hardware, then the M20's own
grep -v '^#' "$D/rootfs/from-s9" | while read -r f; do
	[ -n "$f" ] || continue
	install -D -m "$(stat -c %a "$DEVROOT/rootfs/overlay/$f")" "$DEVROOT/rootfs/overlay/$f" "$R/$f"
done
cp -a "$D/rootfs/overlay/." "$R/"
# the M20's session settings (scale 3, AFBC off for the r0p0 G71) and its own
# hardware list for ztOSCoreSensorFramework, in place of the S9's defaults
cp -a "$DEVROOT/os/overlay-m20lte/." "$R/"
# NetworkManager owns Wi-Fi (README: "NetworkManager owns Wi-Fi"); networkd
# keeps only the USB link. With the base image's wl* network left in place,
# networkd also took wlan0 and p2p0, and p2p0 never came up: wait-online held
# network-online.target (and everything after it) for two minutes a boot
rm -f "$R/etc/systemd/network/20-wlan.network"
# GSettings defaults from the overlay (Celluloid on the MFC decoder)
ch glib-compile-schemas /usr/share/glib-2.0/schemas > /dev/null 2>&1 || true
# git keeps no mode for these, and sudo ignores a sudoers file that is not 0440
chmod 440 "$R/etc/sudoers.d/10-proxy"
# The panel is the bootloader's and has no kernel driver: dsi.py talks to
# its DSIM over /dev/mem, as ztos-panel-selfmask does on the S9. On the
# phone it is ztos-m20-dsi (touch | init | cmd).
install -Dm755 "$D/scripts/dsi.py" "$R/usr/local/sbin/ztos-m20-dsi"
install -Dm644 "$D/scripts/m20_init_seq.py" "$R/usr/local/lib/ztos/m20_init_seq.py"
# the Mali-G71 derivative fix, built on the phone for its mesa (at first
# boot, and again after each mesa update: ztos-m20-mesa.service)
install -Dm755 "$D/mesa/build-mesa.sh" "$R/usr/local/lib/ztos/mesa/build-mesa.sh"
install -Dm644 "$D/mesa/pan-bi-g71-derivatives.diff" "$R/usr/local/lib/ztos/mesa/pan-bi-g71-derivatives.diff"
if [ -d "$DEVROOT/build/wallpapers/backgrounds/ztos" ]; then
	install -d "$R/usr/local/share/backgrounds"
	cp -a "$DEVROOT/build/wallpapers/backgrounds/ztos" "$R/usr/local/share/backgrounds/"
fi
install -d "$R/usr/lib/modules/7.1.0-zboot/extra"
for k in "$D"/drivers/*.ko; do
	[ -f "$k" ] && install -m644 "$k" "$R/usr/lib/modules/7.1.0-zboot/extra/"
done
K=$DEVROOT/build/linux-7.1-m20lte/out
if [ -f "$K/modules.builtin" ]; then
	install -m644 "$K/modules.builtin" "$K/modules.builtin.modinfo" "$R/usr/lib/modules/7.1.0-zboot/"
fi
ch depmod -a 7.1.0-zboot > /dev/null 2>&1 || true
chmod 755 "$R"/usr/local/sbin/* 2>/dev/null || true

# RAM: no screen reader and no Flatpak here, yet the first GTK app starts
# the accessibility bus and the document portal (~50 MB) for good, and the
# desktop portals (~55 MB more; GTK apps fall back to their own dialogs)
ch systemctl --global mask at-spi-dbus-bus.service xdg-document-portal.service \
	xdg-desktop-portal.service xdg-desktop-portal-gtk.service xdg-permission-store.service > /dev/null 2>&1 || true

# systemd-networkd runs the USB link's DHCP server (10-zboot-usb.network;
# NetworkManager leaves usb0 alone), so the computer gets 172.16.42.2
DEVICE_UNITS="zboot-usb-gadget zboot-firstboot zboot-hold systemd-networkd zrn-zram earlyoom
	ztos-m20-mesa ztos-m20-firmware"
