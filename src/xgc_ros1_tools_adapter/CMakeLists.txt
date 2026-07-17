cmake_minimum_required(VERSION 3.10)
project(xgc_ros1_tools_adapter VERSION 0.1.2 LANGUAGES CXX)

find_package(catkin REQUIRED COMPONENTS
  ros_babel_fish
  roscpp
  roslib
)
find_package(PkgConfig REQUIRED)
find_package(PythonInterp 3 REQUIRED)
find_package(Threads REQUIRED)
find_package(xgc2_adapter_runtime_client 0.5.0 EXACT REQUIRED CONFIG)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

catkin_package(
  CATKIN_DEPENDS ros_babel_fish roscpp roslib
)

set(XGC2_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas")
set(XGC2_CONTRACT_GENERATOR
  "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_contract_metadata.py")
set(XGC2_CONTRACT_HEADER
  "${CMAKE_CURRENT_BINARY_DIR}/generated/xgc_ros1_tools_adapter/generated_contract.hpp")
add_custom_command(
  OUTPUT "${XGC2_CONTRACT_HEADER}"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_CURRENT_BINARY_DIR}/generated/xgc_ros1_tools_adapter"
  COMMAND "${PYTHON_EXECUTABLE}" "${XGC2_CONTRACT_GENERATOR}"
          --schema-dir "${XGC2_SCHEMA_DIR}"
          --output "${XGC2_CONTRACT_HEADER}"
  DEPENDS
    "${XGC2_CONTRACT_GENERATOR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_runtime_manifests.py"
    "${XGC2_SCHEMA_DIR}/native-context.schema.json"
    "${XGC2_SCHEMA_DIR}/publish-request.schema.json"
    "${XGC2_SCHEMA_DIR}/publish-result.schema.json"
    "${XGC2_SCHEMA_DIR}/service-call-request.schema.json"
    "${XGC2_SCHEMA_DIR}/service-call-result.schema.json"
  COMMENT "Generating the exact compiled ROS1 Tools capability contract"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_contract_metadata
  DEPENDS "${XGC2_CONTRACT_HEADER}"
)

add_library(${PROJECT_NAME}_core STATIC
  "${XGC2_CONTRACT_HEADER}"
  src/json_codec.cpp
  src/publisher_registry.cpp
  src/service_invoker.cpp
  src/tools_adapter.cpp
  src/type_registry.cpp
)
target_compile_features(${PROJECT_NAME}_core PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_core PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)
target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    include
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${catkin_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
)
add_dependencies(${PROJECT_NAME}_core ${PROJECT_NAME}_contract_metadata)
target_link_libraries(${PROJECT_NAME}_core
  PUBLIC
    xgc2::adapter_runtime_client
    ${catkin_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    Threads::Threads
)

add_executable(${PROJECT_NAME}_node src/main.cpp)
target_compile_features(${PROJECT_NAME}_node PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_node PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME}_core)

add_executable(${PROJECT_NAME}_service_helper src/service_helper_main.cpp)
target_compile_features(${PROJECT_NAME}_service_helper PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME}_service_helper PRIVATE
  -Wall -Wextra -Wpedantic -Werror
)
target_include_directories(${PROJECT_NAME}_service_helper PRIVATE
  include
  ${catkin_INCLUDE_DIRS}
)
target_link_libraries(${PROJECT_NAME}_service_helper
  ${catkin_LIBRARIES}
  Threads::Threads
)

set(XGC2_INSTALLED_EXECUTABLE
  "${CMAKE_INSTALL_PREFIX}/${CATKIN_PACKAGE_BIN_DESTINATION}/${PROJECT_NAME}_node")
set(XGC2_RUNTIME_MANIFEST_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/runtime-manifests")
set(XGC2_ADAPTER_MANIFEST
  "${XGC2_RUNTIME_MANIFEST_DIR}/adapter-definitions/xgc2-ros1-tools-adapter.json")
set(XGC2_PROCESS_MANIFEST
  "${XGC2_RUNTIME_MANIFEST_DIR}/process-definitions/xgc2-ros1-tools-adapter.json")
add_custom_command(
  OUTPUT "${XGC2_ADAPTER_MANIFEST}" "${XGC2_PROCESS_MANIFEST}"
  COMMAND "${PYTHON_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_runtime_manifests.py"
          --executable "$<TARGET_FILE:${PROJECT_NAME}_node>"
          --artifact-path "${XGC2_INSTALLED_EXECUTABLE}"
          --schema-dir "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas"
          --version "${PROJECT_VERSION}"
          --adapter-output "${XGC2_ADAPTER_MANIFEST}"
          --process-output "${XGC2_PROCESS_MANIFEST}"
  COMMAND "${PYTHON_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/verify_runtime_manifests.py"
          --executable "$<TARGET_FILE:${PROJECT_NAME}_node>"
          --artifact-path "${XGC2_INSTALLED_EXECUTABLE}"
          --schema-dir "${XGC2_SCHEMA_DIR}"
          --adapter-manifest "${XGC2_ADAPTER_MANIFEST}"
          --process-manifest "${XGC2_PROCESS_MANIFEST}"
  DEPENDS
    ${PROJECT_NAME}_node
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/generate_runtime_manifests.py"
    "${XGC2_CONTRACT_GENERATOR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/verify_runtime_manifests.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/native-context.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/publish-request.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/publish-result.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/service-call-request.schema.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/service-call-result.schema.json"
  COMMENT "Generating exact ROS1 Tools Adapter Runtime manifests"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_runtime_manifests ALL
  DEPENDS "${XGC2_ADAPTER_MANIFEST}" "${XGC2_PROCESS_MANIFEST}"
)

install(TARGETS
  ${PROJECT_NAME}_node
  ${PROJECT_NAME}_service_helper
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/"
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/schemas
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/install_runtime_manifests.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_manifests.cmake"
  @ONLY
)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_manifests.cmake")

if(CATKIN_ENABLE_TESTING)
  find_package(rostest REQUIRED)
  find_package(geometry_msgs REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(std_srvs REQUIRED)

  catkin_add_gtest(${PROJECT_NAME}_unit_tests
    test/json_codec_test.cpp
    test/service_helper_protocol_test.cpp
    test/tools_adapter_config_test.cpp
    test/type_catalog_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_unit_tests)
    add_dependencies(${PROJECT_NAME}_unit_tests
      ${PROJECT_NAME}_contract_metadata)
    target_compile_features(${PROJECT_NAME}_unit_tests PUBLIC cxx_std_17)
    target_compile_options(${PROJECT_NAME}_unit_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
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

  catkin_add_gtest(${PROJECT_NAME}_service_helper_lifecycle_tests
    test/service_helper_lifecycle_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_service_helper_lifecycle_tests)
    target_compile_features(${PROJECT_NAME}_service_helper_lifecycle_tests
      PUBLIC cxx_std_17)
    target_compile_options(${PROJECT_NAME}_service_helper_lifecycle_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(${PROJECT_NAME}_service_helper_lifecycle_tests
      ${catkin_LIBRARIES}
    )
    add_dependencies(${PROJECT_NAME}_service_helper_lifecycle_tests
      ${PROJECT_NAME}_service_helper)
  endif()

  add_rostest_gtest(${PROJECT_NAME}_integration_tests
    test/tools_adapter_integration.test
    test/tools_adapter_integration_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_integration_tests)
    add_dependencies(${PROJECT_NAME}_integration_tests
      ${PROJECT_NAME}_contract_metadata
      ${PROJECT_NAME}_service_helper)
    target_compile_features(${PROJECT_NAME}_integration_tests PUBLIC cxx_std_17)
    target_compile_options(${PROJECT_NAME}_integration_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror
    )
    target_include_directories(${PROJECT_NAME}_integration_tests PRIVATE
      include
      ${catkin_INCLUDE_DIRS}
      ${JSONCPP_INCLUDE_DIRS}
    )
    target_link_libraries(${PROJECT_NAME}_integration_tests
      ${PROJECT_NAME}_core
      ${catkin_LIBRARIES}
    )
  endif()

  add_test(NAME ${PROJECT_NAME}_runtime_manifest_tests
    COMMAND "${PYTHON_EXECUTABLE}" -m unittest discover -v
            -s "${CMAKE_CURRENT_SOURCE_DIR}/../../test"
            -p "test_*.py"
  )
endif()
