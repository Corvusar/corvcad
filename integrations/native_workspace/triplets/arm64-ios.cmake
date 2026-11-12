# Same ABI settings as integrations/kicad_core/triplets/arm64-ios.cmake.
set(VCPKG_TARGET_ARCHITECTURE arm64)
# Seed CMake's cache before Qt's pre-project autodetection. The toolchain's
# later architecture default alone lets Qt select arm64+x86_64 universal builds.
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphoneos)
set(VCPKG_OSX_DEPLOYMENT_TARGET 17.0)
set(VCPKG_BUILD_TYPE release)
set(X_VCPKG_BUILD_GNU_LIBICONV 0)
# The baseline's vcpkg-make uses the same Darwin host/build triplet on
# Apple silicon, causing configure to execute an iOS program on macOS.
# Keep Darwin's platform selection but force an explicitly distinct host for
# every Autoconf package (including indirect dependencies such as libb2).
# Host tools use the separate arm64-osx triplet and do not inherit this setting.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-darwin17.0.0")
