cmake_minimum_required(VERSION 3.24)
project(VXE VERSION 0.1.0 LANGUAGES C CXX)

# -----------------------------------------------------------------------------
# VXE - Volumetric Voxel & Tessellation Engine (core scaffolding)
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VXE_WITH_JOLT        "Integrate Jolt Physics (terrain collision baking)" ON)
option(VXE_WITH_FASTNOISE2  "Integrate FastNoise2 (SIMD density evaluation)"   ON)
option(VXE_WITH_MESHOPT     "Integrate meshoptimizer (cache/fetch opt, meshlets)" ON)
option(VXE_WITH_OZZ         "Integrate ozz-animation (character runtime)"      ON)
option(VXE_BUILD_DEMO       "Build the headless pipeline demo"                 ON)
option(VXE_BUILD_VIEWER     "Build the windowed D3D11 viewer (Windows only)"   ON)
option(VXE_BUILD_TESTS      "Build unit tests"                                 ON)

include(cmake/Dependencies.cmake)

# -----------------------------------------------------------------------------
# Engine library
# -----------------------------------------------------------------------------
add_library(vxe_engine STATIC
  src/core/TaskSystem.cpp
  src/world/CaveNetwork.cpp
  src/world/DensityGenerator.cpp
  src/world/ChunkStreamer.cpp
  src/meshing/DualContouring.cpp
  src/meshing/MeshOptimizerStage.cpp
  src/terraform/DeltaStore.cpp
  src/terraform/Terraformer.cpp
  src/render/MaterialSystem.cpp
  src/render/TerrainRenderer.cpp
  src/building/StructureGraph.cpp
  src/building/BuildingSystem.cpp
)

if(VXE_WITH_JOLT)
  target_sources(vxe_engine PRIVATE
    src/physics/JoltJobSystemAdapter.cpp
    src/physics/PhysicsWorld.cpp
    src/physics/TerrainCollisionBaker.cpp)
endif()
if(VXE_WITH_OZZ)
  target_sources(vxe_engine PRIVATE src/animation/CharacterAnimator.cpp)
  if(NOT MSVC)  # ozz headers put __m128 in template args (harmless GCC warning)
    set_source_files_properties(src/animation/CharacterAnimator.cpp PROPERTIES COMPILE_OPTIONS -Wno-ignored-attributes)
  endif()
endif()

target_include_directories(vxe_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(vxe_engine PUBLIC
  $<$<BOOL:${VXE_WITH_JOLT}>:VXE_WITH_JOLT=1>
  $<$<BOOL:${VXE_WITH_FASTNOISE2}>:VXE_WITH_FASTNOISE2=1>
  $<$<BOOL:${VXE_WITH_MESHOPT}>:VXE_WITH_MESHOPT=1>
  $<$<BOOL:${VXE_WITH_OZZ}>:VXE_WITH_OZZ=1>)

find_package(Threads REQUIRED)
target_link_libraries(vxe_engine PUBLIC Threads::Threads ${VXE_THIRD_PARTY_LIBS})

if(MSVC)
  target_compile_options(vxe_engine PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
  target_compile_options(vxe_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# -----------------------------------------------------------------------------
# Demo + tests
# -----------------------------------------------------------------------------
if(VXE_BUILD_DEMO)
  add_executable(vxe_demo apps/vxe_demo/main.cpp)
  target_link_libraries(vxe_demo PRIVATE vxe_engine)
endif()

# Windowed demo: Win32 + a small Direct3D 11 backend for the Classic terrain
# path (see apps/vxe_viewer/main.cpp). Windows-only by construction.
if(VXE_BUILD_VIEWER)
  if(WIN32)
    add_executable(vxe_viewer apps/vxe_viewer/main.cpp)
    target_link_libraries(vxe_viewer PRIVATE vxe_engine d3d11 dxgi d3dcompiler)
    if(MSVC)
      target_compile_options(vxe_viewer PRIVATE /W4 /permissive- /Zc:__cplusplus)
    endif()
  else()
    message(STATUS "VXE_BUILD_VIEWER: skipped (needs Win32 + Direct3D 11)")
  endif()
endif()

if(VXE_BUILD_TESTS)
  enable_testing()
  add_executable(vxe_tests tests/vxe_tests.cpp)
  target_link_libraries(vxe_tests PRIVATE vxe_engine)
  add_test(NAME vxe_tests COMMAND vxe_tests)
endif()

# Shaders are compiled by the asset pipeline (see shaders/README.md); they are
# installed alongside the binaries so a backend can load them.
install(DIRECTORY shaders/ DESTINATION share/vxe/shaders)
