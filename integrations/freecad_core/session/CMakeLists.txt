# MIT. Invoked at the end of the unchanged FreeCAD top-level project.
if(NOT FREECAD_STATIC_MODULES OR NOT TARGET FreeCADMainCmd)
  message(FATAL_ERROR "F4 requires the reviewed F3 static FreeCAD profile")
endif()
get_filename_component(CORVCAD_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(CORVCAD_IOS_EXPAT_LIBRARY)
  if(NOT IS_ABSOLUTE "${CORVCAD_IOS_EXPAT_LIBRARY}" OR NOT EXISTS "${CORVCAD_IOS_EXPAT_LIBRARY}")
    message(FATAL_ERROR "Explicit shared static Expat required by the target Python build")
  endif()
  target_link_libraries(FreeCADMainCmd "${CORVCAD_IOS_EXPAT_LIBRARY}")
endif()
get_target_property(_f3_libraries FreeCADMainCmd LINK_LIBRARIES)
get_target_property(_f3_options FreeCADMainCmd LINK_OPTIONS)
get_target_property(_f3_includes FreeCADMainCmd INCLUDE_DIRECTORIES)
add_library(corvcad_freecad_session STATIC
  "${CMAKE_CURRENT_LIST_DIR}/mechanical_session.cpp" "${CMAKE_CURRENT_LIST_DIR}/../ios/static_registry.cpp")
target_compile_features(corvcad_freecad_session PUBLIC cxx_std_20)
target_include_directories(corvcad_freecad_session PRIVATE
  "${CORVCAD_REPO}/engine/include" "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/src" ${_f3_includes}
  ${Python3_INCLUDE_DIRS})
target_link_libraries(corvcad_freecad_session PRIVATE Qt6::Core FreeCADApp)
add_executable(corvcad_mechanical_probe "${CMAKE_CURRENT_LIST_DIR}/mechanical_session_test.cpp")
target_compile_features(corvcad_mechanical_probe PRIVATE cxx_std_20)
target_include_directories(corvcad_mechanical_probe PRIVATE "${CORVCAD_REPO}/engine/include" ${Python3_INCLUDE_DIRS})
target_link_libraries(corvcad_mechanical_probe PRIVATE corvcad_freecad_session Qt6::Core ${_f3_libraries})
if(_f3_options)
  target_link_options(corvcad_mechanical_probe PRIVATE ${_f3_options})
endif()
set_property(TARGET corvcad_mechanical_probe PROPERTY qt_no_entrypoint TRUE)
set_target_properties(corvcad_mechanical_probe PROPERTIES
  MACOSX_BUNDLE FALSE RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/corvcad-session/bin")
