#ifndef DEMO_FRAME_H
#define DEMO_FRAME_H
#include <stdint.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

int frame_wrap(const uint8_t *payload, int len, uint8_t *out, int out_cap);
int frame_parse(const uint8_t *data, int data_len, int *out_off, int *out_len, int *consumed);

/* wire v2：编码一帧。返回总字节数（HEAD+BODY+payload），失败返回负值。 */
int frame_v2_wrap(uint8_t frame_type, uint64_t sequence,
                  const uint8_t *payload, int payload_len,
                  uint8_t *out, int out_cap);

/* wire v2：增量解析。返回：
 *   1     完整一帧（*out_payload 指向 data 内 payload，仅调用期有效）
 *   0     需要更多数据
 *   <0    协议错误（非法长度/type/flags/reserved/payload 长度），调用方应断连
 * *consumed 成功=整帧字节数；错误=建议丢弃字节数（协议错误无需重同步）。 */
int frame_v2_parse(const uint8_t *data, int data_len,
                   int *out_type, uint64_t *out_sequence,
                   const uint8_t **out_payload, int *out_payload_len,
                   int *consumed);

#ifdef __cplusplus
}
#endif

#endif
