#include "frame.h"
#include "protocol.h"
#include <string.h>

int frame_wrap(const uint8_t *payload, int len, uint8_t *out, int out_cap)
{
    if (len < 0 || len > PROTO_MSG_MAX_LEN)
        return DEMO_ERR_INVAL;
    if (out_cap < len + PROTO_FRAME_HEAD_LEN)
        return DEMO_ERR_NOMEM;
    out[0] = (uint8_t)((len >> 8) & 0xFF);
    out[1] = (uint8_t)(len & 0xFF);
    if (len > 0 && payload != NULL)
        for (int i = 0; i < len; i++)
            out[PROTO_FRAME_HEAD_LEN + i] = payload[i];
    return len + PROTO_FRAME_HEAD_LEN;
}

int frame_parse(const uint8_t *data, int data_len, int *out_off, int *out_len, int *consumed)
{
    if (out_off == NULL || out_len == NULL || consumed == NULL)
        return DEMO_ERR_INVAL;
    *consumed = 0;
    if (data_len <= 0)
        return 0;
    if (data == NULL)
        return DEMO_ERR_INVAL;

    if (data_len < PROTO_FRAME_HEAD_LEN)
        return 0; /* 长度前缀未收全 */

    int len = ((int)data[0] << 8) | (int)data[1];
    if (len == 0 || len > PROTO_MSG_MAX_LEN) {
        *consumed = PROTO_FRAME_HEAD_LEN; /* 畸形：消费前缀（丢弃），等待后续重同步 */
        return DEMO_ERR;
    }
    if (data_len < PROTO_FRAME_HEAD_LEN + len)
        return 0; /* 负载未收全 */

    *out_off = PROTO_FRAME_HEAD_LEN;
    *out_len = len;
    *consumed = PROTO_FRAME_HEAD_LEN + len;
    return 1;
}

/* ---------------- wire v2 ---------------- */

static void v2_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void v2_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void v2_put_be64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v);
}

static uint32_t v2_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t v2_get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

int frame_v2_wrap(uint8_t frame_type, uint64_t sequence,
                  const uint8_t *payload, int payload_len,
                  uint8_t *out, int out_cap)
{
    if (out == NULL)
        return DEMO_ERR_INVAL;
    if (payload_len < 0 || (payload_len > 0 && payload == NULL))
        return DEMO_ERR_INVAL;

    int max_payload;
    if (frame_type == PROTO_V2_TYPE_CONTROL_JSON)
        max_payload = PROTO_V2_CONTROL_MAX_LEN;
    else if (frame_type == PROTO_V2_TYPE_SERIAL_BYTES)
        max_payload = PROTO_V2_SERIAL_CHUNK_MAX;
    else
        return DEMO_ERR_INVAL;

    if (payload_len > max_payload)
        return DEMO_ERR_INVAL;
    if (frame_type == PROTO_V2_TYPE_SERIAL_BYTES && payload_len < 1)
        return DEMO_ERR_INVAL;

    int total = PROTO_V2_BODY_HEAD_LEN + payload_len;
    if (out_cap < PROTO_V2_HEAD_LEN + total)
        return DEMO_ERR_NOMEM;

    v2_put_be32(out, (uint32_t)total);
    out[4] = frame_type;
    out[5] = 0; /* flags */
    v2_put_be16(out + 6, 0); /* reserved */
    v2_put_be64(out + 8, sequence);
    if (payload_len > 0)
        memcpy(out + PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN, payload, (size_t)payload_len);
    return PROTO_V2_HEAD_LEN + total;
}

int frame_v2_parse(const uint8_t *data, int data_len,
                   int *out_type, uint64_t *out_sequence,
                   const uint8_t **out_payload, int *out_payload_len,
                   int *consumed)
{
    if (out_type == NULL || out_sequence == NULL || out_payload == NULL ||
        out_payload_len == NULL || consumed == NULL)
        return DEMO_ERR_INVAL;
    *consumed = 0;
    if (data == NULL || data_len <= 0)
        return 0;

    if (data_len < PROTO_V2_HEAD_LEN)
        return 0; /* 长度前缀未收全 */

    uint32_t total = v2_get_be32(data);
    if (total < PROTO_V2_BODY_HEAD_LEN || total > PROTO_V2_FRAME_MAX_LEN) {
        *consumed = PROTO_V2_HEAD_LEN;
        return DEMO_ERR;
    }

    int frame_bytes = (int)(PROTO_V2_HEAD_LEN + total);
    if (data_len < frame_bytes)
        return 0; /* 负载未收全 */

    uint8_t frame_type = data[4];
    uint8_t flags = data[5];
    uint16_t reserved = (uint16_t)((data[6] << 8) | data[7]);
    uint64_t sequence = v2_get_be64(data + 8);
    int payload_len = (int)total - PROTO_V2_BODY_HEAD_LEN;

    if ((frame_type != PROTO_V2_TYPE_CONTROL_JSON &&
         frame_type != PROTO_V2_TYPE_SERIAL_BYTES) ||
        flags != 0 || reserved != 0) {
        *consumed = frame_bytes;
        return DEMO_ERR;
    }
    if (frame_type == PROTO_V2_TYPE_SERIAL_BYTES &&
        (payload_len < 1 || payload_len > PROTO_V2_SERIAL_CHUNK_MAX)) {
        *consumed = frame_bytes;
        return DEMO_ERR;
    }
    if (frame_type == PROTO_V2_TYPE_CONTROL_JSON &&
        payload_len > PROTO_V2_CONTROL_MAX_LEN) {
        *consumed = frame_bytes;
        return DEMO_ERR;
    }

    *out_type = frame_type;
    *out_sequence = sequence;
    *out_payload = data + PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN;
    *out_payload_len = payload_len;
    *consumed = frame_bytes;
    return 1;
}
