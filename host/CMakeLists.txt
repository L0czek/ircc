cmake_minimum_required(VERSION 3.22)

include("${ROOT_DIR}/cmake/clang.cmake")
include("${ROOT_DIR}/cmake/utils.cmake")
project(host)
enable_language(C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${ROOT_DIR}/external/nanopb/extra")
find_package(Nanopb REQUIRED)
find_package(ftxui REQUIRED)
list(APPEND CMAKE_PREFIX_PATH
    "/usr/lib/x86_64-linux-gnu/cmake"
)
find_package(Boost CONFIG REQUIRED COMPONENTS program_options log)
#find_package(Boost REQUIRED)

nanopb_generate_cpp(TARGET protocol-amd64 ${PROTOCOL_SOURCES})

add_executable(hostctl
    src/main.cc
    src/cli.cc
    src/connector.cc
    src/periodic_task.cc
    src/tasks.cc
    src/tui.cc
)

include_directories(inc ${PROTOCOL_INC})

target_link_libraries(hostctl
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
  PRIVATE protocol-amd64
  PRIVATE Boost::program_options
  PRIVATE Boost::log
)

link_compile_commands()
