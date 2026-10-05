# Android Mia GPU viewer

This NativeActivity packages the same `importScene` → `buildWorldFromAsset` →
`AnimationDriver` → `FilamentRenderer` path used by `viewer_gpu`. It targets
arm64 Android and explicitly selects Filament's Vulkan backend. Drag on the
screen to orbit the camera; the `mixamo.com` animation loops automatically.

The build uses the official Filament v1.77.2 Android libraries and matching
`matc` host tool, and builds the official Assimp v6.0.5 source with only its
glTF importer enabled. The APK includes the relevant third-party license texts;
no third-party source or binary is copied into this repository.

```sh
export MY3D_FILAMENT_ANDROID=/path/to/extracted/filament-v1.77.2-android/filament
export MY3D_FILAMENT_MATC=/path/to/filament-v1.77.2-linux/filament/bin/matc
export MY3D_ASSIMP_SOURCE_DIR=/path/to/assimp-6.0.5
export ANDROID_DEVICE=3d3702ca   # optional; defaults to the connected test phone
android/build_and_install.sh
```

The script uses `$ANDROID_HOME` (default `~/Android/Sdk`) and NDK 27.2.12479018,
builds/signs `build/android/my3d-mia.apk`, installs it, and launches the viewer.
`adb logcat -s my3d-android` reports import, animation, and renderer errors.
