#ifndef DEMO_FRAME_H
#define DEMO_FRAME_H
#include <stdint.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

int frame_wrap(const uint8_t *payload, int len, uint8_t *out, int out_cap);
int frame_parse(const uint8_t *data, int data_len, int *out_off, int *out_len, int *consumed);

#ifdef __cplusplus
}
#endif

#endif
