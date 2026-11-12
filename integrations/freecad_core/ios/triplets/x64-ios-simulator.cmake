# Opt-in Intel simulator target. Keep separate from KiCad and arm64 prefixes.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
set(VCPKG_OSX_DEPLOYMENT_TARGET 17.0)
set(VCPKG_BUILD_TYPE release)
set(X_VCPKG_BUILD_GNU_LIBICONV 0)
# The version suffix distinguishes this target from an Intel macOS build,
# preventing Autoconf from trying to execute simulator programs on the host.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=x86_64-apple-darwin17.0.0")
