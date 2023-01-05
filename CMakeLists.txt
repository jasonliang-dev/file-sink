cmake_minimum_required(VERSION 3.12)
project(app)

include(FetchContent)
set(FETCHCONTENT_QUIET NO)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_EXAMPLES OFF)
set(BUILD_TESTING OFF)

FetchContent_Declare(
  libssh2
  URL https://github.com/libssh2/libssh2/archive/refs/tags/libssh2-1.10.0.tar.gz
)
FetchContent_MakeAvailable(libssh2)

set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_EXAMPLES OFF)

FetchContent_Declare(
  glfw
  URL https://github.com/glfw/glfw/archive/refs/tags/3.3.6.tar.gz
)
FetchContent_MakeAvailable(glfw)

# glfw
add_definitions(-DGLFW_INCLUDE_NONE)

# windows
add_definitions(-DNOMINMAX)
add_definitions(-DUNICODE)

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} glfw libssh2)