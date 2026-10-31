#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"
ARCH="${ARCH:-aarch64}"

export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# ---------- NDK ----------
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
[ -z "$NDK_DIR" ] && NDK_DIR="/home/builder/lib/android-ndk-r30"
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
NDK_SYSROOT="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

if [ "$ARCH" = "arm" ]; then
    TRIPLE="armv7a-linux-androideabi30"
    SYSLIB="arm-linux-androideabi"
    CPU_FAMILY="arm"; CPU="armv7"
    LIBDIR_TAG="armhf"
else
    TRIPLE="aarch64-linux-android30"
    SYSLIB="aarch64-linux-android"
    CPU_FAMILY="aarch64"; CPU="armv8-a"
    LIBDIR_TAG="aarch64"
fi

# 🚀 1. PREPARAR SYSROOT DE TERMUX (Si no existe)
if [ ! -d "/tmp/sysroot_ready" ]; then
    echo "-> Descargando librerías y dependencias desde el repositorio de Termux..."
    mkdir -p /tmp/sysroot
    cd /tmp/sysroot
    TERMUX_REPO="https://packages-cf.termux.dev/apt/termux-main"
    curl -s "${TERMUX_REPO}/dists/stable/main/binary-aarch64/Packages" > Packages
    PACKAGES="libdrm libandroid-shmem libxcb libx11 libxshmfence libxext libxrandr libxrender xorgproto libxau libxdmcp"
    for pkg in $PACKAGES; do
        pkg_path=$(awk -v p="Package: $pkg" '$0==p{flag=1} flag && /^Filename:/{print $2; exit}' Packages)
        if [ -n "$pkg_path" ]; then
            curl -L -s -O "${TERMUX_REPO}/${pkg_path}"
        fi
    done
    mkdir -p /data/data/com.termux/files/usr
    for f in *.deb; do
        if [ -f "$f" ]; then
            dpkg-deb -x "$f" /
        fi
    done
    touch /tmp/sysroot_ready
    cd - > /dev/null
fi

# 🚀 2. GENERAR ARCHIVO DE CROSS-COMPILACIÓN PARA EL NDK
mkdir -p /root/build-config
cat << EOF > /root/build-config/cross_file.txt
[binaries]
c = '${NDK_BIN}/${TRIPLE}-clang'
cpp = '${NDK_BIN}/${TRIPLE}-clang++'
ar = '${NDK_BIN}/llvm-ar'
strip = '${NDK_BIN}/llvm-strip'
pkg-config = 'pkg-config'

[constants]
termux_dir = '/data/data/com.termux/files/usr'

[properties]
pkg_config_libdir = termux_dir + '/lib/pkgconfig:' + termux_dir + '/share/pkgconfig'

[built-in options]
c_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I' + termux_dir + '/include', '-include', 'fcntl.h', '-include', 'unistd.h']
cpp_args = ['-D__TERMUX__', '-D__USE_GNU', '-U__ANDROID__', '-I' + termux_dir + '/include', '-include', 'fcntl.h', '-include', 'unistd.h']
c_link_args = ['-L' + termux_dir + '/lib', '-landroid-shmem']
cpp_link_args = ['-L' + termux_dir + '/lib', '-landroid-shmem']

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'
EOF

# 🚀 3. CONFIGURACIÓN CON MESON
if [ ! -d "${BUILD_DIR}" ]; then
  echo "-> Configurando proyecto con Meson (Cross-compilación NDK)..."
  meson setup "${BUILD_DIR}" --cross-file /root/build-config/cross_file.txt \
      -Dcpp_rtti=false \
      -Dgbm=disabled \
      -Dopengl=false \
      -Dllvm=disabled \
      -Dshared-llvm=disabled \
      -Dplatforms=x11 \
      -Dgallium-drivers= \
      -Dxmlconfig=disabled \
      -Dvulkan-drivers=wrapper
fi

# 🚀 4. COMPILACIÓN CON NINJA
echo "-> Compilando con Ninja..."
ninja -C "${BUILD_DIR}" src/vulkan/wrapper/libvulkan_wrapper.so

# 🚀 5. POST-PROCESAMIENTO Y STRIP
WRAPPER_SRC="${BUILD_DIR}/src/vulkan/wrapper/libvulkan_wrapper.so"
if [ ! -f "$WRAPPER_SRC" ]; then
    echo "-> [❌ ERROR GRAVE] El binario libvulkan_wrapper.so no se encuentra en ${BUILD_DIR}/."
    exit 1
fi

cp "$WRAPPER_SRC" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"
STRIP="${NDK_BIN}/llvm-strip"
$STRIP --strip-unneeded -o "$WRAPPER_SRC" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🚀 6. EMPAQUETADO FINAL IDÉNTICO AL YAML (Adrenotools + Manifestos ICD duales)
echo "-> [Host] Estructurando paquete de producción idéntico al CI..."
FINAL_PKG="final_pkg"
rm -rf "$FINAL_PKG"
mkdir -p "$FINAL_PKG/usr/lib" "$FINAL_PKG/usr/share/vulkan/icd.d"

# Copiar wrapper principal
cp -fv "$WRAPPER_SRC" "$FINAL_PKG/usr/lib/libvulkan_wrapper.so"

# Regla de comodines de Adrenotools y dependencias compartidas
cp -fv ./build/subprojects/libadrenotools/*.so ./build/subprojects/libadrenotools/src/hook/*.so "$FINAL_PKG/usr/lib/" 2>/dev/null || true
find "${BUILD_DIR}" -name "*.so" -not -path "*/CMakeFiles/*" -not -path "*/test/*" -exec cp -fv {} "$FINAL_PKG/usr/lib/" \; 2>/dev/null || true

chmod -R 755 "$FINAL_PKG/usr/lib/"
patchelf --set-soname libvulkan_wrapper.so "$FINAL_PKG/usr/lib/libvulkan_wrapper.so" || true
find "$FINAL_PKG/usr/lib/" -name "*.so" -exec patchelf --set-rpath '$ORIGIN' {} \; 2>/dev/null || true

# Manifiestos ICD duales compatibles con la lista blanca
echo '{"ICD": {"api_version": "1.3.289", "library_path": "libvulkan_wrapper.so"}, "file_format_version": "1.0.0"}' > "$FINAL_PKG/usr/share/vulkan/icd.d/wrapper_icd.aarch64.json"
echo '{"ICD": {"api_version": "1.3.289", "library_path": "libvulkan_wrapper.so"}, "file_format_version": "1.0.0"}' > "$FINAL_PKG/usr/share/vulkan/icd.d/wrapper_icd.arm.json"

sync && sleep 3
cd "$FINAL_PKG"
tar -I 'zstd -19 -T0' -cf ../wrapper.tzst usr/
cd ..

cp wrapper.tzst ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 BUILD EXITOSO: gamenative_driver_rootfs.tar.zst Y wrapper.tzst LISTOS!"
echo "=========================================================="
