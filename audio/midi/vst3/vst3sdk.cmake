# MuseScore 3 VST3 host support
#
# MuseScore 4.0 used the VST3 SDK 3.7 hosting helpers.  Keep this target
# separate from the old MuseScore audio target so the SDK can use C++17
# without changing the rest of the MuseScore 3 build.

include(FetchContent)

if (NOT VST3_SDK_PATH AND DEFINED ENV{VST3_SDK_PATH})
      set(VST3_SDK_PATH "$ENV{VST3_SDK_PATH}" CACHE PATH "VST3 SDK checkout" FORCE)
endif()

if (VST3_SDK_PATH)
      get_filename_component(VST3_SDK_PATH "${VST3_SDK_PATH}" ABSOLUTE)
else()
      FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG 358b72ee61bc67fb4592b0d492e0c6a1211ebf11
            GIT_SHALLOW TRUE
            GIT_SUBMODULES base pluginterfaces public.sdk
            GIT_SUBMODULES_RECURSE FALSE
            )
      FetchContent_GetProperties(vst3sdk)
      if (NOT vst3sdk_POPULATED)
            FetchContent_Populate(vst3sdk)
      endif()
      set(VST3_SDK_PATH "${vst3sdk_SOURCE_DIR}")
endif()

foreach(required_dir base pluginterfaces public.sdk)
      if (NOT EXISTS "${VST3_SDK_PATH}/${required_dir}")
            message(FATAL_ERROR "VST3 SDK is incomplete: ${VST3_SDK_PATH}/${required_dir} is missing")
      endif()
endforeach()

set(VST3_SDK_SOURCES
      ${VST3_SDK_PATH}/base/source/baseiids.cpp
      ${VST3_SDK_PATH}/base/source/fbuffer.cpp
      ${VST3_SDK_PATH}/base/source/fdebug.cpp
      ${VST3_SDK_PATH}/base/source/fdynlib.cpp
      ${VST3_SDK_PATH}/base/source/fobject.cpp
      ${VST3_SDK_PATH}/base/source/fstreamer.cpp
      ${VST3_SDK_PATH}/base/source/fstring.cpp
      ${VST3_SDK_PATH}/base/source/updatehandler.cpp
      ${VST3_SDK_PATH}/base/thread/source/fcondition.cpp
      ${VST3_SDK_PATH}/base/thread/source/flock.cpp

      ${VST3_SDK_PATH}/pluginterfaces/base/conststringtable.cpp
      ${VST3_SDK_PATH}/pluginterfaces/base/coreiids.cpp
      ${VST3_SDK_PATH}/pluginterfaces/base/funknown.cpp
      ${VST3_SDK_PATH}/pluginterfaces/base/ustring.cpp

      ${VST3_SDK_PATH}/public.sdk/source/common/commoniids.cpp
      ${VST3_SDK_PATH}/public.sdk/source/common/memorystream.cpp
      ${VST3_SDK_PATH}/public.sdk/source/common/openurl.cpp
      ${VST3_SDK_PATH}/public.sdk/source/common/pluginview.cpp
      ${VST3_SDK_PATH}/public.sdk/source/common/systemclipboard_win32.cpp
      ${VST3_SDK_PATH}/public.sdk/source/common/threadchecker_win32.cpp
      ${VST3_SDK_PATH}/public.sdk/source/main/pluginfactory.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/connectionproxy.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/eventlist.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/hostclasses.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/module.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/module_win32.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/parameterchanges.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/plugprovider.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/hosting/processdata.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/utility/stringconvert.cpp
      ${VST3_SDK_PATH}/public.sdk/source/vst/vstinitiids.cpp
      )

add_library(vst3sdk_hosting STATIC ${VST3_SDK_SOURCES})
target_include_directories(vst3sdk_hosting PUBLIC ${VST3_SDK_PATH})
set_property(TARGET vst3sdk_hosting PROPERTY CXX_STANDARD 17)
set_property(TARGET vst3sdk_hosting PROPERTY CXX_STANDARD_REQUIRED ON)
target_compile_definitions(vst3sdk_hosting PRIVATE NOMINMAX)

if (MSVC)
      target_compile_options(vst3sdk_hosting PRIVATE /W0)
      target_link_libraries(vst3sdk_hosting PUBLIC ole32 shell32)
endif()

set_target_properties(vst3sdk_hosting PROPERTIES FOLDER "thirdparty")
