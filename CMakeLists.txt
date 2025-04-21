cmake_minimum_required(VERSION 3.10)

project(caneo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(dbcppp REQUIRED)

file(GLOB_RECURSE SOURCES "caneo/src/*.cpp")
file(GLOB_RECURSE HEADERS "caneo/inc/*.h")

add_executable(${PROJECT_NAME} 
    ${CMAKE_SOURCE_DIR}/main.cpp 
    ${SOURCES} 
    ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/caneo/inc)

target_link_libraries(${PROJECT_NAME}   
    PUBLIC

    dbcppp::dbcppp
)