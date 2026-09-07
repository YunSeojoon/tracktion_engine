cmake_minimum_required(VERSION 3.22)
project(CoCompose VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory(../../modules/juce juce)
add_subdirectory(../../modules tracktion)
juce_add_gui_app(CoCompose PRODUCT_NAME "CoCompose" BUNDLE_ID "local.cocompose.editor"
    NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
juce_generate_juce_header(CoCompose)
target_sources(CoCompose PRIVATE Main.cpp LiveProject.h)
target_compile_features(CoCompose PRIVATE cxx_std_20)
target_compile_definitions(CoCompose PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
    TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1)
target_link_libraries(CoCompose PRIVATE
    tracktion::tracktion_core tracktion::tracktion_engine tracktion::tracktion_graph
    juce::juce_audio_utils juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(CoCompose PRIVATE /W4 /WX /utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
endif()
