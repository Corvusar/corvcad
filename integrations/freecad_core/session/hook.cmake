# MIT. Extend the reviewed F3 build without editing its hashed source tree.
set(CORVCAD_SESSION_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "F4 adapter sources")
cmake_language(DEFER CALL include "${CORVCAD_SESSION_SOURCE_DIR}/CMakeLists.txt")
