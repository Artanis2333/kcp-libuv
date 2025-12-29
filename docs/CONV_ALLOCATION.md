# KCP Conv（会话ID）分配策略

## Conv的作用

Conv（Conversation ID）是KCP协议中用于标识唯一连接的会话ID：
- KCP协议头中的第一个字段（4字节）
- 通信双方必须使用**相同的conv**
- 服务器通过conv来区分不同的客户端连接
- 范围：1 - 0xFFFFFFFF（0不能使用）

## 当前示例的问题

```cpp
// 示例代码中使用固定值
uint32_t conv = 1234;  // ❌ 生产环境不能这样用！
```

**问题：**
- 所有客户端使用相同的conv
- 服务器无法区分不同客户端
- 第二个客户端会覆盖第一个

## 生产环境的Conv分配策略

### 策略1：客户端生成（推荐）

**原理：** 客户端生成唯一的conv，服务器被动接受

#### 方法1.1：时间戳 + 随机数
```cpp
#include <chrono>
#include <random>

uint32_t generate_conv() {
    // 高16位：时间戳（秒级，约18小时循环）
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    uint32_t timestamp = (seconds & 0xFFFF) << 16;
    
    // 低16位：随机数
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFF);
    uint32_t random = dis(gen);
    
    uint32_t conv = timestamp | random;
    
    // 确保不为0
    return (conv == 0) ? 1 : conv;
}
```

**优点：**
- 实现简单
- 无需服务器分配
- 冲突概率极低

**缺点：**
- 理论上可能冲突
- 需要额外的冲突检测

#### 方法1.2：UUID/GUID转换
```cpp
#include <uuid/uuid.h>

uint32_t generate_conv_from_uuid() {
    uuid_t uuid;
    uuid_generate(uuid);
    
    // 取UUID的前4字节作为conv
    uint32_t conv = (uuid[0] << 24) | (uuid[1] << 16) | 
                    (uuid[2] << 8) | uuid[3];
    
    return (conv == 0) ? 1 : conv;
}
```

#### 方法1.3：MAC地址 + 进程ID + 序号
```cpp
uint32_t generate_conv() {
    static uint32_t sequence = 0;
    
    // MAC地址后3字节的哈希
    uint32_t mac_hash = get_mac_hash() & 0xFFFF;
    
    // 进程ID
    uint32_t pid = (getpid() & 0xFF) << 16;
    
    // 递增序号
    uint32_t seq = (++sequence) & 0xFF;
    
    return (mac_hash << 16) | (pid & 0xFF00) | seq;
}
```

### 策略2：服务器分配（更可靠）

**原理：** 客户端向服务器请求conv，服务器集中分配

#### 方法2.1：全局递增计数器
```cpp
class ConvAllocator {
private:
    std::atomic<uint32_t> next_conv_{1000};  // 从1000开始
    std::mutex mutex_;
    std::unordered_set<uint32_t> used_convs_;
    
public:
    uint32_t allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        uint32_t conv;
        do {
            conv = next_conv_++;
            // 防止溢出回到0
            if (conv == 0) {
                conv = 1;
                next_conv_ = 1;
            }
        } while (used_convs_.count(conv) > 0);
        
        used_convs_.insert(conv);
        return conv;
    }
    
    void release(uint32_t conv) {
        std::lock_guard<std::mutex> lock(mutex_);
        used_convs_.erase(conv);
    }
};
```

#### 方法2.2：分段分配
```cpp
class SegmentedConvAllocator {
private:
    // 将conv空间分成多个段，每个服务器实例使用不同段
    uint32_t server_id_;        // 服务器ID（0-255）
    uint32_t next_in_segment_;  // 段内序号
    
public:
    SegmentedConvAllocator(uint32_t server_id) 
        : server_id_(server_id & 0xFF), next_in_segment_(0) {}
    
    uint32_t allocate() {
        // 高8位：服务器ID
        // 低24位：段内递增序号
        uint32_t conv = (server_id_ << 24) | (next_in_segment_++ & 0xFFFFFF);
        return conv;
    }
};

// 使用：
// 服务器1: server_id=1, conv范围 0x01000000 - 0x01FFFFFF
// 服务器2: server_id=2, conv范围 0x02000000 - 0x02FFFFFF
```

#### 方法2.3：握手时分配
```cpp
// 客户端请求连接（使用临时conv）
struct ConnectRequest {
    uint32_t magic = 0x4B435031;  // "KCP1"
    uint32_t version = 1;
    uint32_t temp_conv = 0;        // 临时conv（0表示请求分配）
};

// 服务器回复分配的conv
struct ConnectResponse {
    uint32_t magic = 0x4B435032;  // "KCP2"
    uint32_t assigned_conv;        // 分配的conv
    uint32_t result;               // 0=成功，其他=错误码
};

// 客户端流程：
void connect_to_server() {
    // 1. 发送连接请求
    ConnectRequest req;
    req.temp_conv = rand();  // 临时conv用于识别响应
    send_udp(&req, sizeof(req));
    
    // 2. 接收服务器分配的conv
    ConnectResponse resp;
    recv_udp(&resp, sizeof(resp));
    
    // 3. 使用分配的conv创建KCP连接
    if (resp.result == 0) {
        uint32_t conv = resp.assigned_conv;
        create_kcp_connection(conv);
    }
}

// 服务器流程：
void on_connect_request(ConnectRequest* req, sockaddr* addr) {
    // 分配新的conv
    uint32_t conv = conv_allocator_.allocate();
    
    // 回复客户端
    ConnectResponse resp;
    resp.assigned_conv = conv;
    resp.result = 0;
    send_udp_to(addr, &resp, sizeof(resp));
    
    // 创建连接对象
    create_connection(conv, addr);
}
```

### 策略3：混合方式（最佳实践）

**客户端生成 + 服务器验证**

```cpp
// 客户端生成conv
uint32_t conv = generate_conv_client();

// 发送连接请求（包含客户端生成的conv）
struct HandshakePacket {
    uint32_t magic = 0x4B435033;
    uint32_t conv;          // 客户端建议的conv
    uint32_t token;         // 验证令牌
};

// 服务器验证并接受或重新分配
struct HandshakeResponse {
    uint32_t magic = 0x4B435034;
    uint32_t conv;          // 最终使用的conv（可能与请求不同）
    uint32_t result;        // 0=接受，1=conv冲突已重新分配
};

// 服务器端：
void on_handshake(HandshakePacket* pkt, sockaddr* addr) {
    uint32_t conv = pkt->conv;
    
    // 检查conv是否已被使用
    if (is_conv_in_use(conv)) {
        // 冲突，分配新的conv
        conv = conv_allocator_.allocate();
    }
    
    HandshakeResponse resp;
    resp.conv = conv;
    resp.result = (conv == pkt->conv) ? 0 : 1;
    send_udp_to(addr, &resp, sizeof(resp));
    
    create_connection(conv, addr);
}
```

## 实际应用场景

### 场景1：单服务器
**推荐：客户端生成（策略1.1）**
```cpp
// 客户端
uint32_t conv = generate_conv_timestamp_random();
client.connect(server_ip, server_port, conv);

// 服务器：被动接受，冲突时踢掉旧连接
auto old_conn = find_connection(conv);
if (old_conn) {
    old_conn->close();  // 踢掉旧连接
}
create_connection(conv, addr);
```

### 场景2：多服务器集群
**推荐：服务器分配 + 分段（策略2.2）**
```cpp
// 每个服务器使用不同的server_id
// 服务器1
SegmentedConvAllocator allocator1(1);  // conv: 0x01000000 - 0x01FFFFFF

// 服务器2
SegmentedConvAllocator allocator2(2);  // conv: 0x02000000 - 0x02FFFFFF

// 负载均衡器可以根据conv高8位路由到对应服务器
```

### 场景3：P2P通信
**推荐：双方协商（策略3）**
```cpp
// A和B要建立P2P连接
// 1. A生成conv_a，发给B
// 2. B生成conv_b，发给A
// 3. 使用conv = conv_a ^ conv_b作为最终conv

uint32_t conv_a = generate_conv();
uint32_t conv_b = receive_peer_conv();
uint32_t final_conv = conv_a ^ conv_b;
```

### 场景4：断线重连
**需要：持久化conv**
```cpp
class PersistentConnection {
private:
    uint32_t conv_;
    std::string token_;  // 重连令牌
    
public:
    // 首次连接：服务器分配conv和token
    void first_connect() {
        HandshakeRequest req;
        send_to_server(&req);
        
        HandshakeResponse resp = recv_from_server();
        conv_ = resp.conv;
        token_ = resp.token;
        
        // 保存到本地
        save_to_file(conv_, token_);
    }
    
    // 重连：使用保存的conv和token
    void reconnect() {
        load_from_file(&conv_, &token_);
        
        ReconnectRequest req;
        req.conv = conv_;
        req.token = token_;
        send_to_server(&req);
    }
};
```

## Conv冲突处理

### 检测冲突
```cpp
bool is_conv_conflict(uint32_t conv, const sockaddr* addr) {
    auto conn = find_connection(conv);
    if (!conn) {
        return false;  // 没有冲突
    }
    
    // 检查地址是否相同
    if (is_same_address(conn->get_addr(), addr)) {
        return false;  // 同一客户端，不算冲突
    }
    
    return true;  // 不同客户端使用了相同conv，有冲突
}
```

### 处理策略
```cpp
void handle_conv_conflict(uint32_t conv, const sockaddr* addr) {
    auto old_conn = find_connection(conv);
    
    // 策略A：踢掉旧连接（适合快速响应）
    if (old_conn->is_idle() || old_conn->is_timeout()) {
        old_conn->close();
        create_new_connection(conv, addr);
    }
    
    // 策略B：拒绝新连接（保护现有连接）
    else {
        send_error_to(addr, "Conv already in use");
    }
    
    // 策略C：分配新conv（最安全）
    uint32_t new_conv = allocate_new_conv();
    send_new_conv_to(addr, new_conv);
}
```

## 最佳实践总结

### 推荐方案

| 场景 | 推荐策略 | 原因 |
|------|---------|------|
| 内网/Demo | 客户端生成（时间戳+随机） | 简单，冲突概率低 |
| 生产环境 | 服务器分配（握手时分配） | 可靠，完全避免冲突 |
| 多服务器 | 分段分配 | 便于负载均衡和路由 |
| P2P | 双方协商 | 对等通信 |
| 断线重连 | 持久化conv | 保持会话一致性 |

### 代码模板
```cpp
// 在KCPServer中添加conv分配器
class KCPServer {
private:
    ConvAllocator conv_allocator_;  // conv分配器
    
public:
    // 修改find_or_create_connection
    KCPConnection* find_or_create_connection(
        const char* data, int len, const struct sockaddr* addr) {
        
        // 检查是否是握手包
        if (is_handshake_packet(data, len)) {
            // 分配新的conv
            uint32_t conv = conv_allocator_.allocate();
            auto conn = create_connection(conv, addr);
            send_handshake_response(addr, conv);
            return conn;
        }
        
        // 普通数据包，从包头提取conv
        uint32_t conv = *(uint32_t*)data;
        return find_connection(conv);
    }
};
```

## 总结

1. **当前示例使用固定conv仅用于演示，生产环境必须改进**
2. **最安全的方式：服务器集中分配conv**
3. **最简单的方式：客户端生成（时间戳+随机数）**
4. **必须实现冲突检测和处理机制**
5. **建议在握手阶段协商conv**

Conv的分配策略直接影响系统的可靠性和安全性，需要根据具体场景选择合适的方案！
