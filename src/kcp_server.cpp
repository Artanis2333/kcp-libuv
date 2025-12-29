#include "kcp_server.h"
#include <chrono>
#include <cstring>
#include <iostream>

/**
 * 构造函数实现
 */
KCPServer::KCPServer(uv_loop_t *loop)
    : loop_(loop), running_(false), next_conv_(1000), timeout_(30000),
      kcp_nodelay_(1), kcp_interval_(10), kcp_resend_(2), kcp_nc_(1),
      kcp_sndwnd_(128), kcp_rcvwnd_(128), kcp_mtu_(1400) {
  // 初始化UDP句柄
  // loop: 事件循环
  // &udp_handle_: UDP句柄指针
  uv_udp_init(loop_, &udp_handle_);

  // 设置UDP句柄的data字段为this指针
  // 在回调函数中可以通过handle->data获取KCPServer对象
  udp_handle_.data = this;

  // 初始化定时器
  // 用于定期调用KCP的update函数
  uv_timer_init(loop_, &timer_);
  timer_.data = this;

  std::cout << "[KCPServer] 服务器已创建" << std::endl;
}

/**
 * 析构函数实现
 */
KCPServer::~KCPServer() {
  stop();

  // 关闭所有连接
  connections_.clear();

  std::cout << "[KCPServer] 服务器已销毁" << std::endl;
}

/**
 * 绑定并启动服务器
 */
int KCPServer::bind_and_listen(const std::string &ip, int port) {
  struct sockaddr_in addr;

  // 创建IPv4地址结构
  // ip.c_str(): IP地址字符串
  // port: 端口号
  // &addr: 输出的地址结构
  // 返回值：0表示成功，<0表示失败
  int ret = uv_ip4_addr(ip.c_str(), port, &addr);
  if (ret < 0) {
    std::cerr << "[KCPServer] 无效的IP地址: " << uv_strerror(ret) << std::endl;
    return ret;
  }

  // 绑定UDP地址
  // &udp_handle_: UDP句柄
  // (const struct sockaddr*)&addr: 地址结构
  // 0: 标志位（0表示默认行为，UV_UDP_REUSEADDR表示允许地址重用）
  ret = uv_udp_bind(&udp_handle_, (const struct sockaddr *)&addr, 0);
  if (ret < 0) {
    std::cerr << "[KCPServer] 绑定失败: " << uv_strerror(ret) << std::endl;
    return ret;
  }

  // 开始接收UDP数据
  // &udp_handle_: UDP句柄
  // alloc_buffer: 内存分配回调（libuv需要缓冲区时调用）
  // on_udp_recv: 数据接收回调（接收到数据时调用）
  ret = uv_udp_recv_start(&udp_handle_, alloc_buffer, on_udp_recv);
  if (ret < 0) {
    std::cerr << "[KCPServer] 启动接收失败: " << uv_strerror(ret) << std::endl;
    return ret;
  }

  // 启动定时器
  // &timer_: 定时器句柄
  // on_timer: 定时器回调函数
  // 0: 第一次触发延迟（0表示立即触发）
  // 10: 重复间隔（10ms，与KCP的interval保持一致或更小）
  // 注意：定时器间隔应该小于等于KCP的interval参数
  ret = uv_timer_start(&timer_, on_timer, 0, 10);
  if (ret < 0) {
    std::cerr << "[KCPServer] 启动定时器失败: " << uv_strerror(ret)
              << std::endl;
    return ret;
  }

  running_ = true;
  std::cout << "[KCPServer] 服务器已启动，监听 " << ip << ":" << port
            << std::endl;
  return 0;
}

/**
 * 运行事件循环
 */
void KCPServer::run() {
  if (!running_) {
    std::cerr << "[KCPServer] 服务器未启动" << std::endl;
    return;
  }

  std::cout << "[KCPServer] 事件循环开始运行" << std::endl;

  // 运行事件循环
  // UV_RUN_DEFAULT: 默认模式，会一直运行直到没有活跃的句柄和请求
  // 这是一个阻塞调用，会在这里一直执行，处理所有的IO事件
  uv_run(loop_, UV_RUN_DEFAULT);

  std::cout << "[KCPServer] 事件循环已退出" << std::endl;
}

/**
 * 停止事件循环
 */
void KCPServer::stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  // 停止定时器
  uv_timer_stop(&timer_);

  // 停止UDP接收
  uv_udp_recv_stop(&udp_handle_);

  // 停止事件循环
  // 会使uv_run返回
  uv_stop(loop_);

  std::cout << "[KCPServer] 服务器已停止" << std::endl;
}

/**
 * 设置KCP配置参数
 */
void KCPServer::set_kcp_config(int nodelay, int interval, int resend, int nc,
                               int sndwnd, int rcvwnd, int mtu) {
  kcp_nodelay_ = nodelay;
  kcp_interval_ = interval;
  kcp_resend_ = resend;
  kcp_nc_ = nc;
  kcp_sndwnd_ = sndwnd;
  kcp_rcvwnd_ = rcvwnd;
  kcp_mtu_ = mtu;

  std::cout << "[KCPServer] KCP配置已更新: "
            << "nodelay=" << nodelay << ", interval=" << interval
            << ", resend=" << resend << ", nc=" << nc << ", sndwnd=" << sndwnd
            << ", rcvwnd=" << rcvwnd << ", mtu=" << mtu << std::endl;
}

/**
 * 获取当前时间戳（毫秒）
 */
uint32_t KCPServer::get_current_ms() {
  // 使用C++11的chrono库获取单调时钟时间
  // steady_clock: 单调时钟，不受系统时间调整影响
  // time_since_epoch(): 从纪元（时钟的起点）到现在的时长
  // count(): 获取时长的数值
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  return static_cast<uint32_t>(ms.count());
}

/**
 * 内存分配回调函数实现
 */
void KCPServer::alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                             uv_buf_t *buf) {
  // 从handle->data获取KCPServer对象
  KCPServer *server = (KCPServer *)handle->data;

  // 使用服务器的接收缓冲区
  // uv_buf_init: 初始化libuv缓冲区结构
  // server->recv_buffer_: 缓冲区指针
  // sizeof(server->recv_buffer_): 缓冲区大小（64KB）
  // 注意：这里使用静态缓冲区，避免频繁分配内存
  *buf = uv_buf_init(server->recv_buffer_, sizeof(server->recv_buffer_));
}

/**
 * UDP接收回调函数实现
 */
void KCPServer::on_udp_recv(uv_udp_t *handle, ssize_t nread,
                            const uv_buf_t *buf, const struct sockaddr *addr,
                            unsigned flags) {
  // 从handle->data获取KCPServer对象
  KCPServer *server = (KCPServer *)handle->data;

  // nread < 0 表示接收错误
  if (nread < 0) {
    std::cerr << "[KCPServer] UDP接收错误: " << uv_strerror(nread) << std::endl;
    return;
  }

  // nread == 0 表示没有数据
  if (nread == 0) {
    return;
  }

  // addr == nullptr 表示没有发送方地址
  if (!addr) {
    return;
  }

  // flags & UV_UDP_PARTIAL 表示数据被截断（缓冲区太小）
  if (flags & UV_UDP_PARTIAL) {
    std::cerr << "[KCPServer] UDP数据被截断" << std::endl;
    return;
  }

  // 处理接收到的数据
  server->handle_udp_data(buf->base, nread, addr);
}

/**
 * 定时器回调函数实现
 */
void KCPServer::on_timer(uv_timer_t *handle) {
  // 从handle->data获取KCPServer对象
  KCPServer *server = (KCPServer *)handle->data;

  // 更新所有连接
  server->update_connections();
}

/**
 * 处理UDP数据
 */
void KCPServer::handle_udp_data(const char *data, int len,
                                const struct sockaddr *addr) {
  // KCP数据包至少需要24字节（KCP协议头）
  if (len < 24) {
    return;
  }

  // 解析KCP协议头获取conv
  // KCP协议头格式：conv(4字节) + cmd(1字节) + ...
  // 使用小端字节序
  uint32_t conv = *(uint32_t *)data;

  // 查找或创建连接
  KCPConnection *conn = find_or_create_connection(conv, addr);
  if (!conn) {
    return;
  }

  // 更新连接的活跃时间
  conn->update_active_time(get_current_ms());

  // 将数据输入到KCP
  conn->input(data, len);

  // 尝试接收数据
  conn->recv();
}

/**
 * 查找或创建连接
 */
KCPConnection *
KCPServer::find_or_create_connection(uint32_t conv,
                                     const struct sockaddr *addr) {
  // 查找现有连接
  auto it = connections_.find(conv);
  if (it != connections_.end()) {
    return it->second.get();
  }

  // 创建新连接
  std::cout << "[KCPServer] 创建新连接，conv=" << conv << std::endl;

  // 使用智能指针管理连接对象
  auto conn = std::make_shared<KCPConnection>(conv, &udp_handle_, addr);

  // 初始化KCP参数
  conn->init_kcp(kcp_nodelay_, kcp_interval_, kcp_resend_, kcp_nc_, kcp_sndwnd_,
                 kcp_rcvwnd_, kcp_mtu_);

  // 设置连接为已连接状态
  conn->set_state(KCPConnection::CONNECTED);

  // 更新活跃时间
  conn->update_active_time(get_current_ms());

  // 设置回调函数
  // 使用lambda捕获this指针，在回调中调用成员函数
  conn->set_data_callback([this](KCPConnection *c, const char *data, int len) {
    this->on_connection_data(c, data, len);
  });

  conn->set_close_callback(
      [this](KCPConnection *c) { this->on_connection_close(c); });

  // 添加到连接映射
  connections_[conv] = conn;

  // 调用新连接回调
  if (new_connection_callback_) {
    new_connection_callback_(conn.get());
  }

  return conn.get();
}

/**
 * 移除连接
 */
void KCPServer::remove_connection(uint32_t conv) {
  auto it = connections_.find(conv);
  if (it != connections_.end()) {
    std::cout << "[KCPServer] 移除连接，conv=" << conv << std::endl;
    connections_.erase(it);
  }
}

/**
 * 更新所有连接
 */
void KCPServer::update_connections() {
  uint32_t current = get_current_ms();

  // 遍历所有连接
  // 注意：使用迭代器遍历，因为可能会删除连接
  for (auto it = connections_.begin(); it != connections_.end();) {
    auto &conn = it->second;

    // 检查连接是否超时
    if (conn->is_timeout(current, timeout_)) {
      std::cout << "[KCPServer] 连接超时，conv=" << conn->get_conv()
                << std::endl;
      conn->close();
      it = connections_.erase(it);
      continue;
    }

    // 更新KCP状态
    conn->update(current);

    // 尝试接收数据
    conn->recv();

    ++it;
  }
}

/**
 * 连接数据接收回调
 */
void KCPServer::on_connection_data(KCPConnection *conn, const char *data,
                                   int len) {
  std::cout << "[KCPServer] 收到数据，conv=" << conn->get_conv()
            << ", len=" << len << ", data=" << std::string(data, len)
            << std::endl;

  // 这里可以处理接收到的数据
  // 示例：回显数据
  conn->send(data, len);
}

/**
 * 连接关闭回调
 */
void KCPServer::on_connection_close(KCPConnection *conn) {
  std::cout << "[KCPServer] 连接关闭，conv=" << conn->get_conv() << std::endl;
  remove_connection(conn->get_conv());
}
