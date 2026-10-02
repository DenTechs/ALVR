# PyroWave foveation development setup

The PyroWave focus stream is optional. The regular ALVR video encoder remains the background stream, and the focus stream is enabled only when both native libraries are available.

## Windows streamer

Build PyroWave from commit `89f7e47d4abbf650c91fae766728af866c5e32a0` with its D3D11/Vulkan interop library. Its 0.6 C API is still changing, so use the same revision for the Windows and Android libraries. Run PyroWave's `checkout_granite.sh` to fetch its pinned Granite dependency. On Windows, build the shared library with:

```powershell
cmake -S . -B build-interop -G "Visual Studio 17 2022" -A x64
cmake --build build-interop --config Release --target pyrowave-shared
```

Set `ALVR_PYROWAVE_DIR` to the PyroWave source checkout before building ALVR. The checkout must include `pyrowave.h`, Granite Vulkan headers under `Granite/third_party/khronos/vulkan-headers/include`, and `build-interop/Release/pyrowave-shared.lib`. The build links against that import library. The xtask packaging step copies `build-interop/Release/libpyrowave-shared-0.dll` beside the ALVR server binaries. The streamer imports the shared D3D11 focus atlas into Vulkan on the same GPU, so the graphics driver must support D3D11/Vulkan external memory interop.

## Android headset

Build the same PyroWave checkout for Android arm64 with the installed NDK. Set `ANDROID_NDK_HOME` to the NDK directory, then place `build-android/libpyrowave-shared.so` in ALVR's `deps/android_openxr/arm64-v8a/` before building the client:

```powershell
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$env:ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DCMAKE_BUILD_TYPE=Release
cmake --build build-android --target pyrowave-shared
```

Builds without this library omit the decoder and do not advertise focus-stream support.

The video settings expose a 4:2:0 or 4:4:4 focus stream. The dashboard bandwidth estimate uses the aligned per-eye crop dimensions, configured bits per pixel, and refresh rate:

`2 × crop width × crop height × bits per pixel × frames per second`

The estimate is in bits per second before conversion to Mbps; background video and transport overhead are additional.
