# iOS simulator on Apple silicon Macs (CI runners), static, release only, matching the app's
# iOS 17 minimum.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
set(VCPKG_OSX_DEPLOYMENT_TARGET 17.0)
set(VCPKG_BUILD_TYPE release)
# The iOS SDK ships libiconv; vcpkg's GNU libiconv build cannot configure for the simulator.
set(X_VCPKG_BUILD_GNU_LIBICONV 0)
