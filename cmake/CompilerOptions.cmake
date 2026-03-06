if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _UNICODE UNICODE)
    
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " /Od /Zi")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2 /Zi")
else()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        string(APPEND CMAKE_CXX_FLAGS_DEBUG " -g -O0")
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3")
    endif()
endif()
