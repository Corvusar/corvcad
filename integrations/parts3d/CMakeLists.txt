# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop build of the 3D part generator and its check (docs/3D_MODELS.md gate 2):
#   cmake -S integrations/parts3d -B build/parts3d -DCMAKE_BUILD_TYPE=Release && cmake --build build/parts3d
#   build/parts3d/parts3d_check all
cmake_minimum_required(VERSION 3.20)
project(corvcad_parts3d CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCASCADE REQUIRED) # its DataExchange component needs the others' targets anyway
add_library(corvcad_parts3d STATIC parts3d.cpp parts3d_c_api.cpp)
target_include_directories(corvcad_parts3d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(corvcad_parts3d PUBLIC TKernel TKMath TKBRep TKG2d TKG3d TKGeomBase TKGeomAlgo TKTopAlgo
  TKPrim TKBO TKFillet TKOffset TKShHealing TKBool TKMesh)
add_executable(parts3d_check check.cpp)
target_link_libraries(parts3d_check corvcad_parts3d TKDESTEP TKXSBase)
