#ifndef PC_SHA256_H
#define PC_SHA256_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 标准 SHA-256（FIPS 180-4），用于事件 data 的 sha256 字段。 */
typedef struct pc_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buflen;
} pc_sha256_ctx_t;

void pc_sha256_init(pc_sha256_ctx_t *ctx);
void pc_sha256_update(pc_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void pc_sha256_final(pc_sha256_ctx_t *ctx, uint8_t digest[32]);

/* 便捷：计算字节串摘要并以 64 字符小写十六进制写入 out（容量至少 65）。 */
void pc_sha256_hex(const uint8_t *data, size_t len, char *out);

#ifdef __cplusplus
}
#endif

#endif
