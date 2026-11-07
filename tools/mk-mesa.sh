#!/bin/bash
# Mesa for the iOS userland on ginkgo: OpenGL ES through freedreno on the Adreno 610, the kernel's
# SweetGPU service standing in for Linux's DRM (tools/mesa/redwood-drm). Built on a macOS runner
# (.github/workflows/mesa.yml) with Xcode's clang and iPhoneOS SDK; nothing of Mesa is in the
# repository. Adapted from willow-hackios's tools/mk-mesa.sh (MIT), whose tools/mesa patches are
# used unchanged: freedreno on Darwin, the OSMesa ES profile, three ir3 fixes.
#
#   DRIVERS=softpipe,freedreno   the gallium drivers (softpipe: plain C, no GPU)
#   OUT=dir                      where the results go (default ./mesa-out)
#
# Results: libOSMesa.8.dylib (install name /var/sweet/lib/libOSMesa.8.dylib), exported-gl.txt, gl.xml.
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${BUILD_DIR:-$PWD/mesa-build}
OUT=${OUT:-$PWD/mesa-out}
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC=$(xcrun -sdk iphoneos -f clang)
mkdir -p "$B" "$OUT"
cd "$B"

if [ ! -d mesa-$VERSION ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/*.patch; do
		echo "patch: $(basename "$p")"
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
	done
fi
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm

cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0']
ar = '$(xcrun -f ar)'
ranlib = '$(xcrun -f ranlib)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/var/sweet --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false
ninja -C $BUILD

xcrun strip -x -o "$OUT/libOSMesa.8.dylib" $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /var/sweet/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
codesign -f -s - "$OUT/libOSMesa.8.dylib"
nm -gU "$OUT/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OUT/exported-gl.txt"
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml "$OUT/gl.xml"
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $(stat -f %z "$OUT/libOSMesa.8.dylib") bytes, $(wc -l < "$OUT/exported-gl.txt") gl* functions"
