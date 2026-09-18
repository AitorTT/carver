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
  src/core/fat.cpp
  src/core/image.cpp
  src/core/ntfs.cpp
  src/core/partition.cpp
  src/core/recover.cpp
  src/core/signature.cpp
  src/core/carver.cpp
)

target_include_directories(carver_core PUBLIC src)

if(WIN32)
  target_link_libraries(carver_core PUBLIC setupapi bcrypt)
endif()

add_executable(carver-cli src/cli/main.cpp)
target_link_libraries(carver-cli PRIVATE carver_core)

if(MINGW)
  target_link_options(carver-cli PRIVATE -static)
endif()

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_executable(carver-gui WIN32
  src/gui/main.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(carver-gui PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(carver-gui PRIVATE carver_core d3d11 dxgi d3dcompiler dwmapi comdlg32 ole32 shell32)

if(MINGW)
  target_link_options(carver-gui PRIVATE -static)
endif()
