cmake_minimum_required(VERSION 3.8)
project(cage_ros_bridge)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(backward_ros REQUIRED)
find_package(ament_cmake_auto REQUIRED)
find_package(Boost COMPONENTS program_options timer REQUIRED)
ament_auto_find_build_dependencies()

add_subdirectory(CageClient)

if(${tf2_geometry_msgs_VERSION} VERSION_LESS 0.18.0)
  target_compile_definitions(cage_ros_bridge PUBLIC
    USE_TF2_GEOMETRY_MSGS_DEPRECATED_HEADER
  )
endif()

ament_auto_add_executable(cage_ros_bridge
  bridge_main.cpp
)

add_backward(cage_ros_bridge)

target_link_libraries(cage_ros_bridge
  Boost::program_options
  cageClientIF
  rt
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()
ament_auto_package(INSTALL_TO_SHARE)
