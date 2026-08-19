#ifndef PC_SCENARIO_H
#define PC_SCENARIO_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* scenario（设计文档第 11 节）：--scenario 只允许 sim 后端；
 * schema 固定，id 唯一，delay_ms 0~60000；目标不匹配当前进程的 action 被忽略；
 * 目标匹配但 action/args/schema 非法时启动失败。
 * 载荷上限：APP_DATA_TEXT_MAX（512 字节），超过则拒绝（status=rejected）。 */

typedef enum pc_scenario_action_kind {
    PC_SCENARIO_ACTION_AUTO_PROVISION = 0,
    PC_SCENARIO_ACTION_SEND_APP_DATA,
    PC_SCENARIO_ACTION_INJECT_FAULT,
    PC_SCENARIO_ACTION_REQUEST_STOP
} pc_scenario_action_kind_t;

typedef struct pc_scenario_action {
    char id[64];
    char target[16];       /* "pc" | "device" */
    char after_event[32];  /* ready|ap_ready|session_online|session_offline|null */
    int delay_ms;          /* 0~60000 */
    pc_scenario_action_kind_t kind;
    char arg_ssid[33];
    char arg_password[64];
    char arg_text[520];    /* 原始长度可能超过 512：arg_text_len 记录真实字节数 */
    int arg_text_len;
    char arg_fault_name[64];
    char arg_fault_json[256];
} pc_scenario_action_t;

typedef struct pc_scenario {
    int schema;
    int action_count;
    pc_scenario_action_t actions[16];
} pc_scenario_t;

/* 解析并校验文件：schema/action/args 非法返回 DEMO_ERR（含 id 重复、delay 越界、
 * 未知 action、args 缺字段/类型错误）。文件不存在或 JSON 损坏亦返回 DEMO_ERR。 */
int pc_scenario_load(const char *path, pc_scenario_t *out);

/* 后端门控：--scenario 仅允许 sim（backend_sim=1）。启动前调用，非法则进程必须失败。 */
int pc_scenario_validate_backend(int backend_sim);

#ifdef __cplusplus
}
#endif

#endif
