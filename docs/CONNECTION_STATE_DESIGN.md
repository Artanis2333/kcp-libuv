# KCP连接状态管理设计说明

## KCP vs TCP 连接管理

### TCP的连接管理
TCP是面向连接的协议，内置了完整的连接管理机制：
- **三次握手建立连接**：SYN -> SYN-ACK -> ACK
- **四次挥手断开连接**：FIN -> ACK -> FIN -> ACK
- 这些机制在TCP协议栈内部自动完成

### KCP的连接管理
**KCP本身不包含连接管理机制！**

KCP是一个纯粹的ARQ（自动重传请求）协议，只负责：
- 数据的可靠传输
- 拥塞控制
- 流量控制
- 快速重传

**KCP不负责：**
- ❌ 连接建立（没有握手机制）
- ❌ 连接断开（没有挥手机制）
- ❌ 连接状态管理

## 当前框架的设计

### 简化实现（当前版本）
```
客户端                    服务器
  |                         |
  | ---- KCP数据包 ------>  | (收到第一个包，创建连接，状态=CONNECTED)
  | <--- KCP数据包 ------  |
  |                         |
  | ---- close() --------> | (直接关闭，状态=DISCONNECTED)
```

**优点：**
- 简单直接，易于理解
- 适合内网或受信环境
- 减少延迟（无握手开销）

**缺点：**
- 无法防止伪造连接
- 无法协商连接参数
- 关闭不够优雅

## 是否需要实现握手机制？

### 场景分析

#### 场景1：不需要握手（当前实现）
**适用场景：**
- 内网环境（局域网游戏、内部服务）
- 受信环境（已知客户端）
- 追求极致性能（每毫秒都重要）
- 简单应用

**实现：**
```cpp
// 服务器：收到第一个KCP包就创建连接
conn->set_state(CONNECTED);

// 客户端：connect后直接发送数据
conn->set_state(CONNECTED);
conn->send(data, len);
```

#### 场景2：需要握手（推荐生产环境）
**适用场景：**
- 公网服务
- 需要安全验证
- 需要协商参数（MTU、窗口大小等）
- 防止攻击（SYN Flood等）

**实现建议：**

```cpp
// === 定义握手消息类型 ===
enum PacketType {
    PKT_SYN = 1,      // 请求连接
    PKT_SYN_ACK = 2,  // 确认连接
    PKT_ACK = 3,      // 握手完成
    PKT_FIN = 4,      // 请求断开
    PKT_FIN_ACK = 5,  // 确认断开
    PKT_DATA = 6      // 普通数据
};

struct HandshakePacket {
    uint32_t type;        // 消息类型
    uint32_t conv;        // 会话ID
    uint32_t token;       // 验证令牌
    uint32_t timestamp;   // 时间戳
};

// === 三次握手建立连接 ===

// 客户端：
// 1. 发送SYN
void connect() {
    state_ = CONNECTING;
    HandshakePacket pkt = {PKT_SYN, conv_, generate_token(), get_time()};
    send_udp_direct((char*)&pkt, sizeof(pkt));
}

// 2. 收到SYN-ACK
void on_syn_ack(HandshakePacket* pkt) {
    if (state_ == CONNECTING && verify_token(pkt->token)) {
        HandshakePacket ack = {PKT_ACK, conv_, pkt->token, get_time()};
        send_udp_direct((char*)&ack, sizeof(ack));
        state_ = CONNECTED;
    }
}

// 服务器：
// 1. 收到SYN
void on_syn(HandshakePacket* pkt) {
    auto conn = create_connection(pkt->conv);
    conn->set_state(CONNECTING);
    HandshakePacket syn_ack = {PKT_SYN_ACK, pkt->conv, generate_token(), get_time()};
    conn->send_udp_direct((char*)&syn_ack, sizeof(syn_ack));
}

// 2. 收到ACK
void on_ack(HandshakePacket* pkt) {
    auto conn = find_connection(pkt->conv);
    if (conn && conn->get_state() == CONNECTING && verify_token(pkt->token)) {
        conn->set_state(CONNECTED);
    }
}

// === 四次挥手断开连接 ===

// 主动方：
// 1. 发送FIN
void close() {
    if (state_ == CONNECTED) {
        state_ = DISCONNECTING;
        HandshakePacket fin = {PKT_FIN, conv_, 0, get_time()};
        send_udp_direct((char*)&fin, sizeof(fin));
    }
}

// 2. 收到FIN-ACK
void on_fin_ack(HandshakePacket* pkt) {
    if (state_ == DISCONNECTING) {
        state_ = DISCONNECTED;
    }
}

// 被动方：
// 1. 收到FIN
void on_fin(HandshakePacket* pkt) {
    auto conn = find_connection(pkt->conv);
    if (conn && conn->get_state() == CONNECTED) {
        // 回复ACK
        HandshakePacket ack = {PKT_ACK, pkt->conv, 0, get_time()};
        conn->send_udp_direct((char*)&ack, sizeof(ack));
        
        // 发送自己的FIN
        HandshakePacket fin = {PKT_FIN, pkt->conv, 0, get_time()};
        conn->send_udp_direct((char*)&fin, sizeof(fin));
        
        conn->set_state(DISCONNECTING);
    }
}

// 2. 收到ACK
void on_ack_for_fin(HandshakePacket* pkt) {
    auto conn = find_connection(pkt->conv);
    if (conn && conn->get_state() == DISCONNECTING) {
        conn->set_state(DISCONNECTED);
    }
}
```

## 完整状态转换图

### 带握手的状态机
```
客户端                              服务器

[初始]                              [初始]
  |                                   |
  | ---- SYN (token=ABC) ----------> |
  |                                   |
[CONNECTING]                    [创建连接]
  |                             [CONNECTING]
  |                                   |
  | <--- SYN-ACK (token=ABC) ------- |
  |                                   |
  | ---- ACK (token=ABC) ----------> |
  |                                   |
[CONNECTED]  <-- 可以发送数据 --> [CONNECTED]
  |                                   |
  |          数据交互...              |
  |                                   |
  | ---- FIN -----------------------  |
  |                                   |
[DISCONNECTING]                       |
  |                             [收到FIN]
  | <--- ACK ------------------------ |
  |                                   |
  | <--- FIN ------------------------ |
  |                              [DISCONNECTING]
  | ---- ACK ----------------------> |
  |                                   |
[DISCONNECTED]                  [DISCONNECTED]
```

## 实现建议

### 方案A：简单模式（当前实现）
**适用：**内网、demo、快速原型

**实现：**无握手，直接通信

### 方案B：基础握手（推荐）
**适用：**一般生产环境

**实现：**
- 三次握手建立连接
- 带令牌验证
- 超时重传SYN/FIN

### 方案C：完整握手（高安全要求）
**适用：**公网、高安全场景

**实现：**
- 三次握手 + 加密令牌
- 四次挥手 + 等待队列清空
- 防重放攻击（时间戳）
- 防SYN Flood（限速）
- 连接令牌验证

## 当前框架的选择

当前框架采用**方案A（简单模式）**，原因：
1. 作为示例，降低复杂度
2. 适合学习和理解KCP原理
3. 内网环境完全够用
4. 用户可以根据需求自行扩展

## 如何扩展

如果需要实现握手机制，可以：

1. **在应用层实现握手协议**
   - 定义握手包格式
   - 使用`send_udp_direct()`发送握手包
   - 在数据回调中识别并处理握手包

2. **修改框架支持握手**
   - 在`KCPConnection`中添加握手逻辑
   - 在`KCPServer`中添加握手验证
   - 提供配置选项启用/禁用握手

3. **参考现有实现**
   - KCPTUN：使用简单的令牌验证
   - KCP with QUIC style：借鉴QUIC的握手
   - GameNetworkingSockets：Valve的实现

## 总结

- KCP本身**不包含**连接管理
- 当前框架采用**简化实现**（无握手）
- 生产环境建议实现**基础握手**（三次握手+令牌）
- 高安全场景实现**完整握手**（四次挥手+加密验证）
- 框架已提供扩展接口（`send_udp_direct`、状态管理等）

选择哪种方案取决于具体应用场景！
