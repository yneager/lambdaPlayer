cmake_minimum_required(VERSION 3.21)
project(LambdaPlayer VERSION 0.3.2 LANGUAGES CXX)
set(LAMBDA_RELEASE_TAG "v${PROJECT_VERSION}" CACHE STRING "Release tag used for update comparison")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets Concurrent OpenGLWidgets Quick WebEngineWidgets WebChannel)

# Stremio addon protocol client (no GUI dependencies; shared with the tests).
qt_add_library(lambda_stremio STATIC
  src/stremio/addonclient.cpp
  src/stremio/addonclient.h
  src/stremio/addonmanager.cpp
  src/stremio/addonmanager.h
  src/stremio/addonurl.cpp
  src/stremio/addonurl.h
  src/stremio/capabilities.cpp
  src/stremio/capabilities.h
  src/stremio/common.cpp
  src/stremio/common.h
  src/stremio/contentservice.cpp
  src/stremio/contentservice.h
  src/stremio/json.cpp
  src/stremio/language.cpp
  src/stremio/language.h
  src/stremio/json.h
  src/stremio/legacytransport.cpp
  src/stremio/legacytransport.h
  src/stremio/lzstring.cpp
  src/stremio/lzstring.h
  src/stremio/manifest.cpp
  src/stremio/manifest.h
  src/stremio/resources.cpp
  src/stremio/resources.h
  src/stremio/serverprocess.cpp
  src/stremio/serverprocess.h
  src/stremio/streamresolver.cpp
  src/stremio/streamresolver.h
  src/stremio/transport.cpp
  src/stremio/transport.h
  src/stremio/videoparams.cpp
  src/stremio/videoparams.h
)
target_include_directories(lambda_stremio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lambda_stremio PUBLIC Qt6::Core Qt6::Network)
if(MSVC)
  target_compile_options(lambda_stremio PRIVATE /utf-8)
endif()

set(MPV_ROOT "" CACHE PATH "Path to libmpv development package")
if(NOT MPV_INCLUDE_DIR)
  find_path(MPV_INCLUDE_DIR NAMES mpv/client.h HINTS "${MPV_ROOT}/include")
endif()
if(NOT MPV_LIBRARY)
  find_library(MPV_LIBRARY NAMES mpv mpv-2 mpv-1 HINTS "${MPV_ROOT}/lib")
endif()
if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
  message(FATAL_ERROR "libmpv development files not found.")
endif()

qt_add_executable(LambdaPlayer WIN32
  src/main.cpp
  src/updatechecker.cpp
  src/updatechecker.h
  src/addonconfigurewindow.cpp
  src/addonconfigurewindow.h
  src/addonsbridge.cpp
  src/addonsbridge.h
  src/stremiobackend.cpp
  src/stremiobackend.h
  src/locallibrary.cpp
  src/locallibrary.h
  src/homepage.cpp
  src/homepage.h
  src/downloadsbridge.cpp
  src/downloadsbridge.h
  src/playerchrome.cpp
  src/playerchrome.h
  src/mainwindow.cpp
  src/mainwindow.h
  src/mpvvideowidget.cpp
  src/mpvvideowidget.h
  src/windowbridge.cpp
  src/windowbridge.h
  src/interpolationcontroller.cpp
  src/interpolationcontroller.h
  src/frc/amffrcinterpolator.cpp
  src/frc/amffrcinterpolator.h
  src/frc/frameinterpolator.h
  src/frc/genericd3d11fruc.cpp
  src/frc/genericd3d11fruc.h
  src/frc/rifed3d11.cpp
  src/frc/rifed3d11.h
  src/frc/computeshaders.cpp
  src/frc/computeshaders.h
  src/frc/d3d11videoprocessorprobe.cpp
  src/frc/d3d11videoprocessorprobe.h
  src/frc/screeninterpolationcontroller.cpp
  src/frc/screeninterpolationcontroller.h
  src/frc/wgldxinterop.cpp
  src/frc/wgldxinterop.h
  # AMD AMF SDK (MIT): runtime loader and helpers used by AMFFactory.
  third_party/amf/public/common/AMFFactory.cpp
  third_party/amf/public/common/AMFSTL.cpp
  third_party/amf/public/common/Thread.cpp
  third_party/amf/public/common/TraceAdapter.cpp
  third_party/amf/public/common/Windows/ThreadWindows.cpp
)

target_include_directories(LambdaPlayer PRIVATE "${MPV_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
                           "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf")
target_link_libraries(LambdaPlayer PRIVATE lambda_stremio Qt6::Concurrent Qt6::Widgets Qt6::OpenGLWidgets Qt6::Quick Qt6::WebEngineWidgets Qt6::WebChannel "${MPV_LIBRARY}")

set(VUI_RESOURCE_FILES
  resources/vui/index.html
  resources/vui/home.css
  resources/vui/addons.css
  resources/vui/downloads.css
  resources/vui/downloads.js
  resources/vui/motrix-client.js
  resources/vui/stremio.js
  resources/vui/player.html
  resources/vui/styles.css
  resources/vui/player.js
  resources/vui/home.js
  resources/vui/window.js
  resources/vui/release-highlights.js
  resources/vui/preferences.js
  resources/vui/preferences.css
  resources/vui/frame-clock.js
  resources/vui/window.css
  resources/vui/motion.css
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/vui/InterVariable.woff2")
  list(APPEND VUI_RESOURCE_FILES resources/vui/InterVariable.woff2)
endif()

qt_add_resources(LambdaPlayer "vui_home"
  PREFIX "/vui"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/vui"
  FILES ${VUI_RESOURCE_FILES}
)

qt_add_resources(LambdaPlayer "download_config" PREFIX "/downloads"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/downloads"
  FILES resources/downloads/aria2.conf)

qt_add_resources(LambdaPlayer "release_notes" PREFIX "/releases"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/docs/releases"
  FILES docs/releases/v0.3.0.md docs/releases/v0.3.1.md docs/releases/v0.3.2.md)

qt_add_resources(LambdaPlayer "frc_shaders"
  PREFIX "/frc"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/frc"
  FILES
    resources/frc/generic_downscale.hlsl
    resources/frc/generic_motion.hlsl
    resources/frc/generic_cleanup.hlsl
    resources/frc/generic_warp.hlsl
    resources/frc/generic_scene.hlsl
    resources/frc/generic_copy.hlsl
    resources/frc/rife_ops.hlsl
    resources/frc/rife_io.hlsl
)

target_compile_definitions(LambdaPlayer PRIVATE LAMBDA_VERSION="${PROJECT_VERSION}")
target_compile_definitions(LambdaPlayer PRIVATE LAMBDA_RELEASE_TAG="${LAMBDA_RELEASE_TAG}")

if(WIN32)
  enable_language(RC)
  configure_file(resources/branding/windows.rc.in "${CMAKE_CURRENT_BINARY_DIR}/windows.rc" @ONLY)
  target_sources(LambdaPlayer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/windows.rc")
  target_compile_definitions(LambdaPlayer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # dwmapi: rounded corners / shadow for the custom-framed window.
  target_link_libraries(LambdaPlayer PRIVATE dwmapi)
  # AMD FRC backend: Direct3D 11 + DXGI; AMF itself is loaded at runtime.
  target_link_libraries(LambdaPlayer PRIVATE d3d11 dxgi winmm ole32 opengl32)
  target_link_libraries(LambdaPlayer PRIVATE d3dcompiler)
endif()

if(MSVC)
  # Sources are UTF-8.
  target_compile_options(LambdaPlayer PRIVATE /utf-8)
endif()

set_target_properties(LambdaPlayer PROPERTIES OUTPUT_NAME "LambdaPlayer")
if(WIN32)
  set(LAMBDA_DOWNLOAD_ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/tools/extra/win32/x64/aria2c.exe")
  if(EXISTS "${LAMBDA_DOWNLOAD_ENGINE}")
    add_custom_command(TARGET LambdaPlayer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LAMBDA_DOWNLOAD_ENGINE}" "$<TARGET_FILE_DIR:LambdaPlayer>/aria2c.exe")
  else()
    message(WARNING "Downloads requires: node tools/download-engine/fetch-engine.mjs --platform win32 --arch x64")
  endif()
endif()
qt_add_resources(LambdaPlayer "branding"
  PREFIX "/branding"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding"
  FILES resources/branding/lambda.ico resources/branding/lambda.svg)
qt_add_resources(LambdaPlayer "updater" PREFIX "/updater"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/updater" FILES resources/updater/update.ps1)

# FRC compute shaders are compiled at build time with fxc (Windows SDK) and
# embedded as :/frc/cso/<file>_<entry>.cso. The RIFE kernels take ~25 s to
# compile, so runtime D3DCompile is only a fallback when fxc is unavailable.
set(FRC_SHADER_ENTRIES
  generic_scene:main generic_downscale:main generic_motion:main generic_cleanup:main generic_warp:main generic_copy:main
  rife_ops:conv3x3 rife_ops:deconv rife_ops:deconv4x4s2 rife_ops:interp rife_ops:binary
  rife_ops:activation rife_ops:copy rife_ops:pixelShuffle rife_ops:warp
  rife_io:prepare rife_io:sceneDiff rife_io:packMotion rife_io:synthesize
)
file(GLOB LAMBDA_SDK_FXC_DIRS "C:/Program Files (x86)/Windows Kits/10/bin/*/x64")
list(SORT LAMBDA_SDK_FXC_DIRS)
list(REVERSE LAMBDA_SDK_FXC_DIRS)
find_program(LAMBDA_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" ${LAMBDA_SDK_FXC_DIRS})
set(FRC_CSO_FILES)
if(WIN32 AND LAMBDA_FXC)
  foreach(item IN LISTS FRC_SHADER_ENTRIES)
    string(REPLACE ":" ";" parts "${item}")
    list(GET parts 0 shader_file)
    list(GET parts 1 shader_entry)
    set(cso "${CMAKE_CURRENT_BINARY_DIR}/frc_cso/${shader_file}_${shader_entry}.cso")
    add_custom_command(OUTPUT "${cso}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/frc_cso"
      COMMAND "${LAMBDA_FXC}" /nologo /T cs_5_0 /E ${shader_entry} /O3 /Fo "${cso}"
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/frc/${shader_file}.hlsl"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/frc/${shader_file}.hlsl"
      COMMENT "fxc ${shader_file}.hlsl:${shader_entry}"
      VERBATIM)
    list(APPEND FRC_CSO_FILES "${cso}")
  endforeach()
  add_custom_target(frc_cso DEPENDS ${FRC_CSO_FILES})
  qt_add_resources(LambdaPlayer "frc_cso"
    PREFIX "/frc/cso"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/frc_cso"
    FILES ${FRC_CSO_FILES}
  )
  add_dependencies(LambdaPlayer frc_cso)
else()
  message(WARNING "fxc not found: FRC compute shaders will be compiled at runtime (slow first use)")
endif()

# Offline interpolation bench (validation/quality/timing on saved frames);
# not deployed with the app.
option(LAMBDA_BUILD_FRC_BENCH "Build the frame-interpolation bench tool" ON)
if(WIN32 AND LAMBDA_BUILD_FRC_BENCH)
  qt_add_executable(frc_bench
    tools/frc-bench/main.cpp
    src/frc/amffrcinterpolator.cpp
    src/frc/genericd3d11fruc.cpp
    src/frc/rifed3d11.cpp
    src/frc/computeshaders.cpp
    third_party/amf/public/common/AMFFactory.cpp
    third_party/amf/public/common/AMFSTL.cpp
    third_party/amf/public/common/Thread.cpp
    third_party/amf/public/common/TraceAdapter.cpp
    third_party/amf/public/common/Windows/ThreadWindows.cpp
  )
  target_include_directories(frc_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf")
  target_compile_definitions(frc_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(frc_bench PRIVATE Qt6::Gui d3d11 dxgi d3dcompiler winmm ole32)
  qt_add_resources(frc_bench "frc_bench_shaders"
    PREFIX "/frc"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/frc"
    FILES
      resources/frc/generic_downscale.hlsl
      resources/frc/generic_motion.hlsl
      resources/frc/generic_cleanup.hlsl
      resources/frc/generic_warp.hlsl
      resources/frc/generic_scene.hlsl
    resources/frc/generic_copy.hlsl
      resources/frc/rife_ops.hlsl
      resources/frc/rife_io.hlsl
  )
  if(FRC_CSO_FILES)
    qt_add_resources(frc_bench "frc_bench_cso"
      PREFIX "/frc/cso"
      BASE "${CMAKE_CURRENT_BINARY_DIR}/frc_cso"
      FILES ${FRC_CSO_FILES}
    )
    add_dependencies(frc_bench frc_cso)
  endif()
  if(MSVC)
    target_compile_options(frc_bench PRIVATE /utf-8)
  endif()
endif()

option(LAMBDA_BUILD_TESTS "Build the Stremio addon protocol tests" ON)
if(LAMBDA_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)
  qt_add_executable(tst_shortcuts tests/tst_shortcuts.cpp src/windowbridge.cpp src/windowbridge.h)
  target_include_directories(tst_shortcuts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(tst_shortcuts PRIVATE Qt6::Widgets Qt6::Test)
  add_test(NAME shortcuts COMMAND tst_shortcuts)
  qt_add_executable(tst_mpvglstate tests/tst_mpvglstate.cpp)
  target_include_directories(tst_mpvglstate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(tst_mpvglstate PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
  add_test(NAME mpvglstate COMMAND tst_mpvglstate)
  qt_add_executable(tst_videopresentation tests/tst_videopresentation.cpp)
  target_include_directories(tst_videopresentation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(tst_videopresentation PRIVATE Qt6::Test)
  add_test(NAME videopresentation COMMAND tst_videopresentation)
  qt_add_executable(tst_locallibrary tests/tst_locallibrary.cpp src/locallibrary.cpp src/locallibrary.h)
  target_include_directories(tst_locallibrary PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(tst_locallibrary PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Test)
  if(MSVC)
    target_compile_options(tst_locallibrary PRIVATE /utf-8)
  endif()
  add_test(NAME locallibrary COMMAND tst_locallibrary)
  qt_add_executable(tst_updatechecker tests/tst_updatechecker.cpp src/updatechecker.cpp src/updatechecker.h)
  qt_add_resources(tst_updatechecker "test_release_notes" PREFIX "/releases"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/docs/releases" FILES docs/releases/v0.3.0.md docs/releases/v0.3.1.md docs/releases/v0.3.2.md)
  target_include_directories(tst_updatechecker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(tst_updatechecker PRIVATE Qt6::Gui Qt6::Network Qt6::Test)
  target_compile_definitions(tst_updatechecker PRIVATE LAMBDA_UPDATE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/updater/tls")
  if(MSVC)
    target_compile_options(tst_updatechecker PRIVATE /utf-8)
  endif()
  add_test(NAME updatechecker COMMAND tst_updatechecker)
  if(WIN32)
    add_executable(update_probe WIN32 tests/updater/probe.cpp)
    set_target_properties(update_probe PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_test(NAME update_transaction COMMAND powershell -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/updater/run-tests.ps1"
      -Probe $<TARGET_FILE:update_probe> -Updater "${CMAKE_CURRENT_SOURCE_DIR}/resources/updater/update.ps1"
      -Root "${CMAKE_CURRENT_BINARY_DIR}/update-transaction-tests")
  endif()
  set(LAMBDA_TESTS
    addonclient
    addonmanager
    addonurl
    capabilities
    manifest
    resources
    streams
    transport
  )
  foreach(test_name IN LISTS LAMBDA_TESTS)
    qt_add_executable(tst_${test_name} tests/stremio/tst_${test_name}.cpp tests/stremio/testutil.h tests/stremio/mockaddonserver.h)
    target_include_directories(tst_${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/stremio")
    target_link_libraries(tst_${test_name} PRIVATE lambda_stremio Qt6::Test)
    target_compile_definitions(tst_${test_name} PRIVATE LAMBDA_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/stremio/data")
    if(MSVC)
      target_compile_options(tst_${test_name} PRIVATE /utf-8)
    endif()
    add_test(NAME ${test_name} COMMAND tst_${test_name})
  endforeach()
endif()
