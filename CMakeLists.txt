cmake_minimum_required(VERSION 3.18)

if(DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE FILEPATH "VitaSDK CMake toolchain")
endif()

project(vitashelflauncher C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(vitashelflauncher_core src/core/vitashelflauncher.c)
target_include_directories(vitashelflauncher_core PUBLIC include)
target_compile_options(vitashelflauncher_core PRIVATE -Wall -Wextra -Werror)

if(DEFINED ENV{VITASDK})
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
  set(VITA_APP_NAME "VitaShelf Launcher")
  set(VITA_TITLEID "VSHELF001")
  set(VITA_VERSION "01.00")

  add_executable(vitashelflauncher
    src/vita/main.c
    src/vita/catalog_cache.c
    src/vita/ui_audio.c
    src/vita/ui_text.c
    src/vita/ui_backdrop.c
    src/vita/ui_presentation.c
    src/vita/ui_renderer.c
    src/vita/vita_backend.c)
  target_compile_definitions(vitashelflauncher PRIVATE VP_APP_VERSION="${VITA_VERSION}")
  # Catch large automatic buffers before they can exhaust a Vita thread stack.
  target_compile_options(vitashelflauncher PRIVATE -Werror=frame-larger-than=16384)
  target_link_libraries(vitashelflauncher vitashelflauncher_core vita2d freetype png jpeg curl zip z zstd bz2 lzma ssl crypto m pthread
    SceCtrl_stub SceTouch_stub SceDisplay_stub SceGxm_stub SceLibKernel_stub SceAppMgr_stub SceCommonDialog_stub SceIme_stub
    ScePgf_stub SceIofilemgr_stub SceNet_stub SceNetCtl_stub SceSsl_stub SceSysmodule_stub SceAudio_stub
    ScePromoterUtil_stub_weak SceRtc_stub ScePower_stub)
  configure_file("$ENV{VITASDK}/arm-vita-eabi/ssl/openssl.cnf"
    "${CMAKE_CURRENT_BINARY_DIR}/openssl.cnf" COPYONLY)
  vita_create_self(eboot.bin vitashelflauncher UNSAFE)
  # Explicit inventory: every mapped system needs both its original logo and banner.
  # Only runtime resources are packaged; source vectors and previews stay in the repo.
  set(VP_BLUEGLASS_LOGOS
    platform_nes platform_famicom platform_snes platform_gameboy platform_gba platform_nds
    platform_genesis platform_mastersystem platform_gamegear platform_psx platform_psp platform_vita
    platform_arcade platform_pce platform_atari2600 platform_wonderswan platform_homebrew platform_generic
    platform_dreamcast platform_n3ds platform_gamecube platform_ps2 platform_wii
    platform_gameboycolor platform_neogeo)
  set(VP_BLUEGLASS_UI emblem focus console_surface console_focus ui_library ui_home ui_store ui_settings ui_download
    ui_check ui_star ui_sync ui_key ui_folder ui_general ui_interface ui_storage ui_info ui_server ui_psx ui_romm ui_buttons)
  set(VP_BLUEGLASS_FILES)
  foreach(asset IN LISTS VP_BLUEGLASS_LOGOS)
    list(APPEND VP_BLUEGLASS_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/logos/${asset}.png" "assets/blueglass/logos/${asset}.png")
    string(REPLACE "platform_" "header_" header "${asset}")
    list(APPEND VP_BLUEGLASS_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/headers/${header}.png" "assets/blueglass/headers/${header}.png")
    string(REPLACE "platform_" "" system "${asset}")
    list(APPEND VP_BLUEGLASS_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/backgrounds/${system}.png" "assets/blueglass/backgrounds/${system}.png")
  endforeach()
  foreach(asset IN LISTS VP_BLUEGLASS_UI)
    list(APPEND VP_BLUEGLASS_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/ui/${asset}.png" "assets/blueglass/ui/${asset}.png")
  endforeach()
  foreach(weight Regular Medium SemiBold Bold)
    list(APPEND VP_BLUEGLASS_FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/fonts/Inter-${weight}.ttf" "assets/blueglass/fonts/Inter-${weight}.ttf")
  endforeach()
  vita_create_vpk(vitashelflauncher.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
     NAME ${VITA_APP_NAME}
     FILE ${CMAKE_CURRENT_SOURCE_DIR}/themes/default/theme.ini assets/default/theme.ini
            ${CMAKE_CURRENT_BINARY_DIR}/openssl.cnf openssl.cnf
             ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/backgrounds/general.png assets/blueglass/backgrounds/general.png
             ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/fonts/OFL.txt assets/blueglass/fonts/OFL.txt
             ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/sources/Carbon-README.txt assets/licenses/Carbon-README.txt
              ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/NOTICE.md assets/licenses/Blueglass-NOTICE.md
              ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/sources/RomM-AGPL-3.0.txt assets/licenses/RomM-AGPL-3.0.txt
              ${CMAKE_CURRENT_SOURCE_DIR}/assets/blueglass/sources/romm-isotipo.svg assets/licenses/romm-isotipo.svg
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/NOTICE.md assets/licenses/Audio-NOTICE.md
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/01-navegacion-alternativa.wav assets/audio/01-navegacion-alternativa.wav
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/02-cambio-tab.wav assets/audio/02-cambio-tab.wav
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/03-entrar.wav assets/audio/03-entrar.wav
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/04-volver-salir.wav assets/audio/04-volver-salir.wav
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/05-iniciar-descarga.wav assets/audio/05-iniciar-descarga.wav
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/06-descarga-completa.wav assets/audio/06-descarga-completa.wav
            ${VP_BLUEGLASS_FILES})
else()
  message(STATUS "VITASDK not set: only vitashelflauncher_core is configured")
endif()
