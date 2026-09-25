cmake_minimum_required(VERSION 3.21)
project(LambdaPlayer VERSION 0.2.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets OpenGLWidgets Quick WebEngineWidgets WebChannel)

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
  src/addonconfigurewindow.cpp
  src/addonconfigurewindow.h
  src/addonsbridge.cpp
  src/addonsbridge.h
  src/stremiobackend.cpp
  src/stremiobackend.h
  src/homepage.cpp
  src/homepage.h
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
target_link_libraries(LambdaPlayer PRIVATE lambda_stremio Qt6::Widgets Qt6::OpenGLWidgets Qt6::Quick Qt6::WebEngineWidgets Qt6::WebChannel "${MPV_LIBRARY}")

set(VUI_RESOURCE_FILES
  resources/vui/index.html
  resources/vui/home.css
  resources/vui/addons.css
  resources/vui/stremio.js
  resources/vui/player.html
  resources/vui/styles.css
  resources/vui/player.js
  resources/vui/home.js
  resources/vui/window.js
  resources/vui/window.css
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/vui/InterVariable.woff2")
  list(APPEND VUI_RESOURCE_FILES resources/vui/InterVariable.woff2)
endif()

qt_add_resources(LambdaPlayer "vui_home"
  PREFIX "/vui"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/vui"
  FILES ${VUI_RESOURCE_FILES}
)

qt_add_resources(LambdaPlayer "frc_shaders"
  PREFIX "/frc"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/frc"
  FILES
    resources/frc/generic_motion.hlsl
    resources/frc/generic_warp.hlsl
    resources/frc/generic_copy.hlsl
)

target_compile_definitions(LambdaPlayer PRIVATE LAMBDA_VERSION="${PROJECT_VERSION}")

if(WIN32)
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

option(LAMBDA_BUILD_TESTS "Build the Stremio addon protocol tests" ON)
if(LAMBDA_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)
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
