# -----------------------------------------------------------------------------
# Third-party dependencies, pinned by tag. Fetched at configure time.
#
# Offline / vendored builds: point FetchContent at local checkouts, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_JOLTPHYSICS=/path/to/JoltPhysics
#   -DFETCHCONTENT_SOURCE_DIR_FASTNOISE2=/path/to/FastNoise2
#   -DFETCHCONTENT_SOURCE_DIR_MESHOPTIMIZER=/path/to/meshoptimizer
#   -DFETCHCONTENT_SOURCE_DIR_OZZ=/path/to/ozz-animation
# -----------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
set(VXE_THIRD_PARTY_LIBS "")

# --- Jolt Physics ------------------------------------------------------------
if(VXE_WITH_JOLT)
  # 512 km^2 world => ~22.6 km on a side. Bodies live in double precision
  # (RVec3) so collision far from the origin stays exact; shapes stay float
  # and are expressed relative to each chunk's origin.
  set(DOUBLE_PRECISION          ON  CACHE BOOL "" FORCE)
  # Our engine subclasses Jolt interfaces (job system, layer filters), and our
  # TUs are built with RTTI, so Jolt must emit typeinfo too.
  set(CPP_RTTI_ENABLED          ON  CACHE BOOL "" FORCE)
  set(ENABLE_ALL_WARNINGS       OFF CACHE BOOL "" FORCE)
  # Jolt defaults to the static MSVC runtime (/MT); everything else here uses
  # the DLL runtime (/MD), and mixing the two is a hard LNK2038 at link time.
  set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  set(OVERRIDE_CXX_FLAGS        OFF CACHE BOOL "" FORCE)
  set(ENABLE_INSTALL            OFF CACHE BOOL "" FORCE)
  set(JPH_USE_DX12              OFF CACHE BOOL "" FORCE)
  set(JPH_USE_VK                OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL               OFF CACHE BOOL "" FORCE)
  set(TARGET_UNIT_TESTS         OFF CACHE BOOL "" FORCE)
  set(TARGET_HELLO_WORLD        OFF CACHE BOOL "" FORCE)
  set(TARGET_PERFORMANCE_TEST   OFF CACHE BOOL "" FORCE)
  set(TARGET_SAMPLES            OFF CACHE BOOL "" FORCE)
  set(TARGET_VIEWER             OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build)
  FetchContent_MakeAvailable(JoltPhysics)
  list(APPEND VXE_THIRD_PARTY_LIBS Jolt)
endif()

# --- FastNoise2 --------------------------------------------------------------
if(VXE_WITH_FASTNOISE2)
  set(FASTNOISE2_TOOLS     OFF CACHE BOOL "" FORCE)
  set(FASTNOISE2_TESTS     OFF CACHE BOOL "" FORCE)
  # Bit-identical density across SSE2/AVX2/AVX512/NEON. Mandatory for us:
  # edit deltas are stored relative to procedurally regenerated base density,
  # so two machines must regenerate exactly the same base field.
  set(FASTNOISE2_STRICT_FP ON  CACHE BOOL "" FORCE)
  FetchContent_Declare(FastNoise2
    GIT_REPOSITORY https://github.com/Auburn/FastNoise2.git
    GIT_TAG        v1.1.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(FastNoise2)
  list(APPEND VXE_THIRD_PARTY_LIBS FastNoise)
endif()

# --- meshoptimizer -----------------------------------------------------------
if(VXE_WITH_MESHOPT)
  FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v1.3
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(meshoptimizer)
  list(APPEND VXE_THIRD_PARTY_LIBS meshoptimizer)
endif()

# --- ozz-animation -----------------------------------------------------------
if(VXE_WITH_OZZ)
  set(ozz_build_tools   OFF CACHE BOOL "" FORCE)
  set(ozz_build_fbx     OFF CACHE BOOL "" FORCE)
  set(ozz_build_gltf    OFF CACHE BOOL "" FORCE)
  set(ozz_build_samples OFF CACHE BOOL "" FORCE)
  set(ozz_build_howtos  OFF CACHE BOOL "" FORCE)
  set(ozz_build_tests   OFF CACHE BOOL "" FORCE)
  set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
  set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)   # /MD, to match Jolt and the engine
  FetchContent_Declare(ozz
    GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
    GIT_TAG        0.16.0   # 0.17 requires CMake 3.30
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(ozz)
  # ozz_animation_offline is only needed for runtime-built rigs (demo/tests);
  # a shipping build loads pre-baked .ozz archives with ozz_animation alone.
  list(APPEND VXE_THIRD_PARTY_LIBS ozz_animation ozz_animation_offline ozz_base)
endif()
