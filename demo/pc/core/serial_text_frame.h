#ifndef DEMO_SERIAL_TEXT_FRAME_H
#define DEMO_SERIAL_TEXT_FRAME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* PC→ESP32→MCU 单帧串口文本消息编码规则 v1。
 *
 * 文本输入框内容按 UTF-8 字节编码为单帧，经 wire v2 SERIAL_BYTES 透明下发；
 * ESP32 不解析本帧，仅原样写入 UART1，由 MCU 按同一规则解包。
 *
 * 帧布局（大端长度，最大 payload 512 字节）：
 *   [0]      0xAA  帧头1
 *   [1]      0x55  帧头2
 *   [2]      0x01  版本
 *   [3..4]   payload 字节数（大端，1..512）
 *   [5..N]   UTF-8 文本 payload
 *   [5+N]    XOR 校验（对前面所有字节逐字节异或）
 */
namespace serial_text_frame {

constexpr uint8_t kSof0 = 0xAA;
constexpr uint8_t kSof1 = 0x55;
constexpr uint8_t kVersion = 0x01;
constexpr size_t kMaxPayloadBytes = 512;

/* 文本编码为单帧。text 为空或超过 512 字节时返回 false。 */
bool Encode(const std::string &text, std::vector<uint8_t> *out);

/* 校验并解包单帧，恢复原始文本。格式/长度/校验任一不合法返回 false。 */
bool Decode(const uint8_t *bytes, size_t length, std::string *text);

/* 字节流转成 "AA 55 01 ..." 形式，用于日志打印。 */
std::string Hex(const uint8_t *bytes, size_t length);

} // namespace serial_text_frame

#endif
