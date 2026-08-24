#!/usr/bin/env python3
"""解析 SerialDataModel 落盘的串口 .bin 文件。

该文件是上位机把设备 SERIAL_BYTES 的 payload 原样拼接后写出的字节流。
当前设备契约：frame_size = 4 + rows * cols * 2 = 3172 字节，帧头为 A5 5A。
脚本会：
  1. 自动跳过/保存开头可能存在的 Modbus ACK 等前缀字节
  2. 按帧大小切出每一包完整帧
  3. 把每帧拆成 header(4B) + rows*cols 个小端 uint16 数据点
  4. 输出：
     - summary.txt         总览
     - frames.jsonl        每行一包，含全部数据点（机器可读）
     - frames_index.csv    每包偏移/文件/统计
     - frames/frame_*.bin  每一包原始帧（单独文件）
     - prefix.bin/tail.bin 开头前缀与尾部残包（如有）
"""

import argparse
import csv
import json
import struct
import sys
from pathlib import Path

SYNC = b"\xA5\x5A"
HEADER_SIZE = 4
DEFAULT_FRAME_SIZE = 3172
DEFAULT_ROWS = 36
DEFAULT_COLS = 44


def hex_text(data: bytes, limit: int = 64) -> str:
    text = data.hex(" ").upper()
    if len(data) > limit:
        text = text[: limit * 3] + f" ... ({len(data)} bytes)"
    return text


def find_sync(data: bytes) -> int:
    pos = data.find(SYNC)
    if pos < 0:
        raise SystemExit("文件中未找到 A5 5A 帧头")
    return pos


def detect_frame_size(data: bytes, first: int) -> int:
    second = data.find(SYNC, first + len(SYNC))
    if second < 0:
        raise SystemExit("文件中只有一个 A5 5A，无法自动推断帧大小，请用 --frame-size 指定")
    stride = second - first
    if stride < HEADER_SIZE + 2 or stride > 65536:
        raise SystemExit(f"自动推断的帧大小不合理：{stride}")
    complete = (len(data) - first) // stride
    for i in range(complete):
        off = first + i * stride
        if data[off:off + len(SYNC)] != SYNC:
            raise SystemExit(
                f"帧 {i + 1} 的偏移 {off} 处不是 A5 5A，字节流可能不是固定帧长，"
                "请检查数据或改用 --frame-size")
    return stride


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_bin", help="SerialDataModel 落盘的 .bin 文件")
    parser.add_argument("-o", "--output-dir", help="输出目录；默认在输入文件旁创建 <文件名>_parsed")
    parser.add_argument("--frame-size", type=int, default=0,
                        help="帧大小；默认自动根据相邻 A5 5A 帧头推断")
    parser.add_argument("--rows", type=int, default=DEFAULT_ROWS)
    parser.add_argument("--cols", type=int, default=DEFAULT_COLS)
    parser.add_argument("--no-frame-files", action="store_true",
                        help="不生成 frames/frame_*.bin 单独文件")
    args = parser.parse_args()

    src = Path(args.input_bin).resolve()
    if not src.is_file():
        print(f"找不到输入文件：{src}", file=sys.stderr)
        return 2

    out_dir = Path(args.output_dir).resolve() if args.output_dir else src.with_name(src.stem + "_parsed")
    out_dir.mkdir(parents=True, exist_ok=True)
    frames_dir = out_dir / "frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    data = src.read_bytes()
    first = find_sync(data)
    frame_size = args.frame_size or detect_frame_size(data, first)
    point_count = (frame_size - HEADER_SIZE) // 2
    if point_count <= 0:
        raise SystemExit(f"帧大小 {frame_size} 小于 header({HEADER_SIZE} 字节) + 1 个 uint16")
    if (frame_size - HEADER_SIZE) % 2 != 0:
        print(f"警告：frame_size - 4 = {frame_size - HEADER_SIZE} 不是偶数，数据点解析可能不对",
              file=sys.stderr)

    total_frames, tail_len = divmod(len(data) - first, frame_size)
    if total_frames == 0:
        raise SystemExit("没有完整的一包数据")

    prefix = data[:first]
    tail = data[first + total_frames * frame_size:] if tail_len else b""
    if prefix:
        (out_dir / "prefix.bin").write_bytes(prefix)
    if tail:
        (out_dir / "tail.bin").write_bytes(tail)

    rows = args.rows
    cols = args.cols
    if rows * cols != point_count:
        print(f"警告：--rows*--cols={rows * cols}，但帧内数据点数量={point_count}；"
              "CSV 行列按实际点数继续输出", file=sys.stderr)

    jsonl_path = out_dir / "frames.jsonl"
    csv_path = out_dir / "frames_index.csv"
    header_hex = ""

    with jsonl_path.open("w", encoding="utf-8", newline="\n") as jf, \
         csv_path.open("w", encoding="utf-8", newline="") as cf:
        writer = csv.writer(cf)
        writer.writerow(["frame", "file", "offset", "header_hex",
                         "points", "min", "max", "mean"])
        for index in range(total_frames):
            off = first + index * frame_size
            frame = data[off:off + frame_size]
            if index == 0:
                header_hex = frame[:HEADER_SIZE].hex().lower()
            points = struct.unpack(f"<{point_count}H", frame[HEADER_SIZE:])
            file_name = f"frame_{index + 1:06d}.bin"
            if not args.no_frame_files:
                (frames_dir / file_name).write_bytes(frame)

            values = list(points)
            min_v = min(values)
            max_v = max(values)
            mean_v = sum(values) / float(len(values))
            writer.writerow([index + 1, file_name, off, header_hex,
                             len(values), min_v, max_v, f"{mean_v:.3f}"])

            record = {
                "frame": index + 1,
                "offset": off,
                "header_hex": header_hex,
                "points": values,
            }
            jf.write(json.dumps(record, separators=(",", ":")) + "\n")

            if (index + 1) % 500 == 0:
                print(f"已解析 {index + 1}/{total_frames} 包")

    summary_lines = [
        f"输入文件：{src}",
        f"文件大小：{len(data)} 字节",
        f"帧大小：{frame_size} 字节（header {HEADER_SIZE} + {point_count} 个 uint16 数据点）",
        f"完整帧数：{total_frames}",
        f"每包数据点：{point_count}",
        f"完整数据范围：偏移 {first} .. {first + total_frames * frame_size - 1}",
        f"开头前缀：{len(prefix)} 字节 {hex_text(prefix) if prefix else '(无)'}",
        f"尾部残留：{len(tail)} 字节 {hex_text(tail) if tail else '(无)'}",
        f"帧头：{header_hex}",
        "",
        f"输出目录：{out_dir}",
        f"  frames.jsonl     每行一包（JSON，含 points 数组）",
        f"  frames_index.csv 每包元数据与统计",
        f"  frames/          每包原始帧 frame_000001.bin ..",
    ]
    if prefix:
        summary_lines.append("  prefix.bin       开头前缀（本例为 Modbus ACK）")
    if tail:
        summary_lines.append("  tail.bin         尾部残留")
    summary_text = "\n".join(summary_lines)
    (out_dir / "summary.txt").write_text(summary_text + "\n", encoding="utf-8")
    print(summary_text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
