#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
if [[ ! -x "$SDK/build-tools/35.0.0/aapt" &&
      -x "$HOME/Android/Sdk/build-tools/35.0.0/aapt" ]]; then
    SDK="$HOME/Android/Sdk"
fi
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}"
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" &&
      -f "$SDK/ndk/27.2.12479018/build/cmake/android.toolchain.cmake" ]]; then
    NDK="$SDK/ndk/27.2.12479018"
fi
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$SDK/build-tools/35.0.0}"
FILAMENT_ROOT="${MY3D_FILAMENT_ANDROID:-}"
ASSIMP_SOURCE="${MY3D_ASSIMP_SOURCE_DIR:-}"
MATC="${MY3D_FILAMENT_MATC:-}"
DEVICE="${ANDROID_DEVICE:-3d3702ca}"
BUILD_DIR="$ROOT/build/android"
ASSET_STAGE="$BUILD_DIR/package-assets"

if [[ -z "$FILAMENT_ROOT" || -z "$ASSIMP_SOURCE" || -z "$MATC" ]]; then
    echo "Set MY3D_FILAMENT_ANDROID, MY3D_ASSIMP_SOURCE_DIR and MY3D_FILAMENT_MATC." >&2
    exit 2
fi
for tool in cmake adb python3 keytool; do
    command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 2; }
done
for tool in aapt zipalign apksigner; do
    [[ -x "$BUILD_TOOLS/$tool" ]] || { echo "Missing Android build tool: $BUILD_TOOLS/$tool" >&2; exit 2; }
done
[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "NDK not found: $NDK" >&2; exit 2; }
[[ -f "$SDK/platforms/android-35/android.jar" ]] || { echo "Android platform 35 is not installed." >&2; exit 2; }

mkdir -p "$BUILD_DIR" "$ASSET_STAGE/assets/model" \
    "$ASSET_STAGE/assets/shader/android/vulkan" "$ASSET_STAGE/assets/licenses"
cp -a "$ROOT/assets/model/mia" "$ASSET_STAGE/assets/model/"
cp "$ROOT/libs/filament/LICENSE" "$ASSET_STAGE/assets/licenses/Filament.txt"
cp "$ASSIMP_SOURCE/LICENSE" "$ASSET_STAGE/assets/licenses/Assimp.txt"
for material in unlit lit lit_aorm; do
    "$MATC" -a vulkan -p mobile \
        -o "$ASSET_STAGE/assets/shader/android/vulkan/$material.filamat" \
        "$ROOT/src/mat/$material.mat"
done

cmake -S "$ROOT/android" -B "$BUILD_DIR/cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_STL=c++_shared \
    -DMY3D_ROOT="$ROOT" \
    -DMY3D_FILAMENT_ROOT="$FILAMENT_ROOT" \
    -DMY3D_ASSIMP_SOURCE_DIR="$ASSIMP_SOURCE" \
    -DCMAKE_BUILD_TYPE=Release
if ! cmake --build "$BUILD_DIR/cmake" --target my3d_android --parallel \
    >"$BUILD_DIR/build.log" 2>&1; then
    tail -80 "$BUILD_DIR/build.log" >&2
    exit 1
fi

"$BUILD_TOOLS/aapt" package -f \
    -M "$ROOT/android/AndroidManifest.xml" \
    -I "$SDK/platforms/android-35/android.jar" \
    -A "$ASSET_STAGE" \
    -F "$BUILD_DIR/base.apk"

python3 "$ROOT/android/package_native_libs.py" \
    "$BUILD_DIR/base.apk" "$BUILD_DIR/cmake/libmy3d_android.so" \
    "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"

"$BUILD_TOOLS/zipalign" -f -p 4 "$BUILD_DIR/base.apk" "$BUILD_DIR/aligned.apk"
KEYSTORE="$BUILD_DIR/my3d-debug.keystore"
if [[ ! -f "$KEYSTORE" ]]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -dname "CN=my3d Android Debug,O=my3d,C=CN" \
        -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --key-pass pass:android --out "$BUILD_DIR/my3d-mia.apk" "$BUILD_DIR/aligned.apk"
"$BUILD_TOOLS/apksigner" verify "$BUILD_DIR/my3d-mia.apk"

adb -s "$DEVICE" install -r "$BUILD_DIR/my3d-mia.apk"
adb -s "$DEVICE" shell am force-stop com.printf033.my3d
adb -s "$DEVICE" shell am start -n com.printf033.my3d/android.app.NativeActivity
echo "Installed and launched on $DEVICE: $BUILD_DIR/my3d-mia.apk"
