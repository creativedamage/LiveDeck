cmake_minimum_required(VERSION 3.22)
project(LiveDeck VERSION 0.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if given (-DOMR_JUCE_DIR=/path/to/JUCE),
# otherwise download JUCE 8.0.4.
# ---------------------------------------------------------------------------
set(OMR_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
if(OMR_JUCE_DIR)
    add_subdirectory(${OMR_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# Optional Steinberg ASIO SDK (Windows). Download from steinberg.net and pass
# -DOMR_ASIO_SDK_DIR=C:/sdk/asiosdk_2.3.3 ; SoundGrid and Dante DVS on Windows
# are only reachable through ASIO or WASAPI.
set(OMR_ASIO_SDK_DIR "" CACHE PATH "Path to the Steinberg ASIO SDK (Windows only)")

option(OMR_BUILD_TESTS "Build the console test runner" ON)

set(OMR_CORE_SOURCES
    Source/Core/PlatformFile.cpp
    Source/Core/BwfWriter.cpp
    Source/Core/Recovery.cpp
    Source/Core/RecordTarget.cpp
    Source/Core/RecordEngine.cpp
    Source/Core/PatchModel.cpp
    Source/Core/Session.cpp
    Source/Core/Benchmark.cpp
    Source/Core/EventLog.cpp
    Source/Core/Player.cpp
    Source/Core/PeakStore.cpp
    Source/Core/Updater.cpp
    Source/Core/Remote.cpp
    Source/Core/AudioImport.cpp
    Source/Core/Feedback.cpp
    Source/Core/License.cpp)

# ---------------------------------------------------------------------------
# GitHub repository used for in-app updates ("owner/repo"). Taken from
# -DLIVEDECK_GITHUB_REPO=..., else from the git remote "origin", else empty
# (update checks are then disabled). CI passes $GITHUB_REPOSITORY.
# ---------------------------------------------------------------------------
set(LIVEDECK_GITHUB_REPO "" CACHE STRING "GitHub owner/repo for update checks")
if(NOT LIVEDECK_GITHUB_REPO)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} remote get-url origin
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        OUTPUT_VARIABLE _origin OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_origin MATCHES "github\\.com[:/]([^/]+/[^/]+)$")
            string(REGEX REPLACE "\\.git$" "" LIVEDECK_GITHUB_REPO "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()
message(STATUS "LiveDeck updates from GitHub repo: '${LIVEDECK_GITHUB_REPO}'")

# ---------------------------------------------------------------------------
# Desktop app
# ---------------------------------------------------------------------------
juce_add_gui_app(LiveDeck
    PRODUCT_NAME "LiveDeck"
    COMPANY_NAME "Wayne Goodman Productions"
    BUNDLE_ID com.waynegoodman.livedeck
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    DOCUMENT_EXTENSIONS livedeck ldtemplate
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "LiveDeck records audio from your interfaces (SoundGrid, Dante, MADI, USB)."
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
    PLIST_TO_MERGE "<plist><dict><key>LSApplicationCategoryType</key><string>public.app-category.music</string><key>NSHumanReadableCopyright</key><string>Wayne Goodman Productions</string><key>LSMinimumSystemVersion</key><string>11.0</string><key>NSSupportsAutomaticGraphicsSwitching</key><true/></dict></plist>")

target_sources(LiveDeck PRIVATE
    ${OMR_CORE_SOURCES}
    Source/Audio/AudioEngine.cpp
    Source/UI/ChannelStrip.cpp
    Source/UI/TimelineView.cpp
    Source/UI/RemoteControl.cpp
    Source/UI/FeedbackDialog.cpp
    Source/UI/Account.cpp
    Source/UI/AccountUI.cpp
    Source/UI/TransportBar.cpp
    Source/UI/MainComponent.cpp
    Source/Main.cpp)

target_include_directories(LiveDeck PRIVATE Source)

target_compile_definitions(LiveDeck PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LiveDeck,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LiveDeck,JUCE_VERSION>"
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_JACK=0)
# Accounts + licenses (sign-in and a license check at every launch). OFF only for development builds.
option(LIVEDECK_LICENSING "Require a LiveDeck account and license" ON)
target_compile_definitions(LiveDeck PRIVATE LIVEDECK_LICENSING=$<BOOL:${LIVEDECK_LICENSING}>)

if(LIVEDECK_GITHUB_REPO)
    target_compile_definitions(LiveDeck PRIVATE LIVEDECK_GITHUB_REPO="${LIVEDECK_GITHUB_REPO}")
endif()

if(WIN32 AND OMR_ASIO_SDK_DIR)
    target_compile_definitions(LiveDeck PRIVATE JUCE_ASIO=1)
    target_include_directories(LiveDeck PRIVATE ${OMR_ASIO_SDK_DIR}/common)
endif()

target_link_libraries(LiveDeck
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_cryptography
        juce::juce_osc
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Tests: record path, BWF/RF64, recovery, patching, overrun handling
# ---------------------------------------------------------------------------
if(OMR_BUILD_TESTS)
    juce_add_console_app(LiveDeckTests PRODUCT_NAME "LiveDeckTests")
    target_sources(LiveDeckTests PRIVATE ${OMR_CORE_SOURCES} Tests/TestMain.cpp)
    target_include_directories(LiveDeckTests PRIVATE Source)
    target_compile_definitions(LiveDeckTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(LiveDeckTests
        PRIVATE juce::juce_audio_formats juce::juce_audio_basics juce::juce_core juce::juce_events juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
