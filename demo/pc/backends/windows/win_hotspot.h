#ifndef PC_WIN_HOTSPOT_H
#define PC_WIN_HOTSPOT_H

#include <string>
#include "net_abstraction.h"

/* WinHotspot 系统交互注入点：用于单元测试覆盖成功/能力缺失/权限/超时，
 * 不向公共 C 头暴露 WinRT 类型。所有 op 返回 DEMO_* 错误码，失败时 error 输出
 * 不含密码的中文描述。ctx 由 create/destroy 管理（真实实现）或由测试外部提供。 */
struct WinHotspotOps {
    int (*create)(void **ctx, std::string *error);
    void (*destroy)(void *ctx);
    int (*start)(void *ctx, const char *ssid, const char *password, std::string *error);
    int (*query)(void *ctx, net_ap_status_t *status, std::string *error);
    int (*configure_ipv4)(void *ctx, const char *ipv4, int prefix_length, std::string *error);
    int (*stop)(void *ctx, std::string *error);
};

/* 真实 Windows 10/11 移动热点（WinRT NetworkOperatorTetheringManager）实现。 */
const WinHotspotOps *win_hotspot_real_ops();

class WinHotspot {
public:
    WinHotspot();                                     /* 使用真实 WinRT ops，内部持有 ctx */
    WinHotspot(const WinHotspotOps *ops, void *ctx);  /* 注入 ops；ctx 不归本类所有 */
    ~WinHotspot();
    WinHotspot(const WinHotspot &) = delete;
    WinHotspot &operator=(const WinHotspot &) = delete;

    int Start(const char *ssid, const char *password, std::string *error);
    int Query(net_ap_status_t *status, std::string *error);
    int ConfigureIpv4(const char *ipv4, int prefix_length, std::string *error);
    int Stop(std::string *error);

private:
    const WinHotspotOps *ops_ = nullptr;
    void *ctx_ = nullptr;
    bool owns_ctx_ = false;
};

#endif
