#!/bin/bash
set -e

BUILD_DIR="${1:-${BUILD_DIR:-build}}"

# 🚀 0. CORREGIR REPOSITORIOS DE UBUNTU BIONIC (EOL -> OLD-RELEASES)
echo "-> Actualizando fuentes de APT para Ubuntu Bionic (Old-Releases)..."
sed -i 's/archive.ubuntu.com/old-releases.ubuntu.com/g' /etc/apt/sources.list 2>/dev/null || true
sed -i 's/security.ubuntu.com/old-releases.ubuntu.com/g' /etc/apt/sources.list 2>/dev/null || true
sed -i '/deb-src/d' /etc/apt/sources.list 2>/dev/null || true

# 🚀 1. INSTALAR DEPENDENCIAS BÁSICAS Y DE DESARROLLO DEL SISTEMA
echo "-> Verificando e instalando dependencias de desarrollo necesarias..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --allow-change-held-packages \
    git \
    ninja-build \
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
    build-essential

# 🚀 2. CONFIGURACIÓN EXHAUSTIVA DE PKG_CONFIG_PATH
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1
export PKG_CONFIG_ALLOW_SYSTEM_LIBS=1

echo "-> PKG_CONFIG_PATH configurado como: $PKG_CONFIG_PATH"

# 🚀 3. COMPILAR E INSTALAR LIBDRM DESDE EL REPO DE WAYDROID (LINEAGE-20)
echo "-> Clonando y compilando libdrm desde Waydroid (lineage-20)..."
rm -rf /tmp/android_external_libdrm
git clone --depth 1 https://github.com/waydroid/android_external_libdrm -b lineage-20 /tmp/android_external_libdrm
cd /tmp/android_external_libdrm
rm -rf build_libdrm
meson setup build_libdrm --prefix=/usr/local -Dvalgrind=disabled -Dudev=false
ninja -C build_libdrm install
cd - > /dev/null

# 🚀 4. EXPORTAR PYTHONPATH PARA LOS SCRIPTS DE NIR/COMPILER DE MESA
export PYTHONPATH="/workspace/src/compiler:/workspace/src/compiler/nir:$PYTHONPATH"

# 🚀 5. COMPILAR LOS STUBS DE ANDROID EN UN DIRECTORIO TEMPORAL PARA SATISFACER A MESON
echo "-> Compilando librerías de stubs de Android (libandroid, liblog, libcutils)..."
mkdir -p /tmp/android_build_stubs

# Código fuente rápido para libandroid_stub.c
cat << 'EOF' > /tmp/android_build_stubs/android_stub.c
extern "C" {
void AConfiguration_fromAssetManager() {}
void AAssetManager_fromJava() {}
}
EOF

# Código fuente rápido para liblog_stub.c
cat << 'EOF' > /tmp/android_build_stubs/log_stub.c
extern "C" {
int __android_log_print(int, const char*, const char*, ...) { return 0; }
void __android_log_write() {}
void __android_log_assert() {}
}
EOF

# Código fuente rápido para libcutils_stub.cpp
cat << 'EOF' > /tmp/android_build_stubs/cutils_stub.cpp
#include <stdint.h>
extern "C" {
int property_get(const char*, char *value, const char *default_value) {
   if (default_value) {
       int i = 0;
       while (default_value[i] != '\0' && i < 91) { value[i] = default_value[i]; i++; }
       value[i] = '\0';
       return i;
   }
   return 0;
}
void atrace_begin_body(const char*) {}
void atrace_end_body() {}
void atrace_init() {}
uint64_t atrace_get_enabled_tags() { return 0; }
}
EOF

g++ -shared -fPIC /tmp/android_build_stubs/android_stub.c -o /tmp/android_build_stubs/libandroid.so
g++ -shared -fPIC /tmp/android_build_stubs/log_stub.c -o /tmp/android_build_stubs/liblog.so
g++ -shared -fPIC -I/workspace/include/android_stub /tmp/android_build_stubs/cutils_stub.cpp -o /tmp/android_build_stubs/libcutils.so

# 🚀 6. CONFIGURAR RUTAS HACIA LOS STUBS DEL REPO Y LAS LIBRERÍAS COMPILADAS
export LDFLAGS="-L/tmp/android_build_stubs -L/workspace/src/android_stub $LDFLAGS"
export CFLAGS="-I/workspace/include/android_stub $CFLAGS"
export CXXFLAGS="-I/workspace/include/android_stub -D_GNU_SOURCE -D__ANDROID__ -D__aarch64__ $CXXFLAGS"

# 🛠️ 7. LIMPIAR DIRECTORIO DE COMPILACIÓN ANTERIOR DE MESA
rm -rf "${BUILD_DIR}"

# 🟢 8. CONFIGURACIÓN CON MESON (Nativo para Linux X11 / Winlator Bionic con soporte completo)
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

# 🚀 9. PARCHE AUTOMÁTICO DE SEGURIDAD Y COMPATIBILIDAD
echo "-> Aplicando parches de compatibilidad..."
ADRENOTOOLS_FILE="subprojects/libadrenotools/lib/linkernsbypass/android_linker_ns.cpp"
if [ -f "$ADRENOTOOLS_FILE" ]; then
    sed -i '1i #include <climits>\n#include <sys/syscall.h>\n#include <unistd.h>' "$ADRENOTOOLS_FILE"
fi

KGSL_HEADER="subprojects/libadrenotools/src/hook/kgsl.h"
if [ -f "$KGSL_HEADER" ]; then
    sed -i '1i #ifndef __user\n#define __user\n#endif' "$KGSL_HEADER"
fi

DLEXT_HEADER="include/android_stub/android/dlext.h"
if [ -f "$DLEXT_HEADER" ]; then
    sed -i 's/void\* library_namespace;/struct android_namespace_t\* library_namespace;/g' "$DLEXT_HEADER"
fi

# Parche para evitar conflicto de C11 threads con glibc de Bionic
C11_HEADER="src/c11/threads.h"
if [ -f "$C11_HEADER" ]; then
    sed -i 's/typedef pthread_once_t once_flag;/#ifndef __USE_ISOC11\ntypedef pthread_once_t once_flag;\n#endif/' "$C11_HEADER"
    sed -i 's/void call_once(once_flag \*, void (\*)(void));/#ifndef __USE_ISOC11\nvoid call_once(once_flag *, void (*)(void));\n#endif/' "$C11_HEADER"
fi

# ⚙️ 10. COMPILACIÓN CON NINJA
echo "-> Ejecutando compilación del wrapper Vulkan y el puente Mali..."
ninja -C "${BUILD_DIR}"

# 🟢 11. EXTRACCIÓN DEL BINARIO DEL WRAPPER
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

# 🟢 12. STRIP DE SÍMBOLOS
strip --strip-unneeded -o "${BUILD_DIR}/libvulkan_wrapper.so" "${BUILD_DIR}/libvulkan_wrapper.so.unstripped"

# 🟢 13. ÁRBOL DE DIRECTORIOS RÍGIDO CON ICD RELATIVO (Formato Bannerlator)
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

# 🟢 14. EMPAQUETADO TAR.ZST PARA BANNERLATOR
echo "-> Forjando el paquete comprimido gamenative_driver_rootfs.tar.zst..."
cd "${ROOTFS_DIR}"
tar -cvf - usr | zstd -o "../gamenative_driver_rootfs.tar.zst"
cd - > /dev/null

cp "${BUILD_DIR}/gamenative_driver_rootfs.tar.zst" ./gamenative_driver_rootfs.tar.zst

echo "=========================================================="
echo " 🟢 FORJA WINLATOR BIONIC EXITOSA: ¡ROOTFS LISTO!          "
echo "=========================================================="
