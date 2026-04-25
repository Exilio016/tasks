cmake_minimum_required(VERSION 3.20)
project(tasks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(SQLiteCpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(tasks main.cpp task.cpp gui.cpp)
target_link_libraries(tasks PRIVATE SQLiteCpp PkgConfig::GTK4)
