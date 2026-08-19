cmake_minimum_required(VERSION 3.20)
project(modutech_integration_tests NONE)
enable_testing()

# 可选 CTest 注册：integration_tests 本身不构建任何东西，
# 只把场景注册为 CTest 用例并调用 runners/run_scenario.ps1。
# 前置条件：两端已构建（install 到 artifact 目录），
# 通过以下变量传入产物路径（均为绝对路径）。
#
#   cmake -S demo/integration_tests -B out/integration-build \
#       -DINTEGRATION_PC_EXE=<abs>/provision_pc.exe \
#       -DINTEGRATION_DEVICE_EXE=<abs>/provision_device.exe \
#       -DINTEGRATION_PC_CONTRACT=<abs>/protocol-contract.json \
#       -DINTEGRATION_DEVICE_CONTRACT=<abs>/protocol-contract.json \
#       -DINTEGRATION_RUNTIME_ROOT=<abs>/ss07-evidence-ctest
#   ctest --test-dir out/integration-build --output-on-failure

if(NOT DEFINED INTEGRATION_PC_EXE OR NOT DEFINED INTEGRATION_DEVICE_EXE)
    message(FATAL_ERROR
        "INTEGRATION_PC_EXE / INTEGRATION_DEVICE_EXE 必须显式传入绝对路径")
endif()
if(NOT EXISTS "${INTEGRATION_PC_EXE}" OR NOT EXISTS "${INTEGRATION_DEVICE_EXE}")
    message(FATAL_ERROR "PC 或设备可执行文件不存在")
endif()

set(INTEGRATION_PC_CONTRACT "" CACHE PATH "PC share/protocol-contract.json（绝对路径）")
set(INTEGRATION_DEVICE_CONTRACT "" CACHE PATH "设备 share/protocol-contract.json（绝对路径）")
set(INTEGRATION_RUNTIME_ROOT "${CMAKE_CURRENT_BINARY_DIR}/runs" CACHE PATH "场景证据根目录")

if(NOT DEFINED INTEGRATION_PWSH)
    find_program(INTEGRATION_PWSH powershell)
endif()
if(NOT INTEGRATION_PWSH)
    message(FATAL_ERROR "未找到 powershell，无法注册 CTest")
endif()

set(_runner "${CMAKE_CURRENT_SOURCE_DIR}/runners/run_scenario.ps1")
set(_args
    -Scenario "<SCN>"
    -PcExe "${INTEGRATION_PC_EXE}"
    -DeviceExe "${INTEGRATION_DEVICE_EXE}")
if(INTEGRATION_PC_CONTRACT)
    list(APPEND _args -PcContract "${INTEGRATION_PC_CONTRACT}")
endif()
if(INTEGRATION_DEVICE_CONTRACT)
    list(APPEND _args -DeviceContract "${INTEGRATION_DEVICE_CONTRACT}")
endif()
list(APPEND _args -RuntimeRoot "${INTEGRATION_RUNTIME_ROOT}" -TimeoutSeconds 90)

foreach(_scn a b c d e f g h)
    set(_runargs "${_args}")
    string(REPLACE "<SCN>" "${_scn}" _runargs "${_runargs}")
    add_test(
        NAME "integration_${_scn}"
        COMMAND "${INTEGRATION_PWSH}" -NoProfile -ExecutionPolicy Bypass -File "${_runner}" ${_runargs}
    )
    set_tests_properties("integration_${_scn}" PROPERTIES TIMEOUT 150)
endforeach()
