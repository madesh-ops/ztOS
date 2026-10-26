#!/bin/bash
# Android's Mesa for Waydroid on the Galaxy M20, with the G71 fix
# (pan-bi-g71-derivatives.diff), built on a computer (Arch Linux, e.g. WSL).
#
# Waydroid's vendor image has its own Mesa (26.0.8 in the 20260927 image),
# unpatched: on the M20's G71 r0p0 Subway Surfers' fragment jobs faulted
# (IMPRECISE_FAULT, INSTR_INVALID_ENC in dmesg, ~1250 a run) and left blocks
# of old frames on screen. This builds the host's version (26.2.5) for
# Android, the whole set Waydroid links together (libgallium_dri, EGL, GLES,
# gbm and its dri backend), named as the vendor image names them. With it:
# no fault in a run, no stale blocks.
#
#   bash build-android-mesa.sh [WORKDIR]     -> WORKDIR/out/lib64/...
#
# Then on the phone, as root: copy WORKDIR/out/. to /vendor/ztos/android-vendor/
# and run /usr/share/ztos-waydroid/install.sh --config-only (patch-vendor.sh
# lays that folder over the vendor image).
set -euo pipefail
VER=26.2.5
SHA=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5	# Arch's PKGBUILD
NDK=android-ndk-r27c
API=33
W=${1:-/root/amesa}
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$W" && cd "$W"

pacman -S --needed --noconfirm meson ninja unzip patchelf python-mako python-yaml \
	python-packaging glslang llvm clang libclc spirv-llvm-translator spirv-tools spirv-headers

[ -d $NDK ] || { curl -fL -o ndk.zip https://dl.google.com/android/repository/$NDK-linux.zip; unzip -q ndk.zip; rm ndk.zip; }
[ -f mesa-$VER.tar.xz ] || curl -fL -o mesa-$VER.tar.xz https://archive.mesa3d.org/mesa-$VER.tar.xz
echo "$SHA  mesa-$VER.tar.xz" | sha256sum -c --quiet
rm -rf mesa-$VER && tar -xf mesa-$VER.tar.xz
sed 's/\r$//' "$here/pan-bi-g71-derivatives.diff" | patch -d mesa-$VER -p1 -s

# Panfrost's internal kernels are OpenCL, compiled at build time by mesa_clc
# and panfrost_compile: built here for the computer, from the same source.
cd mesa-$VER
meson setup native --buildtype=release -Dgallium-drivers=panfrost -Dvulkan-drivers= \
	-Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
	-Dopengl=false -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dbuild-tests=false \
	-Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled
ninja -C native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
mkdir -p "$W/tools"
cp native/src/compiler/clc/mesa_clc native/src/compiler/spirv/vtn_bindgen2 \
	native/src/panfrost/clc/panfrost_compile "$W/tools/"
export PATH=$W/tools:$PATH

bin=$W/$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
cat > ../android-aarch64.ini <<EOF
[binaries]
c = '$bin/aarch64-linux-android$API-clang'
cpp = '$bin/aarch64-linux-android$API-clang++'
ar = '$bin/llvm-ar'
strip = '$bin/llvm-strip'
c_ld = 'lld'
cpp_ld = 'lld'
pkg-config = '/bin/false'

[built-in options]
c_args = ['-O2', '-g0']
cpp_args = ['-O2', '-g0']
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF
# Android's own libraries (cutils, log, sync, nativewindow, hardware) through
# Mesa's stubs; libdrm built for linking only (the vendor image has its own).
meson setup build --cross-file ../android-aarch64.ini --buildtype=release \
	-Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true \
	-Dandroid-libbacktrace=disabled -Dandroid-strict=false \
	-Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= -Dllvm=disabled \
	-Dmesa-clc=system -Dprecomp-compiler=system \
	-Degl=enabled -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled -Dglx=disabled -Dopengl=true \
	-Dxmlconfig=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
	-Dlmsensors=disabled -Dexpat=disabled -Dbuild-tests=false -Dperfetto=false \
	-Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa -Dshader-cache=enabled \
	-Dgbm-backends-path=/vendor/lib64 -Dallow-fallback-for=libdrm,perfetto
ninja -C build

O=$W/out/lib64
rm -rf "$W/out" && mkdir -p $O/egl
cp build/src/gallium/targets/dri/libgallium_dri.so build/src/gbm/libgbm_mesa.so \
	build/src/gbm/backends/dri/dri_gbm.so $O/
cp build/src/egl/libEGL_mesa.so build/src/mesa/glapi/es2api/libGLESv2_mesa.so \
	build/src/mesa/glapi/es1api/libGLESv1_CM_mesa.so $O/egl/
for f in $O/*.so $O/egl/*.so; do patchelf --remove-rpath "$f"; done
# the sonames the vendor image's copies have
patchelf --set-soname libEGL.so $O/egl/libEGL_mesa.so
patchelf --set-soname libGLESv2.so $O/egl/libGLESv2_mesa.so
patchelf --set-soname libGLESv1_CM.so $O/egl/libGLESv1_CM_mesa.so
"$bin/llvm-strip" --strip-unneeded $O/*.so $O/egl/*.so
ls -la $O $O/egl
