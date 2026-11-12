# MIT. Based on vcpkg 9e593bb18ea69cc5095e012465dcd675a822ed0d
# triplets/arm64-osx.cmake. Host tools only; target recipes stay separate.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# Match the Intel host policy: imported dependency headers use -I, ahead of
# compiler-injected Homebrew paths, without changing the iOS target flags.
list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON)
