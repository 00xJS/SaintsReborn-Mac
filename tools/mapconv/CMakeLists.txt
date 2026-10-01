cmake_minimum_required(VERSION 3.20)
project(SaintsRebornMapConverter LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # no runtime DLLs needed
endif()
add_library(mapconv_core STATIC
  src/build.cpp src/chunk.cpp src/collision.cpp src/geometry.cpp src/gltf_import.cpp src/peg.cpp src/vpp.cpp
  third_party/puff.c)
target_include_directories(mapconv_core PUBLIC src third_party)
add_executable(SaintsRebornMapConverter src/main.cpp $<$<BOOL:${WIN32}>:editor/res/editor.rc>)
target_link_libraries(SaintsRebornMapConverter PRIVATE mapconv_core)
if(WIN32)
  target_link_libraries(SaintsRebornMapConverter PRIVATE shell32)
endif()
add_executable(mapconv_selftest tests/roundtrip.cpp)
target_link_libraries(mapconv_selftest PRIVATE mapconv_core)

# ---- Saints Reborn Map Editor (Windows only: Direct3D 11 + Dear ImGui)
if(WIN32)
  set(ED editor)
  add_executable(SaintsRebornMapEditor WIN32
    ${ED}/src/main.cpp ${ED}/src/app.cpp ${ED}/src/ui.cpp ${ED}/src/renderer.cpp ${ED}/src/scene.cpp ${ED}/src/textures.cpp ${ED}/src/assetlink.cpp
    ${ED}/third_party/imgui/imgui.cpp ${ED}/third_party/imgui/imgui_draw.cpp ${ED}/third_party/imgui/imgui_tables.cpp
    ${ED}/third_party/imgui/imgui_widgets.cpp
    ${ED}/third_party/imgui/backends/imgui_impl_win32.cpp ${ED}/third_party/imgui/backends/imgui_impl_dx11.cpp
    ${ED}/third_party/imguizmo/ImGuizmo.cpp
    ${ED}/res/editor.rc)
  target_include_directories(SaintsRebornMapEditor PRIVATE ${ED}/src ${ED}/third_party ${ED}/third_party/imgui
    ${ED}/third_party/imgui/backends ${ED}/third_party/imguizmo)
  target_compile_definitions(SaintsRebornMapEditor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
  target_link_libraries(SaintsRebornMapEditor PRIVATE mapconv_core d3d11 dxgi d3dcompiler comdlg32 shell32 dwmapi ole32 uuid)
  add_executable(editor_core_test ${ED}/tests/core_test.cpp ${ED}/src/scene.cpp ${ED}/src/textures.cpp)
  target_include_directories(editor_core_test PRIVATE ${ED}/src ${ED}/third_party)
  target_link_libraries(editor_core_test PRIVATE mapconv_core)
endif()
