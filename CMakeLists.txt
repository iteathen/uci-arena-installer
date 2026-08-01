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

find_package(nlohmann_json CONFIG REQUIRED)

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
    arena_installer_tests
    tests/unit/windows_installer_launcher_tests.cpp
  )
  target_include_directories(
    arena_installer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(
    arena_installer_tests PRIVATE Catch2::Catch2WithMain)
  if(WIN32)
    target_link_libraries(
      arena_installer_tests PRIVATE arena_windows_installer_launcher)
  endif()
  include(Catch)
  catch_discover_tests(arena_installer_tests)
endif()
