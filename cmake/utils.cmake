
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

function(configure_openocd)
    set(openocd_config_file "${CMAKE_CURRENT_BINARY_DIR}/openocd.cfg")

    if (NOT EXISTS "${openocd_config_file}")
        file(WRITE "${openocd_config_file}"
            "source [find interface/${programmer_name}.cfg]\n"
            "source [find target/${chip_family}.cfg]\n"
            "gdb_port ${gdb_port}\n"
        )
        message(STATUS "Generated OpenOCD config at ${openocd_config_file}")
    endif ()
    
    add_custom_target(openocd
        COMMAND openocd -f "${openocd_config_file}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Starting openocd with config ${openocd_config_file}"
    )
endfunction(configure_openocd)

function(configure_gdb)
    set(gdb_args -ex "tar ext :${gdb_port}" -ex "monitor reset halt" -ex "load")
    
    if (use_semihosting)
        set(gdb_args ${gdb_args}  -ex "monitor arm semihosting enable")
    endif ()

    add_custom_target(gdb
        COMMAND gdb-multiarch ${gdb_args} -ex "b main" -ex "c" ${output_elf}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Starting GDB"
    )

    add_custom_target(run
        COMMAND gdb-multiarch ${gdb_args} -ex "monitor reset run" -ex "exit" ${output_elf}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Flashing and reseting"
    )


endfunction(configure_gdb)

function(link_compile_commands)
    add_custom_target(link_compile_commands ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Linking or copying compile_commands.json to source directory"
        COMMAND ${CMAKE_COMMAND} -E remove -f "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json"
        BYPRODUCTS "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMENT "Creating symlink for compile_commands.json in source directory"
        VERBATIM
    )
endfunction(link_compile_commands)

