function(binja_diff_atomic_install source destination)
  get_filename_component(parent "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "$ENV{DESTDIR}${parent}")
  set(staged "$ENV{DESTDIR}${destination}.next")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${source}" "${staged}"
    RESULT_VARIABLE copy_result)
  if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to stage ${source} for installation: ${copy_result}")
  endif()
  file(RENAME "${staged}" "$ENV{DESTDIR}${destination}")
  list(APPEND CMAKE_INSTALL_MANIFEST_FILES "${destination}")
  set(CMAKE_INSTALL_MANIFEST_FILES "${CMAKE_INSTALL_MANIFEST_FILES}" PARENT_SCOPE)
endfunction()
