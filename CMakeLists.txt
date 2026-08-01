cmake_minimum_required(VERSION 3.28)

project(
  uci_arena_installer
  VERSION 0.1.0
  DESCRIPTION "Transactional installer and release assembly for UCI Arena"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(ARENA_INSTALLER_BUILD_TESTS "Build installer tests" ON)
set(
  ARENA_TUF_ROOT_SHA256
  ""
  CACHE STRING
  "Pinned SHA-256 of the trusted TUF root for official installer builds"
)

find_package(CLI11 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_library(arena_installer_core
  src/adapters/uci_process.cpp
  src/application/installer/credential_recovery.cpp
  src/application/installer/setup.cpp
  src/application/process_supervisor.cpp
  src/domain/uuid.cpp
)
target_include_directories(
  arena_installer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(
  arena_installer_core
  PRIVATE ARENA_TUF_ROOT_SHA256="${ARENA_TUF_ROOT_SHA256}"
)
target_link_libraries(
  arena_installer_core
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE
    CURL::libcurl
    LibArchive::LibArchive
    unofficial-sodium::sodium
)

if(WIN32)
  target_sources(arena_installer_core PRIVATE
    src/platform/windows/process_platform.cpp
    src/platform/windows/service_control.cpp
    src/platform/windows/workspace_permissions.cpp
  )
  target_link_libraries(arena_installer_core PRIVATE advapi32)
else()
  target_sources(arena_installer_core PRIVATE
    src/platform/linux/process_platform.cpp
    src/platform/linux/service_control.cpp
    src/platform/linux/workspace_permissions.cpp
  )
  target_link_libraries(arena_installer_core PRIVATE pthread)
endif()

add_executable(arena-setup apps/setup/main.cpp)
target_link_libraries(
  arena-setup
  PRIVATE arena_installer_core CLI11::CLI11 CURL::libcurl)

if(MSVC)
  target_compile_options(arena_installer_core PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-setup PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(arena_installer_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-setup PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  add_library(arena_windows_installer_launcher
    src/platform/windows/installer_launcher.cpp
  )
  target_include_directories(
    arena_windows_installer_launcher
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(
    arena_windows_installer_launcher
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE bcrypt comctl32 msi ole32 shell32
  )
  add_executable(
    uci-arena-installer-launcher
    WIN32
    apps/installer_launcher/main.cpp
    apps/installer_launcher/resources.rc
  )
  target_link_libraries(
    uci-arena-installer-launcher
    PRIVATE arena_windows_installer_launcher shell32
  )
  if(MSVC)
    target_compile_options(
      arena_windows_installer_launcher PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(
      uci-arena-installer-launcher PRIVATE /W4 /permissive- /EHsc)
  endif()
endif()

if(ARENA_INSTALLER_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(
    arena_mock_uci tests/mocks/mock_uci_engine.cpp)
  add_executable(
    arena_mock_service tests/mocks/mock_service_cli.cpp)
  add_executable(
    arena_installer_tests
    tests/unit/setup_tests.cpp
    tests/unit/windows_installer_launcher_tests.cpp
  )
  add_dependencies(
    arena_installer_tests arena_mock_uci arena_mock_service)
  target_compile_definitions(
    arena_installer_tests
    PRIVATE
      ARENA_MOCK_UCI_PATH="$<TARGET_FILE:arena_mock_uci>"
      ARENA_MOCK_SERVICE_PATH="$<TARGET_FILE:arena_mock_service>"
  )
  target_include_directories(
    arena_installer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(
    arena_installer_tests
    PRIVATE
      arena_installer_core
      Catch2::Catch2WithMain
      unofficial-sodium::sodium
  )
  if(WIN32)
    target_link_libraries(
      arena_installer_tests PRIVATE arena_windows_installer_launcher)
  endif()
  include(Catch)
  catch_discover_tests(arena_installer_tests)
endif()
