cmake_minimum_required(VERSION 3.15)
project(ExwordBtuProof VERSION 0.1 LANGUAGES C ASM)

if(NOT FXSDK_PLATFORM_LONG STREQUAL "exword")
  message(FATAL_ERROR "Use the EXWORD.cmake toolchain")
endif()

include(GenerateD01)
include(GenerateEXWORDPackage)
find_package(Gint 2.11 REQUIRED)

add_executable(btuproof
  src/main.c
  src/btu.c
  src/btu_entry.S
  src/probes.c)
target_compile_features(btuproof PRIVATE c_std_11)
target_compile_options(btuproof PRIVATE
  -Wall -Wextra -Os -flto=auto -fno-pic -fno-pie
  -fno-stack-protector -ffunction-sections -fdata-sections)
target_link_options(btuproof PRIVATE
  -flto=auto -nostdlib -no-pie -Wl,--build-id=none
  -Wl,--no-warn-rwx-segments -Wl,-Map=btuproof.map
  -Wl,--print-memory-usage)
target_link_libraries(btuproof Gint::Gint)

generate_d01(TARGET btuproof
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/btuproof.d01"
  MODULE "btup")

if(NOT EXWORD_METADATA_SOURCE)
  message(FATAL_ERROR "Set EXWORD_METADATA_SOURCE to the EX-word metadata repo")
endif()
generate_exword_package(
  TARGET btuproof
  D01 "${CMAKE_CURRENT_BINARY_DIR}/btuproof.d01"
  APPID "BTPRF"
  TITLE "SH7305 BTU proof"
  HTML_DIR "${EXWORD_METADATA_SOURCE}/html"
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/packages"
  LOCALES ja cn)
