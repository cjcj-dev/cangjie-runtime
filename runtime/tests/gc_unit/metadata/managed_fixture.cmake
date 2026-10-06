# Complete CJ metadata is a separate linked image, never a C++ header model.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MANAGED_METADATA_LINKER "" CACHE FILEPATH "Qualified CJ linker (one tool family)")
if(NOT MANAGED_METADATA_LINKER)
  if(WIN32)
    find_program(MANAGED_METADATA_LINKER_FOUND lld-link REQUIRED)
  else()
    find_program(MANAGED_METADATA_LINKER_FOUND ld.lld REQUIRED)
  endif()
  set(MANAGED_METADATA_LINKER "${MANAGED_METADATA_LINKER_FOUND}")
endif()
if(WIN32)
  set(_managed_image cj_managed_metadata.dll)
elseif(APPLE)
  set(_managed_image libcj_managed_metadata.dylib)
else()
  set(_managed_image libcj_managed_metadata.so)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_managed_image}"
  COMMAND ${Python3_EXECUTABLE} ${MANAGED_METADATA_SOURCE}/build_managed_fixture.py
    --runtime ${RUNTIME} --output ${CMAKE_CURRENT_BINARY_DIR}
    --cc ${CMAKE_CXX_COMPILER} --linker ${MANAGED_METADATA_LINKER}
    --system ${CMAKE_SYSTEM_NAME} --arch ${CMAKE_SYSTEM_PROCESSOR}
  DEPENDS ${MANAGED_METADATA_SOURCE}/build_managed_fixture.py ${MANAGED_METADATA_SOURCE}/managed_input.S ${MANAGED_METADATA_SOURCE}/windows_input.S
  VERBATIM)
add_custom_target(managed-metadata-image DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_managed_image}")
add_dependencies(${MANAGED_METADATA_CONSUMER} managed-metadata-image)
