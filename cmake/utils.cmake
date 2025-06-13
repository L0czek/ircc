
function(replace_cmake_source_dir start_dir)
    if(NOT IS_DIRECTORY "${start_dir}")
        message(FATAL_ERROR "Provided path '${start_dir}' is not a directory.")
    endif()

    file(GLOB_RECURSE cmake_files
        "${start_dir}/CMakeLists.txt"
        "${start_dir}/*.cmake"
    )

    foreach(file_path IN LISTS cmake_files)
        file(READ "${file_path}" file_content)
        string(REPLACE "\${CMAKE_SOURCE_DIR}" "\${PROJECT_SOURCE_DIR}" updated_content "${file_content}")
        if(NOT file_content STREQUAL updated_content)
            message(STATUS "Replacing in: ${file_path}")
            file(WRITE "${file_path}" "${updated_content}")
        endif()
    endforeach()
endfunction()

function(use_cubemx_target dir)
    replace_cmake_source_dir(${dir})
    add_subdirectory(${dir})
endfunction(use_cubemx_target)

