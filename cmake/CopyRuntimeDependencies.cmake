if(NOT DEFINED INPUT_FILE OR INPUT_FILE STREQUAL "")
  message(FATAL_ERROR "CopyRuntimeDependencies.cmake requires INPUT_FILE")
endif()

if(NOT DEFINED OUTPUT_DIR OR OUTPUT_DIR STREQUAL "")
  message(FATAL_ERROR "CopyRuntimeDependencies.cmake requires OUTPUT_DIR")
endif()

if(NOT DEFINED SEARCH_DIRS OR SEARCH_DIRS STREQUAL "")
  message(FATAL_ERROR "CopyRuntimeDependencies.cmake requires SEARCH_DIRS")
endif()

file(GLOB existing_runtime_dlls "${OUTPUT_DIR}/*.dll")
foreach(existing_runtime_dll IN LISTS existing_runtime_dlls)
  file(REMOVE "${existing_runtime_dll}")
endforeach()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${INPUT_FILE}"
  RESOLVED_DEPENDENCIES_VAR resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
  DIRECTORIES ${SEARCH_DIRS}
)

foreach(dependency IN LISTS resolved_dependencies)
  get_filename_component(dependency_dir "${dependency}" DIRECTORY)
  list(FIND SEARCH_DIRS "${dependency_dir}" dependency_dir_index)
  if(dependency_dir_index LESS 0)
    continue()
  endif()

  get_filename_component(dependency_name "${dependency}" NAME)
  if("${OUTPUT_DIR}/${dependency_name}" STREQUAL "${dependency}")
    continue()
  endif()

  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dependency}" "${OUTPUT_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
  )
endforeach()
