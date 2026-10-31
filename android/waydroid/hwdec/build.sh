#!/bin/bash
# Android's VP9, H.264 and HEVC decoders on the M20's MFC (ztos_hwdec.cpp),
# built on a computer (Arch Linux, e.g. WSL) for Waydroid's 32-bit OMX service.
#
#   bash build.sh VENDOR_LIB VNDK_LIB SYSTEM_LIB [WORKDIR]   -> WORKDIR/out/lib/...
#
# The 32-bit libraries of the vendor image (lib/), of the system image's VNDK
# (system/apex/com.android.vndk.current/lib/) and of the system image
# (system/lib/): the component links against Android's own
# libstagefright_softomx, foundation, libutils and liblog, and the originals
# it stands in for are copied from VENDOR_LIB, their soname renamed.
#
# Then on the phone, as root: copy WORKDIR/out/. to /vendor/ztos/android-vendor/
# and run /usr/share/ztos-waydroid/install.sh --config-only (patch-vendor.sh
# lays that folder over the vendor image).
set -euo pipefail
FFVER=9.0.2	# the host's FFmpeg: its V4L2 decoders are what Tube uses on the MFC
NDK=android-ndk-r27c
API=33
VLIB=$(realpath "$1")
VNDK=$(realpath "$2")
SLIB=$(realpath "$3")
W=${4:-/root/amfc}
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$W" && cd "$W"

[ -d /root/amesa/$NDK ] && ln -sfn /root/amesa/$NDK $NDK
[ -d $NDK ] || { curl -fL -o ndk.zip https://dl.google.com/android/repository/$NDK-linux.zip; unzip -q ndk.zip; rm ndk.zip; }
bin=$W/$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cc=$bin/armv7a-linux-androideabi$API-clang

# LineageOS 20's headers (Waydroid's images are LineageOS 20): the classes
# this derives from must be laid out exactly as the vendor image's
mkdir -p aosp
sparse() {
	local dir=$1 repo=$2; shift 2
	[ -d aosp/$dir ] || git clone -q --depth 1 --filter=blob:none --sparse -b lineage-20.0 \
		https://github.com/LineageOS/$repo aosp/$dir
	git -C aosp/$dir sparse-checkout set "$@"
}
sparse av android_frameworks_av media/libstagefright/omx/include \
	media/libstagefright/foundation/include media/libstagefright/include media/libmedia/include \
	media/module/foundation/include
sparse native android_frameworks_native headers libs/nativewindow/include \
	libs/nativebase/include libs/arect/include libs/ui/include
sparse core android_system_core libutils/include libcutils/include libsystem/include
sparse logging android_system_logging liblog/include

# FFmpeg: the MFC's V4L2 decoders and the software ones, nothing else
if [ ! -f ffout/lib/libavcodec.a ]; then
	[ -f ffmpeg-$FFVER.tar.xz ] || curl -fL -o ffmpeg-$FFVER.tar.xz https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz
	rm -rf ffmpeg-$FFVER && tar -xf ffmpeg-$FFVER.tar.xz
	(cd ffmpeg-$FFVER && ./configure --prefix="$W/ffout" --target-os=android --arch=arm \
		--cpu=armv7-a --enable-cross-compile --cc=$cc --cxx=$cc++ --ar=$bin/llvm-ar \
		--nm=$bin/llvm-nm --ranlib=$bin/llvm-ranlib --strip=$bin/llvm-strip \
		--enable-pic --enable-static --disable-shared --disable-programs --disable-doc \
		--disable-everything --disable-avdevice --disable-avformat --disable-swscale \
		--disable-swresample --disable-avfilter --disable-network --disable-autodetect \
		--enable-pthreads --enable-neon --enable-v4l2-m2m \
		--enable-decoder=h264_v4l2m2m,vp9_v4l2m2m,hevc_v4l2m2m,h264,vp9,hevc \
		--enable-bsf=vp9_superframe_split --enable-parser=h264,vp9,hevc >/dev/null &&
		make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi

# Android's libraries to link against (only their symbols are used)
mkdir -p link
cp "$VLIB/libstagefright_softomx.so" link/
for l in libstagefright_foundation.so libutils.so; do cp "$VNDK/$l" link/; done
cp "$SLIB/liblog.so" link/

a=aosp
inc=(-I$a/av/media/libstagefright/omx/include -I$a/av/media/libstagefright/foundation/include
	-I$a/av/media/module/foundation/include -I$a/av/media/libstagefright/include
	-I$a/av/media/libmedia/include -I$a/native/headers/media_plugin
	-I$a/native/headers/media_plugin/media/openmax -I$a/native/libs/nativebase/include
	-I$a/native/libs/arect/include -I$a/native/libs/nativewindow/include
	-I$a/native/libs/ui/include -I$a/core/libutils/include -I$a/core/libcutils/include
	-I$a/core/libsystem/include -I$a/logging/liblog/include -Iffout/include)

rm -rf out && mkdir -p out/lib
for lib in vpxdec avcdec hevcdec; do
	sw=${lib%ec}sw	# vpxdec -> vpxdsw: the original's new name, as long as the old
	# AOSP builds without RTTI or exceptions; our C++ runtime is the NDK's,
	# static, as nothing of it crosses into Android's libraries
	$cc++ -std=gnu++17 -O2 -fPIC -shared -fno-rtti -fno-exceptions -fvisibility=hidden \
		-Wall -Wno-unused-parameter -Wno-deprecated-declarations \
		-DZT_SW=$sw -DZT_MAX_HW=3 "${inc[@]}" "$here/ztos_hwdec.cpp" \
		-o out/lib/libstagefright_soft_$lib.so -Wl,-soname,libstagefright_soft_$lib.so \
		-Lffout/lib -lavcodec -lavutil -Llink -lstagefright_softomx -lstagefright_foundation \
		-lutils -llog -static-libstdc++ -lm -ldl -Wl,--no-undefined -Wl,--gc-sections -Wl,--exclude-libs,ALL
	$bin/llvm-strip out/lib/libstagefright_soft_$lib.so
	python3 - "$VLIB/libstagefright_soft_$lib.so" "out/lib/libstagefright_soft_$sw.so" "$lib" "$sw" <<'PY'
import sys
src, dst, old, new = sys.argv[1:]
data = open(src, 'rb').read()
o = b'libstagefright_soft_%s.so\0' % old.encode()
n = b'libstagefright_soft_%s.so\0' % new.encode()
assert len(o) == len(n) and data.count(o) >= 1, 'soname not found'
open(dst, 'wb').write(data.replace(o, n))
PY
done
ls -la out/lib
