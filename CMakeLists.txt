cmake_minimum_required(VERSION 3.10)
project(KCP_UDP_Server)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# 查找libuv库
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUV REQUIRED libuv)

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kcp
    ${LIBUV_INCLUDE_DIRS}
)

# KCP源文件
set(KCP_SOURCES
    third_party/kcp/ikcp.c
)

# 链接目录
link_directories(${LIBUV_LIBRARY_DIRS})

# 创建KCP静态库
add_library(kcp STATIC ${KCP_SOURCES})

# 服务器示例
add_executable(kcp_server
    examples/server_main.cpp
    src/kcp_connection.cpp
    src/kcp_server.cpp
)

target_link_libraries(kcp_server
    kcp
    ${LIBUV_LIBRARIES}
    pthread
)

# 客户端示例
add_executable(kcp_client
    examples/client_main.cpp
    src/kcp_connection.cpp
    src/kcp_client.cpp
)

target_link_libraries(kcp_client
    kcp
    ${LIBUV_LIBRARIES}
    pthread
)

# 安装规则
install(TARGETS kcp_server kcp_client
    RUNTIME DESTINATION bin
)
