import os
from graphviz import Digraph

figures_dir = os.path.join(os.path.dirname(__file__), '..', 'figures')

NODE_ATTR = {
    'shape': 'box', 'style': 'rounded,filled',
    'fontname': 'Microsoft YaHei', 'fontsize': '9',
    'margin': '0.15,0.1'
}
GRAPH_ATTR = {
    'nodesep': '0.6', 'ranksep': '0.7',
    'splines': 'ortho',
    'fontname': 'Microsoft YaHei', 'fontsize': '16',
    'fontcolor': '#1A237E', 'dpi': '200',
    'pad': '0.3'
}
EDGE_LABEL = {
    'fontname': 'Microsoft YaHei', 'fontsize': '8',
    'fontcolor': '#BF360C'
}

GREEN = '#4CAF50'
BLUE = '#E3F2FD'
YELLOW = '#FFF9C4'
RED_LIGHT = '#FFCDD2'
GREEN_LIGHT = '#C8E6C9'
ORANGE = '#FFF3E0'
RED = '#F44336'
DARK_BLUE = '#1A237E'
DARK_GREEN = '#2E7D32'
DARK_RED = '#B71C1C'
AMBER = '#F57F17'


def new_graph(name, label):
    dot = Digraph(name=name, format='png',
                  node_attr=dict(NODE_ATTR),
                  graph_attr=dict(GRAPH_ATTR))
    dot.attr(rankdir='TB', label=label, labelloc='t')
    return dot


def edge(dot, a, b, xlabel=None, style=None, color=None):
    kwargs = {}
    if xlabel:
        kwargs['xlabel'] = xlabel
    if style:
        kwargs['style'] = style
    if color:
        kwargs['color'] = color
        kwargs['fontcolor'] = color
    dot.edge(a, b, **EDGE_LABEL, **kwargs)


# ============================================================
# Fig01: 设备启动自检（阶段一）
# ============================================================
dot = new_graph('Fig01_DeviceBoot', '设备启动自检流程')
dot.node('d1', '上电启动\n读取 NVS 配置', fillcolor=GREEN, fontcolor='white')
dot.node('d2', '随机延迟 0~10s\n(上电错峰)', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('d3', '有保存的\nWiFi 配置?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('d4', '连接已保存 WiFi\n(STA 模式)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('d5', '连接结果?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('d6', '连接成功\n进入服务发现', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('d7', '认证失败\nAUTH_FAIL 202', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('d8', '信号弱/超时\n退避重试 1~30s', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('d9', '有备用凭据?\n切换备用 SSID', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('d10', '进入配网模式\n(SoftAP)', fillcolor=RED_LIGHT, fontcolor=DARK_RED)

edge(dot, 'd1', 'd2')
edge(dot, 'd2', 'd3')
edge(dot, 'd3', 'd4', xlabel='有')
edge(dot, 'd3', 'd10', xlabel='无')
edge(dot, 'd4', 'd5')
edge(dot, 'd5', 'd6', xlabel='成功')
edge(dot, 'd5', 'd7', xlabel='认证失败')
edge(dot, 'd5', 'd8', xlabel='信号弱')
edge(dot, 'd7', 'd10')
edge(dot, 'd8', 'd4', xlabel='重试', style='dashed')
edge(dot, 'd8', 'd9', xlabel='5次失败', style='dashed')
edge(dot, 'd9', 'd4', xlabel='有备用', style='dashed')
edge(dot, 'd9', 'd10', xlabel='耗尽', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig01_设备启动自检'), cleanup=True)
print('Saved: Fig01')


# ============================================================
# Fig02: 设备 SoftAP 配网（阶段二）
# ============================================================
dot = new_graph('Fig02_DeviceProvision', '设备 SoftAP 配网流程')
dot.node('p1', '开启 WPA2 热点\nModu_XXXX\nTCP :5935 + 一次性 PIN', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('p2', '等待 auth\n10s 超时 / PIN 一次性\n错5次关热点', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p3', '认证通过?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('p4', '等待 wifi_config\n60s超时 / 校验SSID+密码', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p5', '先尝试连接\n目标 WiFi (STA)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p6', '连接成功?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('p7', '写入 NVS\n标记"未确认"', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p8', '回复 fail\n不动 NVS，保持 AP', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('p9', '回复 ok\n等待 close_ap', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('p10', '收到 close_ap?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('p11', '关闭 AP\n保持 STA', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p12', '进入阶段三\n服务发现', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('p13', '120s 无认证\n关热点 退避5min', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('p14', '回到阶段一\n重新判断', fillcolor=RED_LIGHT, fontcolor=DARK_RED)

edge(dot, 'p1', 'p2')
edge(dot, 'p2', 'p3')
edge(dot, 'p3', 'p4', xlabel='是')
edge(dot, 'p3', 'p2', xlabel='否/超时', style='dashed')
edge(dot, 'p4', 'p5', xlabel='收到')
edge(dot, 'p5', 'p6')
edge(dot, 'p6', 'p7', xlabel='是')
edge(dot, 'p7', 'p9')
edge(dot, 'p6', 'p8', xlabel='否')
edge(dot, 'p8', 'p2', xlabel='等待重发', style='dashed')
edge(dot, 'p9', 'p10')
edge(dot, 'p10', 'p11', xlabel='是')
edge(dot, 'p11', 'p12')
edge(dot, 'p2', 'p13', xlabel='120s超时', style='dashed')
edge(dot, 'p13', 'p14', xlabel='退避后', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig02_设备SoftAP配网'), cleanup=True)
print('Saved: Fig02')


# ============================================================
# Fig03: 设备服务发现（阶段三）
# ============================================================
dot = new_graph('Fig03_DeviceDiscovery', '设备服务发现流程（三级降级）')
dot.node('s1', 'mDNS 解析\nhost._tactile._tcp.local', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('s2', '解析成功?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('s3', 'UDP 组播监听\n224.0.2.1:5936\n(30s 快速轮询)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('s4', '30s 内收到\nhost_announce?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('s5', '候选列表直连\nNVS 主/备地址', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('s6', '获得上位机\nIP + Port', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('s7', '失败循环\n回到 mDNS', fillcolor=RED_LIGHT, fontcolor=DARK_RED)

edge(dot, 's1', 's2')
edge(dot, 's2', 's6', xlabel='是')
edge(dot, 's2', 's3', xlabel='否')
edge(dot, 's3', 's4')
edge(dot, 's4', 's6', xlabel='是')
edge(dot, 's4', 's5', xlabel='否')
edge(dot, 's5', 's6', xlabel='直连成功')
edge(dot, 's5', 's7', xlabel='失败', style='dashed')
edge(dot, 's7', 's1', xlabel='循环重试', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig03_设备服务发现'), cleanup=True)
print('Saved: Fig03')


# ============================================================
# Fig04: 设备连接与自愈（阶段四/五）
# ============================================================
dot = new_graph('Fig04_DeviceConnect', '设备连接与自愈流程')

dot.node('c1', 'TCP 连接上位机\n发送 device_hello\n(proto_ver)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('c2', '收到 host_ack\n版本协商 / 注册确认', fillcolor=RED, fontcolor='white')
dot.node('c3', '心跳保活\n10s ping/pong\n15s 判死', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('c4', 'RSSI 监测\n<-75dBm 持续30s\n主动重连', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('c5', 'TCP 断开\n或心跳超时?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('c6', '重连候选主机\n退避1~30s+抖动', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('c7', '收到 host_bye\n转候选直连', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('c8', 'WiFi 断开?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('c9', '退避重连 WiFi\n5次失败回启动判断', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('c10', '回到阶段一\n重新判断', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('c11', '30s未成功\n转组播发现', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('c12', '重连成功\n恢复心跳', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)

edge(dot, 'c1', 'c2')
edge(dot, 'c2', 'c3')
edge(dot, 'c3', 'c4', xlabel='并行', style='dashed')
edge(dot, 'c4', 'c5', xlabel='异常', style='dashed')
edge(dot, 'c3', 'c5')
edge(dot, 'c3', 'c7', xlabel='host_bye', style='dashed')
edge(dot, 'c7', 'c6', xlabel='转直连', style='dashed')
edge(dot, 'c5', 'c6', xlabel='断开')
edge(dot, 'c5', 'c8', xlabel='正常', style='dashed')
edge(dot, 'c6', 'c12', xlabel='成功', style='dashed')
edge(dot, 'c12', 'c3', style='dashed')
edge(dot, 'c6', 'c11', xlabel='30s失败', style='dashed')
edge(dot, 'c11', 'c1', xlabel='重新发现', style='dashed')
edge(dot, 'c8', 'c9', xlabel='断开')
edge(dot, 'c8', 'c3', xlabel='正常', style='dashed')
edge(dot, 'c9', 'c12', xlabel='重连成功', style='dashed')
edge(dot, 'c9', 'c10', xlabel='5次失败', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig04_设备连接与自愈'), cleanup=True)
print('Saved: Fig04')


# ============================================================
# Fig05: 上位机配网流程
# ============================================================
dot = new_graph('Fig05_HostProvision', '上位机配网流程')

dot.node('h1', '扫描 WiFi 热点\n(标注频段)', fillcolor=GREEN, fontcolor='white')
dot.node('h2', '发现 Modu_XXXX\n热点?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('h3', '用户选择目标设备\n(如 Modu_EEFF)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('h4', '连接 Modu_XXXX\n(暂时断开当前网络)', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('h5', 'TCP 连接 :5935\n发送 auth PIN', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('h6', '认证通过?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('h7', '发送 wifi_config\n(SSID + 密码)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('h8', 'wifi_result\nstatus=ok?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('h9', '提示用户\n检查密码/频段', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('h10', '发送 close_ap\n通知关热点', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('h11', '断开 Modu_XXXX\n切回目标 WiFi', fillcolor=YELLOW, fontcolor=AMBER)

edge(dot, 'h1', 'h2')
edge(dot, 'h2', 'h3', xlabel='是')
edge(dot, 'h2', 'h1', xlabel='否 继续', style='dashed')
edge(dot, 'h3', 'h4')
edge(dot, 'h4', 'h5')
edge(dot, 'h5', 'h6')
edge(dot, 'h6', 'h7', xlabel='是')
edge(dot, 'h6', 'h5', xlabel='否 重输', style='dashed')
edge(dot, 'h7', 'h8')
edge(dot, 'h8', 'h10', xlabel='是')
edge(dot, 'h8', 'h9', xlabel='否')
edge(dot, 'h9', 'h7', xlabel='修正重发', style='dashed')
edge(dot, 'h10', 'h11')

dot.render(os.path.join(figures_dir, 'Fig05_上位机配网流程'), cleanup=True)
print('Saved: Fig05')


# ============================================================
# Fig06: 上位机连接保障流程
# ============================================================
dot = new_graph('Fig06_HostConnect', '上位机连接保障流程')

dot.node('g1', '注册 mDNS 服务\nhost._tactile._tcp.local', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('g2', '组播 host_announce\n224.0.2.1:5936 (1s)\n退出广播 host_bye', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('g3', '等待设备 TCP 连接\n(端口 5935)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('g4', '收到 device_hello\n校验设备身份', fillcolor=RED, fontcolor='white')
dot.node('g5', '回复 host_ack\n心跳参数/server_time\n版本协商 (连接满回 busy)', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('g6', '心跳监控\n15s 无报文判死', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('g7', '设备注册表维护\n上限16 (busy拒绝)\n质量统计/RSSI', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('g8', '校验失败\n断开连接', fillcolor=RED_LIGHT, fontcolor=DARK_RED)

edge(dot, 'g1', 'g2')
edge(dot, 'g2', 'g3')
edge(dot, 'g3', 'g4', xlabel='新连接')
edge(dot, 'g4', 'g5', xlabel='通过')
edge(dot, 'g4', 'g8', xlabel='失败', style='dashed')
edge(dot, 'g8', 'g3', xlabel='继续监听', style='dashed')
edge(dot, 'g5', 'g6')
edge(dot, 'g6', 'g7')
edge(dot, 'g7', 'g3', xlabel='多设备监听', style='dashed')

dot.render(os.path.join(figures_dir, 'Fig06_上位机连接保障流程'), cleanup=True)
print('Saved: Fig06')

print('\nAll flowcharts generated successfully.')
