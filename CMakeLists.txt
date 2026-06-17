cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME read_pdf)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)
find_package(qpdf REQUIRED)
find_package(JPEG REQUIRED)   
find_package(ZLIB REQUIRED)

get_target_property(_qpdf_libs qpdf::libqpdf INTERFACE_LINK_LIBRARIES)
if(_qpdf_libs)
    list(TRANSFORM _qpdf_libs REPLACE "^jpeg$" "JPEG::JPEG")
    list(TRANSFORM _qpdf_libs REPLACE "^z$"    "ZLIB::ZLIB")
    set_target_properties(qpdf::libqpdf PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_qpdf_libs}")
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

find_package(qpdf REQUIRED)

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
set(EXTENSION_SOURCES src/read_pdf_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME}
    qpdf::libqpdf OpenSSL::SSL OpenSSL::Crypto JPEG::JPEG ZLIB::ZLIB
)
target_link_libraries(${LOADABLE_EXTENSION_NAME}
    qpdf::libqpdf OpenSSL::SSL OpenSSL::Crypto JPEG::JPEG ZLIB::ZLIB
)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
