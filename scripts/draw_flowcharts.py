import os
from graphviz import Digraph

figures_dir = os.path.join(os.path.dirname(__file__), '..', 'figures')

NODE_ATTR = {'shape': 'box', 'style': 'rounded,filled',
             'fontname': 'Microsoft YaHei', 'fontsize': '8'}
GRAPH_ATTR = {'nodesep': '0.25', 'ranksep': '0.3', 'splines': 'polyline',
              'fontname': 'Microsoft YaHei', 'fontsize': '16',
              'fontcolor': '#1A237E', 'dpi': '150'}
EDGE_LABEL = {'fontname': 'Microsoft YaHei', 'fontsize': '9', 'fontcolor': '#E65100'}


def new_graph(name, label):
    dot = Digraph(name=name, format='png', node_attr=dict(NODE_ATTR),
                  graph_attr=dict(GRAPH_ATTR))
    dot.attr(rankdir='TB', label=label, labelloc='t')
    return dot


def cluster(dot, cname, label):
    return dot.subgraph(name=cname)


def cattr(c, label):
    c.attr(label=label, style='filled', fillcolor='#F5F5F5',
           fontname='Microsoft YaHei', fontsize='12', fontcolor='#1565C0',
           labeljust='l')


def edge(dot, a, b, xlabel=None, style=None):
    kwargs = {'xlabel': xlabel} if xlabel else {}
    if style:
        kwargs['style'] = style
    dot.edge(a, b, **EDGE_LABEL, **kwargs)


# ============================================================
# Flowchart 1A: 上位机 - 配网流程（阶段二）
# ============================================================
dot = new_graph('HostProvisionFlow', '上位机（PC）配网流程图')
with cluster(dot, 'cluster_p1', '阶段二：配网（SoftAP 会话模式）') as c:
    cattr(c, '阶段二：配网（SoftAP 会话模式）')
    c.node('h1', '扫描 WiFi 热点\n(标注频段)', fillcolor='#4CAF50', fontcolor='white')
    c.node('h2', '发现 Modu_XXXX\n热点?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('h3', '用户选择目标设备', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h4', '连接 Modu_XXXX 热点\n(暂时断开当前网络)', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('h5', 'TCP 连接 192.168.1.1:5935\n发送 auth (WPA2 密码+PIN)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h6', '认证通过?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('h7', '发送 wifi_config', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h8', 'wifi_result\nstatus=ok?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('h9', '提示用户\n检查 SSID/密码/频段', fillcolor='#FFCDD2', fontcolor='#B71C1C')
    c.node('h10', '发送 close_ap\n通知关闭热点', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h11', '断开 Modu_XXXX\n重新连接目标 WiFi', fillcolor='#FFF9C4', fontcolor='#F57F17')

edge(dot, 'h1', 'h2')
edge(dot, 'h2', 'h3', xlabel='  是')
edge(dot, 'h2', 'h1', xlabel='  否，继续扫描', style='dashed')
edge(dot, 'h3', 'h4')
edge(dot, 'h4', 'h5')
edge(dot, 'h5', 'h6')
edge(dot, 'h6', 'h7', xlabel='  是')
edge(dot, 'h6', 'h5', xlabel='  否，重输 PIN', style='dashed')
edge(dot, 'h7', 'h8')
edge(dot, 'h8', 'h10', xlabel='  是')
edge(dot, 'h8', 'h9', xlabel='  否')
edge(dot, 'h9', 'h7', xlabel='  修正后重发', style='dashed')
edge(dot, 'h10', 'h11')

dot.render(os.path.join(figures_dir, 'Fig001A_上位机配网流程'), cleanup=True)
print('Saved: Fig001A')


# ============================================================
# Flowchart 1B: 上位机 - 连接保障流程（阶段三/四/五）
# ============================================================
dot = new_graph('HostConnectFlow', '上位机（PC）连接保障流程图')
with cluster(dot, 'cluster_p2', '阶段三/四：服务通告与连接管理') as c:
    cattr(c, '阶段三/四：服务通告与连接管理')
    c.node('h13', '注册 mDNS 服务\nhost._tactile._tcp.local', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h14', '组播通告 224.0.2.1:5936\nhost_announce (1s)\n退出前广播 host_bye', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h15', '等待设备 TCP 连接', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h16', '收到 device_hello\n校验设备身份', fillcolor='#F44336', fontcolor='white')
    c.node('h17', '回复 host_ack\n心跳参数/server_time\n版本协商 (v3)\n(连接满回 busy)', fillcolor='#C8E6C9', fontcolor='#2E7D32')
with cluster(dot, 'cluster_p3', '阶段五：连接保障') as c:
    cattr(c, '阶段五：连接保障')
    c.node('h19', '心跳监控\n15s 无报文判死', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('h20', '设备注册表维护\n连接上限 16 (busy)\n质量统计/RSSI', fillcolor='#E3F2FD', fontcolor='#1A237E')

edge(dot, 'h13', 'h14')
edge(dot, 'h14', 'h15')
edge(dot, 'h15', 'h16', xlabel='  新连接')
edge(dot, 'h16', 'h17', xlabel='  校验通过')
edge(dot, 'h16', 'h15', xlabel='  校验失败断开', style='dashed')
edge(dot, 'h17', 'h19')
edge(dot, 'h19', 'h20')
edge(dot, 'h20', 'h15', xlabel='  继续监听\n(多设备)', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig001B_上位机连接保障流程'), cleanup=True)
print('Saved: Fig001B')


# ============================================================
# Flowchart 2A: 嵌入式设备 - 启动自检（阶段一）
# ============================================================
dot = new_graph('DeviceStartupFlow', '嵌入式设备（ESP32-C2）启动自检流程图')
with cluster(dot, 'cluster_d1', '阶段一：启动与自检') as c:
    cattr(c, '阶段一：启动与自检')
    c.node('d1', '上电启动\n读取 NVS 配置\n(配置版本校验)', shape='box', style='rounded,filled',
           fillcolor='#4CAF50', fontcolor='white')
    c.node('d33', '随机延迟 0~10s\n(上电错峰)', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d2', '有保存的\nWiFi 配置?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d3', '连接已保存 WiFi (STA)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d4', '连接结果?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d5', '进入阶段三\n服务发现', fillcolor='#C8E6C9', fontcolor='#2E7D32')
    c.node('d6', '认证失败 AUTH_FAIL\n(不可恢复)', fillcolor='#FFCDD2', fontcolor='#B71C1C')
    c.node('d32', '信号弱/超时 NO_AP\n退避重试 (1s~30s+抖动)\n连续 5 次失败', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d40', '有备用凭据集?\n切换备用 SSID 重试 (v3)', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d37', '进入阶段二\n配网模式', fillcolor='#FFCDD2', fontcolor='#B71C1C')

edge(dot, 'd1', 'd33')
edge(dot, 'd33', 'd2')
edge(dot, 'd2', 'd3', xlabel='  有')
edge(dot, 'd2', 'd37', xlabel='  无')
edge(dot, 'd3', 'd4')
edge(dot, 'd4', 'd5', xlabel='  成功')
edge(dot, 'd4', 'd6', xlabel='  认证失败')
edge(dot, 'd4', 'd32', xlabel='  信号弱/超时')
edge(dot, 'd6', 'd37')
edge(dot, 'd32', 'd3', xlabel='  重试', style='dashed')
edge(dot, 'd32', 'd40', xlabel='  5 次失败', style='dashed')
edge(dot, 'd40', 'd3', xlabel='  有备用，切换重试', style='dashed')
edge(dot, 'd40', 'd37', xlabel='  凭据集耗尽', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig002A_设备启动自检流程'), cleanup=True)
print('Saved: Fig002A')


# ============================================================
# Flowchart 2B: 嵌入式设备 - 配网（阶段二）
# ============================================================
dot = new_graph('DeviceProvisionFlow', '嵌入式设备（ESP32-C2）配网流程图')
with cluster(dot, 'cluster_d2', '阶段二：配网 (SoftAP 会话模式)') as c:
    cattr(c, '阶段二：配网 (SoftAP 会话模式)')
    c.node('d7', '开启 WPA2 热点 Modu_XXXX\n(每设备唯一密码)\nTCP Server :5935\n一次性 PIN', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d8', '等待 auth\n10s 超时/PIN 一次性\n错 5 次关热点', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d9', '认证通过?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d10', '等待 wifi_config (60s 超时)\n校验 SSID/密码/2.4G', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d12', '先尝试连接目标 WiFi\n(STA)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d13', '连接成功?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d11', '写入 NVS + 标记未确认\n配网确认 (v3)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d14', '回复 fail\n不动 NVS，保持 AP 重试', fillcolor='#FFCDD2', fontcolor='#B71C1C')
    c.node('d15', '回复 ok', fillcolor='#C8E6C9', fontcolor='#2E7D32')
    c.node('d16', '收到 close_ap?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d17', '关闭 AP\n保持 STA', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d38', '进入阶段三\n服务发现', fillcolor='#C8E6C9', fontcolor='#2E7D32')
    c.node('d31', '120s 无认证完成\n关闭热点, 退避 5min', fillcolor='#FFCDD2', fontcolor='#B71C1C')
    c.node('d39', '回阶段一\n启动判断', fillcolor='#FFCDD2', fontcolor='#B71C1C')

edge(dot, 'd7', 'd8')
edge(dot, 'd8', 'd9')
edge(dot, 'd9', 'd10', xlabel='  是')
edge(dot, 'd9', 'd8', xlabel='  否/超时', style='dashed')
edge(dot, 'd10', 'd12', xlabel='  收到')
edge(dot, 'd12', 'd13')
edge(dot, 'd13', 'd11', xlabel='  是')
edge(dot, 'd11', 'd15')
edge(dot, 'd13', 'd14', xlabel='  否')
edge(dot, 'd14', 'd8', xlabel='  等待重发', style='dashed')
edge(dot, 'd15', 'd16')
edge(dot, 'd16', 'd17', xlabel='  是')
edge(dot, 'd17', 'd38')
edge(dot, 'd8', 'd31', xlabel='  120s 超时', style='dashed')
edge(dot, 'd31', 'd39', xlabel='  退避后重启', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig002B_设备配网流程'), cleanup=True)
print('Saved: Fig002B')


# ============================================================
# Flowchart 2C: 嵌入式设备 - 服务发现（阶段三）
# ============================================================
dot = new_graph('DeviceDiscoveryFlow', '嵌入式设备（ESP32-C2）服务发现流程图')
with cluster(dot, 'cluster_d3', '阶段三：服务发现（三级降级）') as c:
    cattr(c, '阶段三：服务发现（三级降级）')
    c.node('d18', 'mDNS 解析\nhost._tactile._tcp.local', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d19', '解析成功?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d20', 'UDP 组播监听\n224.0.2.1:5936\n(30s 快速轮询)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d21', '30s 内收到\nhost_announce?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d22', '候选列表直连\n(NVS 主/备地址)', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d23', '获得上位机\nIP + Port', fillcolor='#C8E6C9', fontcolor='#2E7D32')

edge(dot, 'd18', 'd19')
edge(dot, 'd19', 'd23', xlabel='  是')
edge(dot, 'd19', 'd20', xlabel='  否')
edge(dot, 'd20', 'd21')
edge(dot, 'd21', 'd23', xlabel='  是')
edge(dot, 'd21', 'd22', xlabel='  否')
edge(dot, 'd22', 'd23', xlabel='  直连尝试')
edge(dot, 'd22', 'd18', xlabel='  失败循环', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig002C_设备服务发现流程'), cleanup=True)
print('Saved: Fig002C')


# ============================================================
# Flowchart 2D: 嵌入式设备 - 连接与自愈（阶段四/五）
# ============================================================
dot = new_graph('DeviceConnectFlow', '嵌入式设备（ESP32-C2）连接与自愈流程图')
with cluster(dot, 'cluster_d4', '阶段四：连接与会话保活') as c:
    cattr(c, '阶段四：连接与会话保活')
    c.node('d24', 'TCP 连接上位机\n长度前缀帧 (v3)\n发送 device_hello (proto_ver)', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d25', '收到 host_ack\n版本协商/注册成功', fillcolor='#F44336', fontcolor='white')
    c.node('d26', '心跳保活\n10s ping(seq)/pong\n15s 无响应判死', fillcolor='#C8E6C9', fontcolor='#2E7D32')
with cluster(dot, 'cluster_d5', '阶段五：异常自愈') as c:
    cattr(c, '阶段五：异常自愈')
    c.node('d34', 'RSSI 监测\n<-75dBm 持续 30s\n→ 主动重连', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d27', 'TCP 断开/心跳超时?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d28', '重连候选主机\n退避+抖动 (1s~30s)\n30s 未成转发现', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d35', '收到 host_bye\n转候选直连 + 快速轮询', fillcolor='#E3F2FD', fontcolor='#1A237E')
    c.node('d29', 'WiFi 断开?', shape='diamond', style='filled',
           fillcolor='#FFF3E0', fontcolor='#BF360C')
    c.node('d30', '退避重连 WiFi\n连续 5 次失败回启动判断', fillcolor='#FFF9C4', fontcolor='#F57F17')
    c.node('d36', '回阶段一\n启动判断', fillcolor='#FFCDD2', fontcolor='#B71C1C')

edge(dot, 'd24', 'd25')
edge(dot, 'd25', 'd26')
edge(dot, 'd26', 'd34', xlabel='  并行监测', style='dashed')
edge(dot, 'd34', 'd27', xlabel='  异常判定', style='dashed')
edge(dot, 'd26', 'd27')
edge(dot, 'd26', 'd35', xlabel='  收到 host_bye', style='dashed')
edge(dot, 'd35', 'd22', xlabel='  转候选直连', style='dashed')
edge(dot, 'd27', 'd28', xlabel='  断开')
edge(dot, 'd27', 'd29', xlabel='  正常', style='dashed')
edge(dot, 'd28', 'd24', xlabel='  重连成功', style='dashed')
edge(dot, 'd28', 'd20', xlabel='  30s 未成功\n转组播发现', style='dashed')
edge(dot, 'd29', 'd30', xlabel='  断开')
edge(dot, 'd29', 'd26', xlabel='  正常', style='dashed')
edge(dot, 'd30', 'd26', xlabel='  重连成功', style='dashed')
edge(dot, 'd30', 'd36', xlabel='  5 次失败', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig002D_设备连接与自愈流程'), cleanup=True)
print('Saved: Fig002D')
