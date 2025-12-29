# 基于KCP和libuv的可靠UDP服务框架

## 项目简介

本项目实现了一个基于KCP协议和libuv事件循环的高性能可靠UDP服务框架，提供了完整的连接管理功能。

## KCP协议参数详解

### 1. ikcp_create(IUINT32 conv, void *user)
创建KCP控制块
- **conv**: 会话编号（Conversation ID）
  - 含义：用于标识一个KCP连接，通信双方的conv必须相同
  - 建议范围：1 - 0xFFFFFFFF（不能为0）
  - 使用建议：可以使用连接ID、时间戳等唯一值

### 2. ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc)
设置KCP工作模式，控制延迟和性能平衡
- **nodelay**: 是否启用nodelay模式
  - 0：禁用（默认模式）
  - 1：启用（低延迟模式）
  - 建议：游戏、实时通信使用1，普通应用使用0
  
- **interval**: 内部更新时钟，单位毫秒
  - 含义：KCP内部flush的时间间隔
  - 默认值：100ms
  - 建议范围：10-100ms
  - 推荐值：
    - 普通模式：100ms
    - 快速模式：20-40ms
    - 极速模式：10ms
  
- **resend**: 快速重传模式
  - 0：关闭快速重传（默认）
  - 1：启用快速重传（发现丢包立即重传）
  - 2：快速重传（更激进的策略）
  - 建议范围：0-2
  - 推荐：网络不稳定时使用1或2
  
- **nc**: 是否关闭拥塞控制
  - 0：启用拥塞控制（默认）
  - 1：关闭拥塞控制
  - 建议：带宽充足且追求低延迟时设为1，否则设为0

### 3. ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd)
设置发送和接收窗口大小
- **sndwnd**: 发送窗口大小
  - 含义：最多可以发送多少个未确认的包
  - 默认值：32
  - 建议范围：32-512
  - 推荐值：
    - 低延迟场景：32-64
    - 高吞吐场景：128-256
    - 极高吞吐：256-512
  
- **rcvwnd**: 接收窗口大小
  - 含义：最多可以接收多少个未处理的包
  - 默认值：128
  - 建议范围：128-1024
  - 推荐值：一般设为sndwnd的2-4倍

### 4. ikcp_setmtu(ikcpcb *kcp, int mtu)
设置最大传输单元
- **mtu**: 最大传输单元大小，单位字节
  - 含义：每个UDP包的最大载荷大小
  - 默认值：1400字节
  - 建议范围：512-1472字节
  - 推荐值：
    - 公网环境：1200-1400字节（避免IP分片）
    - 内网环境：可以设为1472字节（以太网MTU 1500 - IP头20 - UDP头8）
    - 移动网络：1200字节以下

### 5. ikcp_update(ikcpcb *kcp, IUINT32 current)
更新KCP状态（必须定期调用）
- **current**: 当前时间戳，单位毫秒
  - 含义：KCP内部用于计算超时和重传的时间基准
  - 必须是单调递增的时间戳
  - 建议：使用系统启动时间或统一的时间基准

### 6. ikcp_send(ikcpcb *kcp, const char *buffer, int len)
发送数据
- **buffer**: 数据缓冲区
- **len**: 数据长度
  - 建议范围：1-任意长度（KCP会自动分片）
  - 注意：大数据会被分成多个包发送

### 7. ikcp_recv(ikcpcb *kcp, char *buffer, int len)
接收数据
- **buffer**: 接收缓冲区
- **len**: 缓冲区大小
- 返回值：实际接收的字节数，<0表示没有数据

### 8. kcp->rx_minrto（最小RTO）
设置最小重传超时时间
- **含义**: RTO（Retransmission TimeOut）的最小值
- **默认值**: 100ms
- **建议范围**: 10-100ms
- **推荐值**:
  - 低延迟场景：30ms
  - 标准场景：100ms
- **注意**: 设置过小可能导致不必要的重传，增加网络负担

### 9. kcp->fastresend（快速重传触发次数）
设置快速重传的ACK跨越次数
- **含义**: 收到多少个后续ACK就触发快速重传
- **默认值**: 0（关闭）
- **建议范围**: 0-5
- **推荐值**:
  - 标准模式：0（关闭）
  - 快速模式：2
  - 极速模式：1
- **示例**: 设置为2时，收到ACK 5,7,8会立即重传包6
- **注意**: 与nodelay的resend参数配合使用效果更佳

### 10. kcp->stream（流模式）
设置消息模式或流模式
- **0**: 消息模式（默认）
  - 保留消息边界
  - 每次recv接收完整消息
  - 适合大多数应用场景
- **1**: 流模式
  - 类似TCP，数据连续无边界
  - 适合传输大量连续数据
- **建议**: 大多数情况使用消息模式（0）

### 11. kcp->dead_link（死链接检测）
设置最大重传次数
- **含义**: 数据包重传达到此次数后认为连接断开
- **默认值**: 20次
- **建议范围**: 10-100次
- **推荐值**:
  - 稳定网络：20次
  - 不稳定网络：50次
  - 移动网络：30-40次
- **注意**: 设置过大会延迟连接断开检测

### 12. ikcp_waitsnd(ikcpcb *kcp)
获取等待发送的包数量
- **返回值**: 发送队列和发送缓冲区中的包总数
- **用途**:
  - 流量控制：waitsnd过大时暂停发送
  - 拥塞检测：判断网络是否拥塞
  - 队列管理：避免内存占用过大
- **建议**: 当waitsnd > 1000时考虑暂停发送

## libuv参数详解

### 1. uv_loop_init(uv_loop_t *loop)
初始化事件循环
- 无参数，使用默认配置

### 2. uv_udp_init(uv_loop_t *loop, uv_udp_t *handle)
初始化UDP句柄
- **loop**: 事件循环指针
- **handle**: UDP句柄指针

### 3. uv_ip4_addr(const char *ip, int port, struct sockaddr_in *addr)
创建IPv4地址结构
- **ip**: IP地址字符串（如"0.0.0.0"）
- **port**: 端口号
  - 建议范围：1024-65535
  - 注意：1024以下需要root权限
- **addr**: 输出的地址结构

### 4. uv_udp_bind(uv_udp_t *handle, const struct sockaddr *addr, unsigned int flags)
绑定UDP地址
- **handle**: UDP句柄
- **addr**: 地址结构
- **flags**: 标志位
  - UV_UDP_REUSEADDR：允许地址重用
  - 0：默认行为

### 5. uv_udp_recv_start(uv_udp_t *handle, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb)
开始接收UDP数据
- **alloc_cb**: 内存分配回调
  - 建议缓冲区大小：2048-8192字节
- **recv_cb**: 接收数据回调

### 6. uv_udp_send(uv_udp_send_t *req, uv_udp_t *handle, const uv_buf_t bufs[], unsigned int nbufs, const struct sockaddr *addr, uv_udp_send_cb send_cb)
发送UDP数据
- **req**: 发送请求对象
- **handle**: UDP句柄
- **bufs**: 数据缓冲区数组
- **nbufs**: 缓冲区数量
- **addr**: 目标地址
- **send_cb**: 发送完成回调

### 7. uv_timer_init(uv_loop_t *loop, uv_timer_t *handle)
初始化定时器
- **loop**: 事件循环
- **handle**: 定时器句柄

### 8. uv_timer_start(uv_timer_t *handle, uv_timer_cb cb, uint64_t timeout, uint64_t repeat)
启动定时器
- **timeout**: 第一次触发延迟，单位毫秒
- **repeat**: 重复间隔，单位毫秒
  - 0：仅触发一次
  - >0：周期性触发
  - 建议：KCP update使用10-20ms

## 框架配置建议

### 低延迟模式（适用于游戏、实时通信）
```cpp
ikcp_nodelay(kcp, 1, 10, 2, 1);
ikcp_wndsize(kcp, 128, 128);
ikcp_setmtu(kcp, 1200);
```

### 标准模式（适用于一般应用）
```cpp
ikcp_nodelay(kcp, 0, 40, 0, 0);
ikcp_wndsize(kcp, 64, 256);
ikcp_setmtu(kcp, 1400);
```

### 高吞吐模式（适用于文件传输）
```cpp
ikcp_nodelay(kcp, 0, 100, 0, 0);
ikcp_wndsize(kcp, 256, 1024);
ikcp_setmtu(kcp, 1400);
```

## 构建方法

```bash
mkdir build
cd build
cmake ..
make
```

## 运行示例

```bash
# 启动服务器
./kcp_server 8888

# 启动客户端
./kcp_client 127.0.0.1 8888
```

## 数据发送方式

框架提供两种数据发送方式，适用于不同的业务场景：

### 1. 可靠传输（send）
通过KCP协议发送数据，提供可靠性保证

```cpp
// 发送可靠数据
conn->send(data, len);
```

**特点：**
- ✓ 保证送达：丢包自动重传
- ✓ 保证顺序：按发送顺序接收
- ✓ 自动分片：支持任意大小数据
- ✓ 流量控制：自动调节发送速度
- ✓ 拥塞控制：网络拥塞时自动降速

**适用场景：**
- 重要的业务数据：玩家操作、交易信息等
- 大数据传输：文件、资源下载等
- 需要顺序保证的消息：聊天消息、任务更新等

### 2. 不可靠传输（send_udp_direct）
直接通过UDP发送，不经过KCP，无可靠性保证

```cpp
// 发送不可靠数据（心跳包、状态同步等）
conn->send_udp_direct(data, len);
```

**特点：**
- ✓ 超低延迟：无重传和确认机制
- ✓ 低带宽占用：不占用KCP发送队列
- ✓ 简单高效：直接UDP传输
- ✗ 不保证送达：数据可能丢失
- ✗ 不保证顺序：可能乱序到达
- ✗ 不自动分片：建议<1400字节

**适用场景：**
- 心跳包：定期发送保持连接，丢失无影响
- 实时状态同步：位置、姿态、血量等，最新数据优先
- 不重要的通知：可以容忍丢失的消息
- 频繁更新的数据：每秒发送多次的数据

**使用建议：**
```cpp
// 心跳包示例（每5秒发送）
const char* heartbeat = "PING";
conn->send_udp_direct(heartbeat, 4);

// 位置同步示例（每秒发送20次）
struct Position { float x, y, z; };
Position pos = {100.0f, 50.0f, 200.0f};
conn->send_udp_direct((char*)&pos, sizeof(pos));

// 重要数据必须用可靠传输
const char* important = "Player Attack";
conn->send(important, strlen(important));
```

## 注意事项

1. **KCP需要定期update**：必须在应用层定期调用ikcp_update或ikcp_check+ikcp_flush
2. **时间戳一致性**：所有KCP实例应使用相同的时间基准
3. **MTU设置**：需要考虑网络环境，避免IP分片
4. **窗口大小**：根据延迟和带宽调整，过大会占用更多内存
5. **连接超时**：需要在应用层实现心跳机制检测连接状态
6. **内存管理**：及时释放断开的连接资源
7. **线程安全**：KCP不是线程安全的，需要在应用层保护
8. **选择合适的发送方式**：重要数据用send，不重要数据用send_udp_direct

## 性能优化建议

1. **调整系统UDP缓冲区**：
   ```bash
   # Linux
   sysctl -w net.core.rmem_max=16777216
   sysctl -w net.core.wmem_max=16777216
   ```

2. **使用对象池**：复用连接对象，减少频繁的内存分配

3. **批量处理**：在一次事件循环中处理多个消息

4. **合理设置超时**：避免过多的无效连接占用资源

## 参考资料

- [KCP官方文档](https://github.com/skywind3000/kcp)
- [libuv官方文档](http://docs.libuv.org/)
