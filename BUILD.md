# Build Azahar Multisplit

The Windows release embeds a patched Azahar Libretro engine as PE resource 101. The frontend loads a separate engine instance for each selected player.

## Engine source

Use Azahar **2126.2**, commit [`219d743a1e698101fe1ab8bef43e6563126c3c27`](https://github.com/azahar-emu/azahar/tree/219d743a1e698101fe1ab8bef43e6563126c3c27), including its recursive submodules. From this repository’s root:

```sh
git clone --branch 2126.2 --recurse-submodules https://github.com/azahar-emu/azahar.git azahar
git -C azahar apply ../azahar-2126.2-multisplit.patch
```

## Windows x64

Use CMake, Ninja and an x86-64 LLVM-MinGW toolchain with the target C/C++ and resource compilers. The commands below use a Windows-hosted x86-64 LLVM-MinGW installation on PATH. On a cross-build host, pass the same `-DCMAKE_TOOLCHAIN_FILE=...` to both configure commands.

```sh
cmake -S azahar -B build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres -DENABLE_LIBRETRO=ON -DENABLE_VULKAN=ON -DENABLE_OPENGL=ON -DENABLE_QT=OFF -DENABLE_SDL2=OFF -DENABLE_LTO=OFF -DENABLE_TESTS=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DLIBTYPE=static -DCMAKE_SHARED_LINKER_FLAGS=-static
cmake --build build/engine --target citra_libretro
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres "-DAZAHAR_ENGINE_DLL=$PWD/build/engine/bin/Release/azahar_libretro.dll" "-DAZAHAR_VULKAN_HEADERS=$PWD/azahar/externals/vulkan-headers/include"
cmake --build build/core
```

The outer build produces `build/core/azahar_multisplit_libretro.dll`. Install that DLL in `cores` and this repository’s `azahar_multisplit_libretro.info` in `info`. A native Linux wrapper build uses an adjacent patched `azahar_libretro.so` and accepts `-DMULTISPLIT_BUILD_SMOKE=ON` to build the EGL smoke test.
