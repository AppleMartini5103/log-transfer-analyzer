add_executable(server
    src/app/main.cpp
)

target_include_directories(server PRIVATE
    src
    ${THIRDPARTY_DIR}/libuv/include
)
target_link_libraries(server PRIVATE common ${LIBUV_LIB})

if(NOT WIN32)
    target_link_libraries(server PRIVATE pthread dl)
endif()
