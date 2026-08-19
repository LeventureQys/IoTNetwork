# -*- coding: utf-8 -*-
"""beta v1.1 简化版流程图生成器。

用法：python scripts/draw_flowcharts_v11.py
输出：figures/Fig101_PC启动序列.png、figures/Fig102_设备六态状态机.png
依赖：python + graphviz（pip install graphviz），Windows 下使用 Microsoft YaHei 字体。
"""
import os
import sys

# Windows 上 graphviz 的 dot 可能不在 PATH，自动补入常见安装位置
if sys.platform == 'win32':
    _candidates = [
        r'C:\Program Files\Graphviz\bin',
        r'C:\Program Files (x86)\Graphviz\bin',
    ]
    for _p in _candidates:
        if os.path.isdir(_p) and _p not in os.environ.get('PATH', ''):
            os.environ['PATH'] = _p + ';' + os.environ.get('PATH', '')

from graphviz import Digraph

FIGURES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'figures')
os.makedirs(FIGURES_DIR, exist_ok=True)

NODE_ATTR = {
    'shape': 'box', 'style': 'rounded,filled',
    'fontname': 'Microsoft YaHei', 'fontsize': '10',
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
    'fontname': 'Microsoft YaHei', 'fontsize': '9',
    'fontcolor': '#BF360C'
}

GREEN = '#4CAF50'
GREEN_LIGHT = '#C8E6C9'
BLUE = '#E3F2FD'
YELLOW = '#FFF9C4'
ORANGE = '#FFF3E0'
RED_LIGHT = '#FFCDD2'
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
# Fig101：PC 启动序列（热点 → 固定 IP → TCP）
# ============================================================
dot = new_graph('Fig101_PcStartup', 'PC 启动序列（beta v1.1）')
dot.node('p1', '校验配置\nSSID/密码/IP/端口', fillcolor=GREEN, fontcolor='white')
dot.node('p2', '启动 WPA2 热点\nSSID=Modu_PC（Modu_ 前缀）', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p3', '热点 IPv4 =\n192.168.137.1/24 ?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('p4', '配置/修正热点 IP\n(wifi_ap_configure_ipv4)', fillcolor=YELLOW, fontcolor=AMBER)
dot.node('p5', '复核通过?', shape='diamond', fillcolor=ORANGE, fontcolor=DARK_RED)
dot.node('p6', '监听 TCP 0.0.0.0:5935\n(事件 tcp_listening)', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('p7', '等待唯一设备连接\n(事件 ready)', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('p8', 'device_hello → host_ack\n会话在线 + 心跳', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('r0', '拒绝启动\n退出码 1', fillcolor=RED_LIGHT, fontcolor=DARK_RED)
dot.node('r1', '停止热点并回滚\n退出码 2', fillcolor=RED_LIGHT, fontcolor=DARK_RED)

edge(dot, 'p1', 'p2', xlabel='通过')
edge(dot, 'p1', 'r0', xlabel='非法')
edge(dot, 'p2', 'p3')
edge(dot, 'p2', 'r1', xlabel='失败')
edge(dot, 'p3', 'p6', xlabel='是')
edge(dot, 'p3', 'p4', xlabel='否')
edge(dot, 'p4', 'p5')
edge(dot, 'p5', 'p6', xlabel='通过')
edge(dot, 'p5', 'r1', xlabel='仍不匹配')
edge(dot, 'p6', 'p7')
edge(dot, 'p6', 'r1', xlabel='端口占用')
edge(dot, 'p7', 'p8', xlabel='设备连接')

dot.render(filename='Fig101_PC启动序列', directory=FIGURES_DIR, cleanup=True)
print('generated Fig101_PC启动序列.png')

# ============================================================
# Fig102：设备六态状态机
# ============================================================
dot = new_graph('Fig102_DeviceStateMachine', '设备六态状态机（beta v1.1）')
dot.node('s0', 'BOOT\n配置校验 + 上电错峰', fillcolor=GREEN, fontcolor='white')
dot.node('s1', 'WIFI_SCAN\n扫描精确 SSID', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('s2', 'STA_JOIN\n连接热点 + 校验 IP/SSID', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('s3', 'CONNECT\nTCP 直连 192.168.137.1:5935\n+ device_hello', fillcolor=BLUE, fontcolor=DARK_BLUE)
dot.node('s4', 'SESSION\nping/pong + app_data', fillcolor=GREEN_LIGHT, fontcolor=DARK_GREEN)
dot.node('s5', 'HEAL\n退避自愈', fillcolor=YELLOW, fontcolor=AMBER)

edge(dot, 's0', 's1', xlabel='错峰到期')
edge(dot, 's1', 's2', xlabel='wifi_target_found')
edge(dot, 's1', 's1', xlabel='未找到：WiFi 退避', style='dashed')
edge(dot, 's2', 's3', xlabel='wifi_connected')
edge(dot, 's2', 's5', xlabel='认证/校验失败', style='dashed')
edge(dot, 's3', 's4', xlabel='host_ack ok')
edge(dot, 's3', 's5', xlabel='TCP/握手失败', style='dashed')
edge(dot, 's4', 's5', xlabel='断线/超时/busy')
edge(dot, 's5', 's3', xlabel='WiFi 健康：TCP 退避重连')
edge(dot, 's5', 's1', xlabel='WiFi 失效：断 STA 回扫描', style='dashed')

dot.render(filename='Fig102_设备六态状态机', directory=FIGURES_DIR, cleanup=True)
print('generated Fig102_设备六态状态机.png')
