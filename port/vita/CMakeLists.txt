# PoP SoT (Xbox retail NTSC) para PS Vita: el C generado de rePOP
# (upstream/rePOP/source/xbox-retail/gen), su runtime compartido y un host Vita
# (src/), como port/android/CMakeLists.txt de upstream. Fase 3R del plan.
#
# El submódulo no se edita: tools/vitaize.py genera en el build las copias que
# cambian (memoria guest por la MMU por software de compat/vvm.h).
cmake_minimum_required(VERSION 3.16)
# vita.cmake de VitaSDK declara cmake_minimum_required < 3.5, que CMake 4 rechaza
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

# HOSTTEST=ON: el mismo programa para Linux ARM32 (hosttest/run.sh, Docker armv7)
# con un psp2 falso, para arrancar el juego y cazar crashes antes de la consola.
option(HOSTTEST "Compilar para Linux ARM32 con hosttest/psp2_fake.c" OFF)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT HOSTTEST)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define VITASDK (ej. export VITASDK=$HOME/vitasdk)")
  endif()
endif()

project(pop-sot C)
if(NOT HOSTTEST)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(VITA_APP_NAME "Prince of Persia: The Sands of Time")
set(VITA_TITLEID  "PSOT00001")
set(VITA_VERSION  "00.02")
set(DATA_DIR      "ux0:data/pop-sot")
if(HOSTTEST)
  set(DATA_DIR "/data")
endif()
set(TARGET_ID     "xbox-retail")

get_filename_component(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(REPOP "${PORT_ROOT}/upstream/rePOP")
set(RT    "${REPOP}/port/third_party/xboxrecomp/src")
set(TGT   "${REPOP}/source/${TARGET_ID}")
set(VZ    "${CMAKE_CURRENT_BINARY_DIR}/vitaize")

# data_manifest.txt (raíz del port) -> archivos que el host exige en DATA_DIR
file(STRINGS "${PORT_ROOT}/data_manifest.txt" _manifest REGEX "^[^#]" ENCODING UTF-8)
set(MANIFEST_ENTRIES "")
foreach(f IN LISTS _manifest)
  string(STRIP "${f}" f)
  string(APPEND MANIFEST_ENTRIES "  \"${f}\",\n")
endforeach()
configure_file(src/manifest.h.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.h" @ONLY)

# ── copias traducidas (vitaize.py) ─────────────────────────────
# DirectSound generado: sus accesos a registros de la APU pasan por
# compat/mmio_vita.h (la Vita no tiene trampas de página).
set(DSOUND_GEN source/${TARGET_ID}/gen/xdk/dsound/dsound_0000.c source/${TARGET_ID}/gen/xdk/dsound/dsound_0001.c)
set(VITAIZED
  kernel/xbox_memory_layout.c kernel/kernel_bridge.c kernel/kernel_rtl.c
  kernel/kernel_hal.c kernel/kernel_thunks.c
  kernel/nv2a_pb_exec.c nv2a/qemu_shim.h apu/apu_shim.h apu/apu_state.h
  apu/apu_core.c apu/apu_vp.c apu/apu_dsp.c apu/apu_mmio_hook.c
  port/game/recomp_manual.c port/game/guest_probe.h source/${TARGET_ID}/target_hooks.c
  platform/win32_compat.c kernel/guest_vmem.c port/windows/src/scene.c kernel/nv2a_hw_d3d11.inc
  kernel/kernel_path.c
  ${DSOUND_GEN})
set(_vz_inputs "${CMAKE_CURRENT_SOURCE_DIR}/tools/vitaize.py" "${TGT}/gen/recomp_types.h")
foreach(f IN LISTS VITAIZED)
  if(f MATCHES "^(port|source)/")
    list(APPEND _vz_inputs "${REPOP}/${f}")
  else()
    list(APPEND _vz_inputs "${RT}/${f}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_vz_inputs})
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/vitaize.py"
          "${REPOP}" "${VZ}" "${TARGET_ID}" ${VITAIZED}
  RESULT_VARIABLE _vz_result)
if(NOT _vz_result EQUAL 0)
  message(FATAL_ERROR "vitaize.py falló: upstream cambió, revisar las anclas")
endif()

# ── runtime compartido, como lo compila Android ────────────────
set(RUNTIME_SOURCES
  ${VZ}/rt/win32_compat.c
  ${VZ}/rt/kernel_path.c ${RT}/kernel/kernel_pool.c ${VZ}/rt/kernel_rtl.c
  ${RT}/kernel/kernel_memory.c ${RT}/kernel/kernel_file.c ${RT}/kernel/kernel_thread.c
  ${RT}/kernel/kernel_sync.c ${VZ}/rt/kernel_hal.c ${RT}/kernel/kernel_xbox.c
  ${RT}/kernel/kernel_ob.c ${RT}/kernel/kernel_io.c ${RT}/kernel/kernel_crypto.c
  ${VZ}/rt/kernel_thunks.c ${VZ}/rt/kernel_bridge.c ${VZ}/rt/xbox_memory_layout.c
  ${VZ}/rt/guest_vmem.c ${RT}/kernel/icall_feedback.c ${RT}/kernel/recomp_trace.c
  ${RT}/kernel/nv2a_pb_scan.c ${VZ}/rt/nv2a_pb_exec.c ${RT}/kernel/nv2a_vsh_interp.c
  ${RT}/kernel/nv2a_combiner.c
  ${RT}/audio/dsound_device.c ${RT}/audio/wma_decoder.c
  ${VZ}/rt/apu_core.c ${VZ}/rt/apu_vp.c ${VZ}/rt/apu_dsp.c ${VZ}/rt/apu_mmio_hook.c
  ${RT}/usb/ohci.c ${RT}/usb/usb_gamepad.c
  ${RT}/video/video_player.c ${RT}/video/video_pump.c)
  # video/fb_present.c no: su ventana es Win32/SDL; src/fb_vita.c la sustituye.

set(HOST_SOURCES
  src/main_vita.c src/fb_vita.c src/input_vita.c src/audio_vita.c compat/vvm.c compat/posix_vita.c compat/dispatch_cache_vita.c
  compat/mmio_vita.c
  ${REPOP}/port/android/src/options_android.c
  ${REPOP}/port/android/src/host_files.c
  ${REPOP}/port/android/src/sha256.c
  ${VZ}/rt/scene.c
  ${VZ}/rt/recomp_manual.c)

file(GLOB_RECURSE GEN_SOURCES CONFIGURE_DEPENDS "${TGT}/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "Sin C generado en ${TGT}/gen (git submodule update --init)")
endif()
foreach(f IN LISTS DSOUND_GEN)
  get_filename_component(_name "${f}" NAME)
  list(REMOVE_ITEM GEN_SOURCES "${REPOP}/${f}")
  list(APPEND GEN_SOURCES "${VZ}/gen/${_name}")
endforeach()

add_executable(${PROJECT_NAME} ${RUNTIME_SOURCES} ${HOST_SOURCES} ${VZ}/rt/target_hooks.c ${GEN_SOURCES})

# Las ramas __ANDROID__ del runtime son su camino POSIX completo (particiones
# emuladas que el retail necesita, rutas sin distinguir mayúsculas, pilas de
# 8 MB, sin pthread_cancel). Fuera quedan el layout (reserva 4 GB) y el
# ejecutor NV2A (incluiría el encoder SDL/Vulkan).
set_source_files_properties(
  ${VZ}/rt/win32_compat.c
  ${VZ}/rt/kernel_path.c ${RT}/kernel/kernel_file.c ${VZ}/rt/kernel_bridge.c
  ${VZ}/rt/guest_vmem.c ${RT}/kernel/nv2a_pb_scan.c ${VZ}/rt/target_hooks.c
  PROPERTIES COMPILE_DEFINITIONS "__ANDROID__=1")
# Su VirtualAlloc/vistas mmap se sustituyen por compat/vvm.c.
set_property(SOURCE ${VZ}/rt/win32_compat.c APPEND PROPERTY COMPILE_DEFINITIONS
  VirtualAlloc=w32u_VirtualAlloc VirtualFree=w32u_VirtualFree VirtualProtect=w32u_VirtualProtect
  CreateFileMappingA=w32u_CreateFileMappingA CreateFileMappingW=w32u_CreateFileMappingW
  MapViewOfFileEx=w32u_MapViewOfFileEx MapViewOfFile=w32u_MapViewOfFile
  UnmapViewOfFile=w32u_UnmapViewOfFile)
# xbox_GetMemoryOffset() lo da compat/vvm.c (desplazamiento de la ventana contigua).
# Backend GPU (V1.1): la parte compartida de nv2a_hw_d3d11.inc + src/nv2a_hw_vita.inc.
set_property(SOURCE ${VZ}/rt/nv2a_pb_exec.c APPEND PROPERTY COMPILE_DEFINITIONS RESOT_HW_VITA=1)
set_property(SOURCE ${VZ}/rt/xbox_memory_layout.c APPEND PROPERTY COMPILE_DEFINITIONS
  xbox_GetMemoryOffset=xbox_GetMemoryOffset_upstream
  __linux__=1)  # solo para la estructura EXCEPTION_POINTERS de win32_compat.h (sus VEH no saltan)

target_compile_definitions(${PROJECT_NAME} PRIVATE
  RESOT_RUNTIME_RETAIL=1 _GNU_SOURCE "RESOT_DEFAULT_ASPECT=\"16:9\""
  DATA_DIR="${DATA_DIR}" APP_SLUG="${PROJECT_NAME}")
target_include_directories(${PROJECT_NAME} PRIVATE
  "${VZ}/rt" "${VZ}" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}"
  "${TGT}" "${TGT}/gen"
  "${RT}" "${RT}/kernel" "${RT}/platform" "${RT}/apu" "${RT}/nv2a" "${RT}/usb"
  "${RT}/input" "${RT}/audio" "${RT}/video" "${RT}/d3d"
  "${REPOP}/port/third_party/xboxrecomp/include/xbox"
  "${REPOP}/port/android/src" "${REPOP}/port/windows/src")
# El código x86 elevado y el runtime asumen la aritmética de x86 (ver Android).
target_compile_options(${PROJECT_NAME} PRIVATE
  -fwrapv -fsigned-char -ffp-contract=off -fno-strict-aliasing -O2 -w)
# El C generado incluye "recomp_types.h" de su propia carpeta: se pre-incluye la
# copia con XBOX_PTR por vvm_ptr() y su guarda deja fuera la original. El
# runtime la encuentra antes por ${VZ} en la ruta de includes.
set_property(SOURCE ${GEN_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS RECOMP_GENERATED_CODE)
set_property(SOURCE ${GEN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -include "${VZ}/recomp_types.h")
if(HOSTTEST)
  # Solo los headers psp2 de VitaSDK (no su newlib): copiados por hosttest/run.sh.
  target_sources(${PROJECT_NAME} PRIVATE hosttest/psp2_fake.c hosttest/window_sdl.c)
  target_include_directories(${PROJECT_NAME} PRIVATE "${PSP2_INCLUDE}")
  target_compile_options(${PROJECT_NAME} PRIVATE -mthumb -mfpu=neon)   # como la Vita (Cortex-A9)
  # Arena grande para medir el pico de EXT_VMA que la Vita debe cubrir (vvm.c).
  set_property(SOURCE compat/vvm.c APPEND PROPERTY COMPILE_DEFINITIONS VVM_EXT_ARENA_MB=160)
  # GL de Mesa (llvmpipe) en lugar de vitaGL; shaders en GLSL (nv2a_hw_vita.inc)
  set_property(SOURCE ${VZ}/rt/nv2a_pb_exec.c APPEND PROPERTY COMPILE_DEFINITIONS RESOT_HW_GLSL=1)
  target_sources(${PROJECT_NAME} PRIVATE hosttest/vitagl_fake.c)
  target_link_libraries(${PROJECT_NAME} -Wl,--wrap=recomp_lookup SDL2 EGL OpenGL pthread m)
  return()
endif()
# vitaGL vendorizado (vendor/vitaGL/VENDORED.md, el mismo árbol que Skater Nation):
# ABI hard-float de VitaSDK, sin logs de depuración y con caché de shaders CG
# compilados en ux0:data (el generador de combiners crea decenas por escena).
set(VITAGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitaGL")
include(ExternalProject)
ExternalProject_Add(vitaGL_vendored
  SOURCE_DIR "${VITAGL_DIR}" BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND "" INSTALL_COMMAND ""
  BUILD_COMMAND make NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 LOG_ERRORS=1
  BUILD_BYPRODUCTS "${VITAGL_DIR}/libvitaGL.a")
add_library(vitaGL_lib STATIC IMPORTED)
set_target_properties(vitaGL_lib PROPERTIES IMPORTED_LOCATION "${VITAGL_DIR}/libvitaGL.a")
add_dependencies(${PROJECT_NAME} vitaGL_vendored)
set_property(SOURCE ${VZ}/rt/nv2a_pb_exec.c APPEND PROPERTY INCLUDE_DIRECTORIES "${VITAGL_DIR}/source")
target_sources(${PROJECT_NAME} PRIVATE compat/threads_vita.c compat/log_vita.c)   # solo Vita: [THR], afinidad y log x32
target_link_libraries(${PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  -Wl,--pic-veneer -Wl,--wrap=recomp_lookup -Wl,--wrap=pthread_create -Wl,--wrap=CreateThread
  SceDisplay_stub SceSysmem_stub ScePower_stub
  vitaGL_lib vitashark SceShaccCgExt taihen_stub SceShaccCg_stub mathneon
  SceKernelThreadMgr_stub SceIofilemgr_stub SceRtc_stub SceAppUtil_stub SceTouch_stub SceAudio_stub SceAppMgr_stub SceCtrl_stub
  SceGxm_stub SceKernelDmacMgr_stub SceCommonDialog_stub SceDisplay_stub
  m stdc++)  # stdc++ después de vitaGL: es C++ por dentro

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
# ATTRIBUTE2=12: memoria extendida (el bloque guest + la ventana contigua son 176 MB).
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/livearea/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/livearea/pic0.png sce_sys/pic0.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/livearea/bg0.png sce_sys/livearea/contents/bg0.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/livearea/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_CURRENT_SOURCE_DIR}/livearea/template.xml sce_sys/livearea/contents/template.xml)
