#!/usr/bin/env bash
# 设备端启动脚本（自包含：只引用 demo/device 内部）。
# Linux 默认真实热点后端；传 --backend sim 使用跨进程模拟热点。
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_ROOT="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${DEVICE_ROOT}/build"
PROGRAM="${BUILD_DIR}/bin/provision_device"
RUNTIME_DIR="${BUILD_DIR}/run"
SIM_CATALOG_DIR="${BUILD_DIR}/sim-catalog"
DEVICE_INDEX="${DEVICE_INDEX:-0}"
QT_PLATFORM="${DEVICE_QPA_PLATFORM:-offscreen}"
BACKEND="${DEVICE_BACKEND:-linux}"

args=("$@")
for ((index = 0; index < ${#args[@]}; index++)); do
    if [[ "${args[index]}" == "--backend" && $((index + 1)) -lt ${#args[@]} ]]; then
        BACKEND="${args[index + 1]}"
    fi
done

if [[ "${BACKEND}" == "sim" ]]; then
    CONFIG="${DEVICE_ROOT}/config/device_sim.json"
else
    CONFIG="${DEVICE_ROOT}/config/device_linux.json"
fi

cmake -S "${DEVICE_ROOT}" -B "${BUILD_DIR}" -DDEVICE_BUILD_TESTS=OFF
cmake --build "${BUILD_DIR}" -j"$(nproc)"

command=(
    env QT_QPA_PLATFORM="${QT_PLATFORM}"
    "${PROGRAM}"
    --config "${CONFIG}"
    --backend "${BACKEND}"
    --device-index "${DEVICE_INDEX}"
    --fresh
    --runtime-dir "${RUNTIME_DIR}"
)

if [[ "${BACKEND}" == "sim" ]]; then
    command+=(--sim-catalog-dir "${SIM_CATALOG_DIR}")
fi
command+=("$@")

if [[ "${BACKEND}" == "linux" && "$(id -u)" -ne 0 ]]; then
    exec sudo "${command[@]}"
fi
exec "${command[@]}"
