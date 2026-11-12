set(VCPKG_TARGET_ARCHITECTURE arm64)
# Qt autodetects before the toolchain; pass the architecture explicitly.
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
set(VCPKG_OSX_DEPLOYMENT_TARGET 17.0)
set(VCPKG_BUILD_TYPE release)
set(X_VCPKG_BUILD_GNU_LIBICONV 0)
# Force Autoconf cross-compilation for all target packages, including indirect
# dependencies. Host tools still use the unmodified arm64-osx triplet.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-darwin17.0.0")
