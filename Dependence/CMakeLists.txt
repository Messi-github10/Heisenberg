add_subdirectory(glm)
add_subdirectory(SpdLog)
add_subdirectory(FFmpeg)
add_subdirectory(LibPlacebo)
add_subdirectory(Lcms2)
add_subdirectory(Vulkan)
add_subdirectory(Volk)
add_subdirectory(miniaudio)
add_subdirectory(nlohmann_json)

include(FetchContent)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)
