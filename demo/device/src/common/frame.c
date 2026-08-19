#include "frame.h"
#include "protocol.h"

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
