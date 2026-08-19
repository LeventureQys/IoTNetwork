#ifndef PC_SCENARIO_RUNNER_H
#define PC_SCENARIO_RUNNER_H

#include <atomic>
#include <cstdint>

#include "pc_scenario.h"
#include "net_abstraction.h"

class HostApp;

/* scenario 执行器：主线程 Run() 阻塞直到全部 action 处理完毕或 stop。
 * 每个 action 只执行一次，且必须恰好产生一条同 action_id 的 scenario_result；
 * 目标不匹配当前进程（非 "pc"）的 action 被忽略（不产生 scenario_result）。
 * 时序契约（设计文档第 11 节）：delay_ms 相对 after_event 到达时刻起算，
 * 到达时刻与等待均使用进程内统一单调毫秒时钟（pc_events_*，毫秒级精度），
 * 与设备侧 scenario 引擎语义一致。 */
class PcScenarioRunner {
public:
    PcScenarioRunner(HostApp *host, net_ctx_t *net, const pc_scenario_t *scenario);
    ~PcScenarioRunner();

    /* 阻塞执行全部目标为 pc 的 action；stop 后尽快退出。 */
    void Run();
    void RequestStop();

private:
    void RunAction(const pc_scenario_action_t &action);
    void EmitResult(const pc_scenario_action_t &action, const char *status,
                    int request_bytes, const char *reason);
    bool WaitForEvent(const char *event, int timeout_ms);
    void WaitUntilMs(uint64_t trigger_at_ms);

    HostApp *host_;
    net_ctx_t *net_;
    pc_scenario_t scenario_;
    std::atomic<bool> stop_{false};
};

#endif
