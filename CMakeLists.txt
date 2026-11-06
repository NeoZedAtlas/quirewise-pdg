cmake_minimum_required(VERSION 3.21)
project(QuirewisePDG VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(QuirewiseImage CONFIG REQUIRED)
find_package(QuirewiseCrypto CONFIG REQUIRED)

set(QW_PDG_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/pdg"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/pdz"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/image"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/djvu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

file(GLOB_RECURSE QW_PDG_SOURCES CONFIGURE_DEPENDS
    src/formats/pdg/*.c
    src/formats/pdz/*.c
    src/formats/djvu/djvu_parser.c)

add_library(quirewise_pdg ${QW_LIBRARY_TYPE} ${QW_PDG_SOURCES})
add_library(Quirewise::PDG ALIAS quirewise_pdg)
set_target_properties(quirewise_pdg PROPERTIES
    EXPORT_NAME PDG
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_pdg PUBLIC c_std_11)
target_include_directories(quirewise_pdg PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${QW_PDG_INCLUDE_DIRS})
target_link_libraries(quirewise_pdg PRIVATE
    Quirewise::Image
    Quirewise::Crypto
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")

install(TARGETS quirewise_pdg EXPORT QuirewisePDGTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewisePDGTargets
    FILE QuirewisePDGTargets.cmake
    NAMESPACE Quirewise::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePDG")
configure_package_config_file(
    cmake/QuirewisePDGConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePDGConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePDG")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePDGConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePDGConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePDGConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePDG")

if(BUILD_TESTING)
    add_executable(quirewise_pdg_djvu_unsupported_test
        tests/djvu_unsupported_test.c)
    target_compile_features(quirewise_pdg_djvu_unsupported_test PRIVATE c_std_11)
    target_include_directories(quirewise_pdg_djvu_unsupported_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/pdg"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/djvu")
    target_link_libraries(quirewise_pdg_djvu_unsupported_test PRIVATE
        quirewise_pdg Quirewise::Image Quirewise::Crypto
        "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")
    add_test(NAME quirewise_pdg_djvu_unsupported
        COMMAND quirewise_pdg_djvu_unsupported_test)
endif()

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_pdg
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(quirewise_pdg_fuzz_smoke tests/pdg_fuzz_smoke.c)
    target_compile_features(quirewise_pdg_fuzz_smoke PRIVATE c_std_11)
    target_include_directories(quirewise_pdg_fuzz_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(quirewise_pdg_fuzz_smoke PRIVATE Quirewise::PDG)
    add_test(NAME quirewise_pdg_fuzz_smoke
        COMMAND quirewise_pdg_fuzz_smoke)
endif()
