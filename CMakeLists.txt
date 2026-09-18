cmake_minimum_required(VERSION 3.20)
project(carver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_library(carver_core STATIC
  src/core/device.cpp
  src/core/device_enum.cpp
  src/core/signature.cpp
  src/core/carver.cpp
)

target_include_directories(carver_core PUBLIC src)

if(WIN32)
  target_link_libraries(carver_core PUBLIC setupapi)
endif()

add_executable(carver-cli src/cli/main.cpp)
target_link_libraries(carver-cli PRIVATE carver_core)

if(MINGW)
  target_link_options(carver-cli PRIVATE -static)
endif()
