/* ============================================================================
 * sim_random.h - 实例化、确定性可控的伪随机源（纯 C11）。
 *
 * seed == 0 时以系统单调时钟自动播种（等价旧实现 host 端行为）；
 * seed != 0 时同一 seed 产生完全相同序列（确定性测试/故障复现）。
 * 无任何全局状态。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_RANDOM_H
#define DEMO_DEVICE_SIM_RANDOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_random sim_random_t;

sim_random_t *sim_random_create(uint32_t seed);
void sim_random_destroy(sim_random_t *rng);
uint32_t sim_random_next(sim_random_t *rng);

#ifdef __cplusplus
}
#endif

#endif
