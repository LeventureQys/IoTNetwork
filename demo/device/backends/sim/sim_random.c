/* ============================================================================
 * sim_random.c - xorshift32 伪随机源（实例化，无全局状态）。
 * ========================================================================== */
#include "sim_random.h"
#include "sim_util.h"

#include <stdlib.h>

struct sim_random {
    uint32_t state;
};

sim_random_t *sim_random_create(uint32_t seed)
{
    sim_random_t *rng = (sim_random_t *)calloc(1, sizeof(sim_random_t));
    if (!rng)
        return NULL;
    if (seed == 0)
        seed = (uint32_t)(sim_util_monotonic_ms() ^ ((uintptr_t)rng >> 4));
    if (seed == 0)
        seed = 0x9E3779B9u; /* xorshift 状态为 0 会卡死，必须非零 */
    rng->state = seed;
    return rng;
}

void sim_random_destroy(sim_random_t *rng)
{
    free(rng);
}

uint32_t sim_random_next(sim_random_t *rng)
{
    if (!rng)
        return 0;
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}
