add_library(ctl
    src/ctl.cc
)

target_include_directories(ctl PRIVATE inc ${BOARD_HEADERS_INC})
