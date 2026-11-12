# MIT. Based on vcpkg 9e593bb18ea69cc5095e012465dcd675a822ed0d
# triplets/community/x64-osx.cmake. Host tools only; target recipes stay separate.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

# Clang's implicit /usr/local/include precedes imported -isystem directories.
# Preserve the reviewed dependency prefix as -I so host Homebrew headers cannot
# replace headers from imported vcpkg libraries (notably Abseil for protoc).
list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON)
