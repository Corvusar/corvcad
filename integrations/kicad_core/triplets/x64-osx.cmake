# Host triplet for ios_deps.sh (protoc and other build tools). With an explicit SDK sysroot,
# clang's default /usr/local/include is the SDK's, so Intel Homebrew headers there (a newer
# Abseil, for example) cannot shadow vcpkg's own.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_SYSROOT macosx)
set(VCPKG_BUILD_TYPE release)
