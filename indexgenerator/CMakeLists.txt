cmake_minimum_required(VERSION 3.1)
project(codebrowser_indexgenerator)
find_package(spdlog CONFIG REQUIRED)
add_executable(codebrowser_indexgenerator indexer.cpp)
target_link_libraries(codebrowser_indexgenerator PRIVATE spdlog::spdlog_header_only)
set_property(TARGET codebrowser_indexgenerator PROPERTY CXX_STANDARD 14)
install(TARGETS codebrowser_indexgenerator RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})


IF (APPLE)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}  -stdlib=libc++" )
ENDIF()
