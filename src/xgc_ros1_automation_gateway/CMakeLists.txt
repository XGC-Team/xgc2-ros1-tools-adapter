cmake_minimum_required(VERSION 3.0.2)
project(xgc_ros1_automation_gateway)

find_package(catkin REQUIRED COMPONENTS
  ros_babel_fish
  roscpp
  roslib
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
find_package(Threads REQUIRED)

catkin_package(
  CATKIN_DEPENDS ros_babel_fish roscpp roslib
)

add_library(${PROJECT_NAME}_core STATIC
  src/gateway.cpp
  src/json_codec.cpp
  src/publisher_registry.cpp
  src/request_cache.cpp
  src/service_invoker.cpp
  src/type_registry.cpp
  src/uds_server.cpp
)
target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_core PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    include
    ${catkin_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
)
target_link_libraries(${PROJECT_NAME}_core
  PUBLIC
    ${catkin_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    Threads::Threads
)

add_executable(${PROJECT_NAME}_node src/main.cpp)
target_compile_features(${PROJECT_NAME}_node PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_node PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME}_core)

install(TARGETS ${PROJECT_NAME}_node
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

if(CATKIN_ENABLE_TESTING)
  find_package(rostest REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(std_srvs REQUIRED)

  catkin_add_gtest(${PROJECT_NAME}_unit_tests
    test/json_codec_test.cpp
    test/type_catalog_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_unit_tests)
    target_compile_features(${PROJECT_NAME}_unit_tests PUBLIC cxx_std_17)
    target_include_directories(${PROJECT_NAME}_unit_tests PRIVATE
      include
      ${catkin_INCLUDE_DIRS}
      ${JSONCPP_INCLUDE_DIRS}
    )
    target_link_libraries(${PROJECT_NAME}_unit_tests
      ${PROJECT_NAME}_core
      ${catkin_LIBRARIES}
    )
  endif()

  add_rostest_gtest(${PROJECT_NAME}_e2e_tests
    test/gateway_e2e.test
    test/gateway_e2e_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_e2e_tests)
    add_dependencies(${PROJECT_NAME}_e2e_tests ${PROJECT_NAME}_node)
    target_compile_features(${PROJECT_NAME}_e2e_tests PUBLIC cxx_std_17)
    target_include_directories(${PROJECT_NAME}_e2e_tests PRIVATE
      include
      ${catkin_INCLUDE_DIRS}
      ${JSONCPP_INCLUDE_DIRS}
    )
    target_link_libraries(${PROJECT_NAME}_e2e_tests
      ${PROJECT_NAME}_core
      ${catkin_LIBRARIES}
    )
  endif()
endif()
