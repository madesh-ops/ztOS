#!/bin/sh
# The Galaxy M20's libgallium, with pan-bi-g71-derivatives.diff (the Mali-G71
# returns 0 for derivatives: text and edges drawn by GTK4 and Firefox break),
# built on the phone itself for whatever mesa pacman has installed, and put in
# place of Arch Linux ARM's (which is kept beside it).
#
#   build-mesa.sh auto       # what ztos-m20-mesa.service runs: if the library
#                            # in place is not ours, build, check, install
#   build-mesa.sh build      # download, verify, patch, configure, build
#   build-mesa.sh install    # check it, then replace the system library
#   build-mesa.sh restore    # put Arch's library back
#
# Run as root. A mesa update from pacman replaces the library; the pacman hook
# 90-ztos-m20-mesa.hook then starts ztos-m20-mesa.service, which runs `auto`
# (it also runs at every boot, so a fresh install or a build cut short by a
# reboot is caught up). Until it is done, about half an hour, apps draw with
# Arch's library: the derivative glitch is back for that time.
#
# The source is checked against the sha256 in Arch's own PKGBUILD for the
# installed release (fetched from gitlab.archlinux.org): the tarball is the one
# Arch built its package from. Only libgallium changes. It is built to match
# the installed package: the same version string (hence the same library name
# and symbol versions), GLX/X11/Wayland, and the drivers this phone can use
# (panfrost, llvmpipe, softpipe, zink); `install` refuses if it lacks a symbol
# the installed EGL, GLX, GBM or DRI loaders import.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/mesa-m20
STATE=/var/lib/ztos/mesa-m20	# sha256 of the library we installed
LOG=/var/log/ztos-m20-mesa.log

# the installed mesa: 1:26.2.5-1 -> VER 26.2.5, Arch's tag 1-26.2.5-1
PKG=$(pacman -Q mesa | cut -d' ' -f2)
VER=$(printf '%s' "$PKG" | sed 's/^[0-9]*://; s/-[^-]*$//')
REL=$(printf '%s' "$PKG" | sed 's/.*-//; s/\..*//')	# (Arch ARM's 1.1 is Arch's 1)
EPOCH=$(printf '%s' "$PKG" | sed -n 's/^\([0-9]*\):.*/\1/p')
LIB=$(ls /usr/lib/libgallium-"$VER"-*.so 2>/dev/null | head -1)
[ -n "$LIB" ] || { echo "no /usr/lib/libgallium-$VER-*.so for mesa $PKG" >&2; exit 1; }
ARCHVER=$(basename "$LIB" .so | sed 's/^libgallium-//')
SRC=$W/mesa-$VER
NEW=$SRC/build-arch/src/gallium/targets/dri/libgallium-$ARCHVER.so

# a word to everyone's shell (a notification), if they are logged in
tell() {
	for ctl in /run/user/*/ztmetro.ctl; do
		[ -p "$ctl" ] && timeout 2 sh -c 'printf "%s\n" "$2" > "$1"' _ "$ctl" "notify Graphics|$1|$2"
	done
	return 0
}

ours() { [ -f "$STATE" ] && [ "$(sha256sum "$LIB" | cut -d' ' -f1)" = "$(cat "$STATE")" ]; }

build() {
	# pacman holds its lock while its hooks run, and this may be started by one
	while [ -e /var/lib/pacman/db.lck ]; do sleep 5; done
	pacman -S --needed --noconfirm meson ninja python-mako python-yaml python-packaging \
		python-pyelftools glslang > /dev/null
	mkdir -p $W && cd $W
	if [ ! -f mesa-$VER.sha256 ]; then
		sum=
		for rel in $REL 1 2 3 4; do
			sum=$(curl -fsL "https://gitlab.archlinux.org/archlinux/packaging/packages/mesa/-/raw/${EPOCH:+$EPOCH-}$VER-$rel/PKGBUILD" |
				sed -n "/^sha256sums=(/{s/^sha256sums=('\([0-9a-f]\{64\}\)'.*/\1/p;q}") && [ -n "$sum" ] && break
		done
		[ -n "$sum" ] || { echo "no checksum for mesa $VER in Arch's PKGBUILD" >&2; return 1; }
		echo "$sum  mesa-$VER.tar.xz" > mesa-$VER.sha256
	fi
	[ -f mesa-$VER.tar.xz ] && sha256sum -c --quiet mesa-$VER.sha256 2>/dev/null ||
		curl -fsL -o mesa-$VER.tar.xz https://archive.mesa3d.org/mesa-$VER.tar.xz
	sha256sum -c --quiet mesa-$VER.sha256
	# a build cut short (a reboot) carries on where it was
	if [ -f $SRC/build-arch/build.ninja ] && [ -f $SRC/.patched ]; then
		ninja -C $SRC/build-arch -j4 src/gallium/targets/dri/libgallium-$ARCHVER.so
		return
	fi
	rm -rf $SRC && tar -xf mesa-$VER.tar.xz
	cd $SRC
	patch -p1 --forward < "$HERE/pan-bi-g71-derivatives.diff"
	touch .patched
	echo $ARCHVER > VERSION
	meson setup build-arch --prefix=/usr --libdir=lib -Dbuildtype=plain \
		--optimization 2 -Db_ndebug=true \
		-Dgallium-drivers=panfrost,llvmpipe,softpipe,zink -Dvulkan-drivers= \
		-Dplatforms=x11,wayland -Dglx=dri -Degl=enabled -Dgbm=enabled \
		-Dgles1=disabled -Dgles2=enabled -Dvideo-codecs= -Dgallium-va=disabled \
		-Dgallium-extra-hud=true -Dvalgrind=disabled -Dlibunwind=disabled -Dtools=
	# four jobs: eight run the phone out of memory
	ninja -C build-arch -j4 src/gallium/targets/dri/libgallium-$ARCHVER.so
}

install_lib() {
	[ -f "$NEW" ] || { echo "no $NEW: run build first" >&2; return 1; }
	need=$(for l in /usr/lib/libEGL_mesa.so.0 /usr/lib/libGLX_mesa.so.0 \
			/usr/lib/gbm/dri_gbm.so /usr/lib/dri/libdril_dri.so /usr/lib/libgbm.so.1; do
		[ -f "$l" ] && nm -D --undefined-only "$l" | awk '/libgallium/ {print $2}'
	done | sed 's/@.*//' | sort -u)
	have=$(nm -D --defined-only "$NEW" | awk '{print $3}' | sed 's/@.*//' | sort -u)
	missing=$(printf '%s\n' "$need" | grep -vxF "$have" || true)
	[ -z "$missing" ] || { printf 'the new library lacks:\n%s\n' "$missing" >&2; return 1; }
	# Arch's, kept for this version; those of versions gone are dropped
	# (the one in place is Arch's unless it is the one we installed last)
	ours || cp -a "$LIB" "$LIB.arch"
	for old in /usr/lib/libgallium-*.so.arch; do
		[ "$old" = "$LIB.arch" ] || rm -f "$old"
	done
	install -m755 "$NEW" "$LIB.new"
	mv "$LIB.new" "$LIB"
	mkdir -p "$(dirname "$STATE")"
	sha256sum "$LIB" | cut -d' ' -f1 > "$STATE"
	echo "installed; Arch's library is kept as $LIB.arch"
}

case "${1:-}" in
auto)
	exec >> "$LOG" 2>&1
	echo "== $(date): mesa $PKG"
	if ours; then
		echo "already ours"
		exit 0
	fi
	# each step its own process: inside an `if`, set -e would not stop a step
	if sh "$0" build && sh "$0" install; then
		rm -rf "$SRC"	# (the tarball and its checksum stay, for a rebuild)
		tell "Graphics fix applied" "Mesa $VER is patched for the M20's GPU. Open apps again to use it."
	else
		echo "failed"
		tell "Graphics fix not applied" "Mesa $VER could not be patched: see $LOG."
		exit 1
	fi
	;;
build)
	build
	;;
install)
	install_lib
	;;
restore)
	mv "$LIB.arch" "$LIB"
	rm -f "$STATE"
	echo "restored Arch's library"
	;;
*)
	sed -n 7,10p "$0"
	exit 2
	;;
esac
