# Fase 0: eboot "smoke" que valida toolchain, VPK, rutas de datos y libshacccg en la Vita real.
# En fases posteriores este mismo CMakeLists crece con port/vita/src (capa de plataforma)
# y el código del juego (source/), nunca al revés.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define VITASDK (ej. export VITASDK=$HOME/vitasdk)")
  endif()
endif()

project(pop-sot C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Prince of Persia: The Sands of Time")
set(VITA_TITLEID  "PSOT00001")
set(VITA_VERSION  "00.01")
set(DATA_DIR      "ux0:data/pop-sot")

# data_manifest.txt (raíz del port) -> lista de archivos que el smoke exige en DATA_DIR
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../data_manifest.txt" _manifest REGEX "^[^#]")
set(MANIFEST_ENTRIES "")
foreach(f IN LISTS _manifest)
  string(STRIP "${f}" f)
  string(APPEND MANIFEST_ENTRIES "  \"${f}\",\n")
endforeach()
configure_file(src/manifest.h.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.h" @ONLY)

add_executable(${PROJECT_NAME} src/main.c)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(${PROJECT_NAME} PRIVATE DATA_DIR="${DATA_DIR}" APP_SLUG="${PROJECT_NAME}")
target_compile_options(${PROJECT_NAME} PRIVATE -O2 -Wall)
target_link_libraries(${PROJECT_NAME}
  vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon m
  SceAppMgr_stub SceCtrl_stub SceDisplay_stub SceGxm_stub SceKernelDmacMgr_stub SceIofilemgr_stub)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME})
