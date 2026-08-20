cmake_minimum_required(VERSION 3.28)

project(
  uci_arena_installer
  VERSION 0.2.0
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

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
  set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
  include(InstallRequiredSystemLibraries)
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_library(arena_installer_core
  src/adapters/asset_search_index.cpp
  src/adapters/filesystem_identity.cpp
  src/adapters/service_control.cpp
  src/adapters/uci_process.cpp
  src/application/installer/credential_recovery.cpp
  src/application/installer/removal_lifecycle.cpp
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
    src/platform/windows/asset_search_index.cpp
    src/platform/windows/filesystem_path.cpp
    src/platform/windows/process_platform.cpp
    src/platform/windows/service_control.cpp
    src/platform/windows/workspace_permissions.cpp
  )
  target_link_libraries(
    arena_installer_core PRIVATE advapi32 ole32 oleaut32 uuid)
else()
  target_sources(arena_installer_core PRIVATE
    src/platform/linux/asset_search_index.cpp
    src/platform/linux/filesystem_path.cpp
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

if(WIN32)
  set(ARENA_INSTALLER_RUNTIME_SEARCH_DIRECTORIES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
else()
  set(ARENA_INSTALLER_RUNTIME_SEARCH_DIRECTORIES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
endif()

install(
  TARGETS arena-setup
  RUNTIME_DEPENDENCIES
    DIRECTORIES ${ARENA_INSTALLER_RUNTIME_SEARCH_DIRECTORIES}
    PRE_EXCLUDE_REGEXES
      "api-ms-"
      "ext-ms-"
      "[Aa]zure[Aa]ttest.*"
      "[Hh]vsi[Ff]ile[Tt]rust\\.dll"
      "[Pp]dm[Uu]tilities\\.dll"
      "[Ww]paxholder\\.dll"
    POST_EXCLUDE_REGEXES
      ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
      "^/lib(64)?/.*"
      "^/usr/lib(64)?/.*"
  RUNTIME DESTINATION bin
)
install(FILES LICENSE DESTINATION share/licenses/uci-arena-installer)
file(GLOB ARENA_INSTALLER_VCPKG_COPYRIGHTS CONFIGURE_DEPENDS
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
foreach(ARENA_INSTALLER_VCPKG_COPYRIGHT IN LISTS ARENA_INSTALLER_VCPKG_COPYRIGHTS)
  get_filename_component(ARENA_INSTALLER_VCPKG_SHARE_DIR
    "${ARENA_INSTALLER_VCPKG_COPYRIGHT}" DIRECTORY)
  get_filename_component(ARENA_INSTALLER_VCPKG_PACKAGE
    "${ARENA_INSTALLER_VCPKG_SHARE_DIR}" NAME)
  install(FILES "${ARENA_INSTALLER_VCPKG_COPYRIGHT}"
    DESTINATION "share/licenses/vcpkg/${ARENA_INSTALLER_VCPKG_PACKAGE}")
endforeach()

if(MSVC)
  target_compile_options(arena_installer_core PRIVATE /W4 /permissive- /EHsc)
  target_compile_options(arena-setup PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(arena_installer_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(arena-setup PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  if(NOT CMAKE_VS_MSBUILD_COMMAND)
    message(FATAL_ERROR "Windows suite bootstrapper build requires Visual Studio MSBuild")
  endif()
  set(
    ARENA_BUNDLE_BAFUNCTIONS_PROJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/bootstrapper/arena-bundle-bafunctions.vcxproj"
  )
  set(
    ARENA_BUNDLE_BAFUNCTIONS_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/arena-bundle-bafunctions.dll"
  )
  add_custom_target(
    arena_bundle_bafunctions ALL
    COMMAND
      "${CMAKE_VS_MSBUILD_COMMAND}"
      "${ARENA_BUNDLE_BAFUNCTIONS_PROJECT}"
      /restore
      /target:Build
      /property:Configuration=$<CONFIG>
      /property:Platform=x64
      /property:OutDir=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/
      /property:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/bafunctions-obj/
      /property:IntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/bafunctions-obj/$<CONFIG>/
    BYPRODUCTS "${ARENA_BUNDLE_BAFUNCTIONS_OUTPUT}"
    COMMENT "Building the WiX bootstrapper lifecycle adapter"
    VERBATIM
  )

  add_executable(
    arena-setup-launcher
    apps/setup_launcher/main.cpp
  )
  target_link_libraries(arena-setup-launcher PRIVATE shell32)
  set_property(
    TARGET arena-setup-launcher
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )

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
      arena-setup-launcher PRIVATE /W4 /permissive- /EHsc)
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
  target_link_libraries(
    arena_mock_service PRIVATE nlohmann_json::nlohmann_json)
  add_executable(
    arena_installer_tests
    tests/unit/setup_tests.cpp
    tests/unit/filesystem_identity_tests.cpp
    tests/unit/service_control_tests.cpp
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
      LibArchive::LibArchive
      unofficial-sodium::sodium
  )
  if(WIN32)
    add_dependencies(arena_installer_tests arena_bundle_bafunctions)
    target_sources(
      arena_installer_tests
      PRIVATE tests/unit/related_bundle_plan_guard_tests.cpp
    )
    target_include_directories(
      arena_installer_tests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/bootstrapper
    )
    target_link_libraries(
      arena_installer_tests PRIVATE arena_windows_installer_launcher)
  endif()
  include(Catch)
  catch_discover_tests(arena_installer_tests)
endif()
