set(CMAKE_SYSTEM_NAME               Linux)
set(CMAKE_SYSTEM_PROCESSOR          amd64)

set(CMAKE_C_COMPILER_ID Clang)
set(CMAKE_CXX_COMPILER_ID Clang)

set(CMAKE_C_COMPILER                /usr/bin/clang)
set(CMAKE_CXX_COMPILER              /usr/bin/clang++)

set(CMAKE_CXX_STANDARD 23)

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

# No CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY / CMAKE_LINKER here: this is the native host
# build. CMake must link its test program to detect the library architecture
# (x86_64-linux-gnu); without it find_package() skips /usr/lib/x86_64-linux-gnu/cmake,
# where Debian installs ftxui since 6.x.

set(CMAKE_C_FLAGS "-Wall -Wextra -Wpedantic")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0")

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic")
set(CMAKE_C_LINK_FLAGS "")
set(CMAKE_CXX_LINK_FLAGS "")


