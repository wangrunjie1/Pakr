cmake_minimum_required(VERSION 3.20)
project(PakrWindows LANGUAGES CXX RC)

set(webview2_native "${PAKR_WEBVIEW2_SDK}/build/native")
add_executable(Pakr.Windows WIN32 Program.cpp "${PAKR_RESOURCES}")
set_property(TARGET Pakr.Windows PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(Pakr.Windows PRIVATE cxx_std_17)
target_compile_options(Pakr.Windows PRIVATE /utf-8)
target_compile_definitions(Pakr.Windows PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(Pakr.Windows PRIVATE "${webview2_native}/include")
target_link_libraries(Pakr.Windows PRIVATE
    "${webview2_native}/x64/WebView2LoaderStatic.lib"
    advapi32 ole32 oleaut32 shell32 shlwapi version winhttp runtimeobject uuid)
