# PC 端自包含工程编译选项（目标级，第三方库不继承 /WX）。

function(pc_require_toolchain)
    if(WIN32)
        if(NOT MSVC OR MSVC_VERSION VERSION_LESS 1930)
            message(FATAL_ERROR "PC 工程要求 Visual Studio 2022（MSVC >= 19.30）")
        endif()
    endif()
endfunction()

# 为目标启用严格警告。PC 自有目标使用；third_party 目标不调用本函数。
function(pc_enable_warnings target)
    if(PC_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pthread)
        endif()
    else()
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -pthread)
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX)
    endif()
endfunction()

# 输出目录统一到 bin/ lib/
function(pc_set_output_dirs target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endfunction()
