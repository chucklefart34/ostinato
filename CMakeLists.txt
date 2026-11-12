cmake_minimum_required(VERSION 3.22)

# Ostinato — mobile-first DAW. One codebase for Windows, macOS, Linux, iOS and Android.
#
#   Windows : cmake -B build -G "Visual Studio 17 2022" && cmake --build build --config Release
#   macOS   : cmake -B build -G Xcode && cmake --build build --config Release
#   iOS     : cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
#             then open build-ios/Ostinato.xcodeproj, pick your signing team, run on the iPhone.
#   Linux   : cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

project(Ostinato VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use a local checkout if given (-DJUCE_DIR=...), otherwise download the pinned release.
set(JUCE_DIR "" CACHE PATH "Path to a JUCE checkout (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce)
else()
    include(FetchContent)
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.5 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

juce_add_gui_app(Ostinato
    PRODUCT_NAME "Ostinato"
    COMPANY_NAME "Ostinato"
    BUNDLE_ID "com.ostinato.daw"
    VERSION ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Ostinato records vocals and instruments from your microphone or audio interface."
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "Ostinato connects to Bluetooth MIDI keyboards and controllers."
    FILE_SHARING_ENABLED TRUE          # exports and projects appear in the iOS Files app
    DOCUMENT_BROWSER_ENABLED TRUE
    REQUIRES_FULL_SCREEN TRUE
    STATUS_BAR_HIDDEN FALSE
    BACKGROUND_AUDIO_ENABLED TRUE      # allowed, but the app stops audio in background unless the user turns it on in Settings
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    ANDROID_MIN_SDK 26
    ANDROID_TARGET_SDK 34)

juce_add_binary_data(OstinatoData SOURCES
    Resources/Fonts/Inter-Regular.otf
    Resources/Fonts/Inter-SemiBold.otf)

file(GLOB_RECURSE OSTINATO_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h)
if(APPLE)
    file(GLOB OSTINATO_APPLE ${CMAKE_CURRENT_SOURCE_DIR}/Source/Platform/*.mm)
    list(APPEND OSTINATO_SOURCES ${OSTINATO_APPLE})
endif()
target_sources(Ostinato PRIVATE ${OSTINATO_SOURCES})
target_include_directories(Ostinato PRIVATE Source)

target_compile_definitions(Ostinato PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    $<$<NOT:$<OR:$<PLATFORM_ID:iOS>,$<PLATFORM_ID:Android>>>:JUCE_MODAL_LOOPS_PERMITTED=1>
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Ostinato,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Ostinato,JUCE_VERSION>")

option(OSTINATO_HEADLESS_LINUX "Build without ALSA/JACK and optional X11 extensions (CI/test machines)" OFF)
if(OSTINATO_HEADLESS_LINUX)
    target_compile_definitions(Ostinato PRIVATE JUCE_ALSA=0 JUCE_JACK=0
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0 JUCE_USE_XINPUT=0)
endif()

target_link_libraries(Ostinato
    PRIVATE
        OstinatoData
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
