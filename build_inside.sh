#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 1. INSTALAR TODAS LAS DEPENDENCIAS DE DESARROLLO EN EL CONTENEDOR DE GITHUB ACTIONS
echo "-> Verificando e instalando dependencias de desarrollo necesarias..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq || true
apt-get install -y -qq \
    libdrm-dev \
    libx11-dev \
    libxcb-randr0-dev \
    libxfixes-dev \
    libx11-xcb-dev \
    libxcb-dri3-dev \
    libxcb-present-dev \
    libxshmfence-dev \
    pkg-config \
    zlib1g-dev \
    libzstd-dev \
    build-essential || true

# 🚀 2. CONFIGURAR PKG_CONFIG_PATH PARA QUE MESON ENCUENTRE LIBDRM Y X11
export PKG_CONFIG_PATH="/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:/usr/local/lib/pkgconfig"

# 🚀 3. EXPORTAR PYTHONPATH PARA LOS SCRIPTS DE NIR/COMPILER DE MESA
export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# 🚀 4. CREAR EL STUB (SIMULADOR) DE LIBANDROID PARA QUE LIBADRENOTOOLS COMPILE PERFECTAMENTE
echo "-> Forjando stub temporal de libandroid para libadrenotools..."
mkdir -p /tmp/android_stub
cat << 'EOF' > /tmp/android_stub/android_stub.c
void AConfiguration_fromAssetManager() {}
void AAssetManager_fromJava() {}
EOF
gcc -shared -fPIC /tmp/android_stub/android_stub.c -o /tmp/android_stub/libandroid.so
export LDFLAGS="-L/tmp/android_stub $LDFLAGS"
export CFLAGS="-I/tmp/android_stub $CFLAGS"

# 🛠️ 5. LIMPIAR DIRECTORIO DE COMPILACIÓN ANTERIOR
rm -rf "${BUILD_DIR}"

# 🟢 6. CONFIGURACIÓN CON MESON (Nativo para Linux X11 / Winlator Bionic con soporte completo)
meson subprojects download || true

meson setup "${BUILD_DIR}" \
-Db_sanitize=none \
-Db_lundef=true \
-Dcpp_rtti=false \
-Dopengl=true \
-Dllvm=disabled \
-Dshared-llvm=disabled \
-Dplatforms=x11 \
-Dgallium-drivers= \
-Ddraw-use-llvm=false \
-Dxmlconfig=disabled \
-Dvulkan-drivers=wrapper \
-Degl=disabled \
-Dglx=disabled \
-Dzlib=enabled \
-Dzstd=enabled

# ⚙️ 7. COMPILACIÓN CON NINJA
echo "-> Ejecutando compilación del wrapper Vulkan y el puente Mali..."
ninja -C "${BUILD_DIR}"

# 🟢 8. EXTRACCIÓN DEL BINARIO DEL WRAPPER
python3 -c '
import os, shutil

build_dir = "'"${BUILD_DIR}"'"
dst = os.path.join(build_dir, "libvulkan_wrapper.so.unstripped")

candidates = []
for root, dirs, files in os.walk(build_dir):
    for file in files:
        if file.endswith(".so") and not any(x in file for x in ["expat", "glapi", "test"]):
            candidates.append(os.path.join(root, file))

src = None
for c in candidates:
    if "wrapper" in c or "vulkan" in c:
        src = c
        break

if not src and candidates:
    src = candidates[0]

if src and os.path.exists(src):
    shutil.copy2(src, dst)
    print(f"-> [Forja Real] Driver wrapper localizado en {src}")
else:
    print("-> [❌ ERROR CRÍTICO] No se encontró el binario del wrapper.")
    exit(1)
'

# 🟢 9. STRIP DE SÍMBOLOS
strip --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🟢 10. ÁRBOL DE DIRECTORIOS RÍGIDO CON ICD RELATIVO (Formato Bannerlator)
ROOTFS_DIR="${BUILD_DIR}/rootfs_export"
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/lib"
mkdir -p "${ROOTFS_DIR}/usr/share/vulkan/icd.d"

cp "${BUILD_DIR}/libvulkan_wrapper.so" "${ROOTFS_DIR}/usr/lib/libvulkan_wrapper.so"

python3 -c '
import os, shutil
build_dir = "'"${BUILD_DIR}"'"
rootfs_lib = "'"${ROOTFS_DIR}"'/usr/lib"
for root, dirs, files in os.walk(os.path.join(build_dir, "subprojects")):
    for file in files:
        if file.endswith(".so"):
            shutil.copy2(os.path.join(root, file), os.path.join(rootfs_lib, file))
' 2>/dev/null || true

cat << 'EOF' > "${ROOTFS_DIR}/usr/share/vulkan/icd.d/wrapper_icd.json"
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_wrapper.so",
        "api_version": "1.3.255"
    }
}
EOF

# 🟢 11. EMPAQUETADO TAR.ZST PARA BANNERLATOR
echo "-> Forjando el paquete comprimido gamenative_driver_rootfs.tar.zst..."
cd "${ROOTFS_DIR}"
tar -cvf - usr | zstd -o "../gamenative_driver_rootfs.tar.zst"
cd - > /dev/null

cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 FORJA WINLATOR BIONIC EXITOSA: ¡ROOTFS LISTO!          "
echo "=========================================================="
