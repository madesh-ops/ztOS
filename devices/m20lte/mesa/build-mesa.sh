#!/bin/sh
# Rebuild the Galaxy M20's libgallium with pan-bi-g71-derivatives.diff, on the
# phone itself, and install it over Arch Linux ARM's (backed up first).
#
#   sh build-mesa.sh build      # download, patch, configure, build (~1.5 h)
#   sh build-mesa.sh install    # check it, then replace the system library
#   sh build-mesa.sh restore    # put Arch's library back
#
# Only libgallium changes. It is built to match the installed mesa package:
# the same version string (Arch's VERSION, hence the same library name and
# symbol versions), GLX/X11/Wayland, and the drivers this phone can use
# (panfrost, llvmpipe, softpipe, zink); `install` refuses if it lacks a symbol
# the installed EGL, GLX, GBM or DRI loaders import. A mesa update from pacman
# overwrites it: rebuild for the new version (and refresh the diff).
set -e
VER=26.2.5
ARCHVER=$VER-arch1.1
SHA=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5
HERE=$(cd "$(dirname "$0")" && pwd)
W=/var/tmp/mesa-m20
SRC=$W/mesa-$VER
LIB=/usr/lib/libgallium-$ARCHVER.so
NEW=$SRC/build-arch/src/gallium/targets/dri/libgallium-$ARCHVER.so

case "$1" in
build)
	[ "$(pacman -Q mesa | cut -d' ' -f2)" = "1:$VER-1" ] ||
		{ echo "installed mesa is not $VER: update VER, ARCHVER, SHA and the diff" >&2; exit 1; }
	sudo pacman -S --needed --noconfirm meson ninja python-mako python-yaml \
		python-packaging python-pyelftools
	mkdir -p $W && cd $W
	[ -f mesa-$VER.tar.xz ] || curl -fL -o mesa-$VER.tar.xz https://archive.mesa3d.org/mesa-$VER.tar.xz
	echo "$SHA  mesa-$VER.tar.xz" | sha256sum -c -
	rm -rf $SRC && tar -xf mesa-$VER.tar.xz
	cd $SRC
	patch -p1 < "$HERE/pan-bi-g71-derivatives.diff"
	echo $ARCHVER > VERSION
	meson setup build-arch --prefix=/usr --libdir=lib -Dbuildtype=plain \
		--optimization 2 -Db_ndebug=true \
		-Dgallium-drivers=panfrost,llvmpipe,softpipe,zink -Dvulkan-drivers= \
		-Dplatforms=x11,wayland -Dglx=dri -Degl=enabled -Dgbm=enabled \
		-Dgles1=disabled -Dgles2=enabled -Dvideo-codecs= -Dgallium-va=disabled \
		-Dgallium-extra-hud=true -Dvalgrind=disabled -Dlibunwind=disabled -Dtools=
	ninja -C build-arch
	;;
install)
	[ -f "$NEW" ] || { echo "no $NEW: run build first" >&2; exit 1; }
	need=$(for l in /usr/lib/libEGL_mesa.so.0 /usr/lib/libGLX_mesa.so.0 \
			/usr/lib/gbm/dri_gbm.so /usr/lib/dri/libdril_dri.so /usr/lib/libgbm.so.1; do
		nm -D --undefined-only "$l" | awk '/libgallium/ {print $2}'
	done | sed 's/@.*//' | sort -u)
	have=$(nm -D --defined-only "$NEW" | awk '{print $3}' | sed 's/@.*//' | sort -u)
	missing=$(printf '%s\n' "$need" | grep -vxF "$have" || true)
	[ -z "$missing" ] || { printf 'the new library lacks:\n%s\n' "$missing" >&2; exit 1; }
	[ -f "$LIB.arch" ] || sudo cp -a "$LIB" "$LIB.arch"
	sudo install -m755 "$NEW" "$LIB.new"
	sudo mv "$LIB.new" "$LIB"
	echo "installed; Arch's library is kept as $LIB.arch"
	;;
restore)
	sudo mv "$LIB.arch" "$LIB"
	echo "restored Arch's library"
	;;
*)
	sed -n 2,8p "$0"
	exit 2
	;;
esac
