#=============================================================================
#  MuseScore
#  Music Composition & Notation
#
#  Copyright (C) 2013 Werner Schweer
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License version 2
#  as published by the Free Software Foundation and appearing in
#  the file LICENSE.GPL
#=============================================================================

set(MODULE audio)

if (VST3_HOST)
      include(FetchContent)

      # VST3 SDK 3.8 is MIT-licensed. Pin the exact SDK revision so this host
      # remains reproducible even when Steinberg's master branch changes.
      set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
      set(SMTG_ENABLE_VST3_MIGRATION OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG 58f8da7936800732561402d7936584ca4505de07
            GIT_SHALLOW TRUE
            GIT_SUBMODULES base cmake pluginterfaces public.sdk
            GIT_SUBMODULES_RECURSE FALSE
            )
      FetchContent_MakeAvailable(vst3sdk)
endif()

include (${CMAKE_CURRENT_LIST_DIR}/midi/midi.cmake)
include (${CMAKE_CURRENT_LIST_DIR}/exports/exports.cmake)

QT5_WRAP_UI (ui_headers
    ${MIDI_UI}
    )

add_library (${MODULE} STATIC
      ${ui_headers}
      ${MIDI_SRC}
      ${EXPORTS_SRC}
      )

target_include_directories(${MODULE} 
   PRIVATE 
      ${PROJECT_SOURCE_DIR}
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_CURRENT_LIST_DIR}
      ${PROJECT_SOURCE_DIR}/global
      ${PROJECT_SOURCE_DIR}/thirdparty  # because include "mscore/musescore.h"
   )

# Do not link mscoreapp here: mscoreapp links audio, and the resulting static
# library cycle makes AUTOMOC unusable with the Xcode generator (CMake >= 4.4).
# audio's sources include mscore/*.h, but those symbols are resolved when the
# app or a test binary links mscoreapp itself.
target_link_libraries (${MODULE}
      ${QT_LIBRARIES}
      audiofile
      libmscore   # for libmscore/*.h and the freetype include dirs it carries
      preferences
      icons       # for the fluid and zerberus settings GUIs
      extension   # to find the soundfonts and sfzs extensions ship
      )

if (VST3_HOST)
      target_sources(${MODULE} PRIVATE
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
            ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.h
            )
      target_include_directories(${MODULE} PRIVATE ${vst3sdk_SOURCE_DIR})

      # sdk_hosting publishes its own C++ standard as a usage requirement.
      # Keep that from overriding the standard used to create MuseScore's
      # shared PCH; only the SDK's link interface is needed by this target.
      target_link_libraries(${MODULE} $<LINK_ONLY:sdk_hosting>)
      if (MSVC)
            set_property(TARGET ${MODULE} PROPERTY CXX_STANDARD 20)
            set_property(TARGET ${MODULE} PROPERTY CXX_STANDARD_REQUIRED ON)
      endif()
endif()

if (BUILD_PCH)
    target_use_pch(${MODULE})
endif (BUILD_PCH)
