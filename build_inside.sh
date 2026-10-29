#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

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
NDK_DIR=$(find /home/builder/lib -maxdepth 2 -name "android-ndk*" 2>/dev/null | head -n 1)
if [ -z "$NDK_DIR" ]; then
    NDK_DIR=$(find /opt /usr/local /home -maxdepth 3 -name "android-ndk*" 2>/dev/null | head -n 1)
fi
NDK_BIN="${NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"

mkdir -p /root/build-config
cat << EOF > /root/build-config/cross_file.txt
[binaries]
c = '${NDK_BIN}/aarch64-linux-android30-clang'
cpp = '${NDK_BIN}/aarch64-linux-android30-clang++'
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
cpu_family = 'aarch64'
cpu = 'armv8-a'
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
cp "${BUILD_DIR}/src/vulkan/wrapper/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"
STRIP="${NDK_BIN}/llvm-strip"
$STRIP --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🚀 6. EMPAQUETADO FINAL EN ROOTFS PARA BANNERLATOR / WINLATOR
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib"
mkdir -p "${ROOTFS_DIR}/usr/share/vulkan/icd.d"

cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/libvulkan_wrapper.so"

# Copiar librerías de dependencias compartidas necesarias
find "${BUILD_DIR}" -name "*.so" -exec cp {} "${ROOTFS_DIR}/usr/lib/" \; 2>/dev/null || true

cat << 'EOF' > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

echo "-> Empaquetando rootfs comprimido en tar.zst..."
cd "${ROOTFS_DIR}"
tar -cvf - usr | zstd -o "../gamenative_driver_rootfs.tar.zst"
cd - > /dev/null

cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 BUILD EXITOSO: gamenative_driver_rootfs.tar.zst LISTO!"
echo "=========================================================="
