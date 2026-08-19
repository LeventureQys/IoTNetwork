#ifndef DEMO_COMMON_H
#define DEMO_COMMON_H
#include <stdint.h>
#include <stddef.h>

#define DEMO_OK         0
#define DEMO_ERR       -1
#define DEMO_ERR_NOMEM -2
#define DEMO_ERR_INVAL -3
#define DEMO_ERR_AGAIN -4
#define DEMO_ERR_TIMEOUT -5

typedef enum { LOG_TRACE = 0, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

#endif
