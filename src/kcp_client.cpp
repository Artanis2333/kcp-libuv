#include "kcp_client.h"
#include <chrono>
#include <cstring>
#include <iostream>

/**
 * 构造函数实现
 */
KCPClient::KCPClient(uv_loop_t *loop)
    : loop_(loop), running_(false), kcp_nodelay_(1), kcp_interval_(10),
      kcp_resend_(2), kcp_nc_(1), kcp_sndwnd_(128), kcp_rcvwnd_(128),
      kcp_mtu_(1400) {
  // 初始化UDP句柄
  uv_udp_init(loop_, &udp_handle_);
  udp_handle_.data = this;

  // 初始化定时器
  uv_timer_init(loop_, &timer_);
  timer_.data = this;

  std::cout << "[KCPClient] 客户端已创建" << std::endl;
}

/**
 * 析构函数实现
 */
KCPClient::~KCPClient() {
  disconnect();
  std::cout << "[KCPClient] 客户端已销毁" << std::endl;
}

/**
 * 连接到服务器
 */
int KCPClient::connect(const std::string &server_ip, int server_port,
                       uint32_t conv) {
  if (connection_) {
    std::cerr << "[KCPClient] 已经存在连接" << std::endl;
    return -1;
  }

  struct sockaddr_in server_addr;

  // 创建服务器地址结构
  int ret = uv_ip4_addr(server_ip.c_str(), server_port, &server_addr);
  if (ret < 0) {
    std::cerr << "[KCPClient] 无效的服务器地址: " << uv_strerror(ret)
              << std::endl;
    return ret;
  }

  // 绑定本地地址（使用0.0.0.0:0表示自动分配）
  struct sockaddr_in local_addr;
  uv_ip4_addr("0.0.0.0", 0, &local_addr);
  ret = uv_udp_bind(&udp_handle_, (const struct sockaddr *)&local_addr, 0);
  if (ret < 0) {
    std::cerr << "[KCPClient] 绑定本地地址失败: " << uv_strerror(ret)
              << std::endl;
    return ret;
  }

  // 开始接收UDP数据
  ret = uv_udp_recv_start(&udp_handle_, alloc_buffer, on_udp_recv);
  if (ret < 0) {
    std::cerr << "[KCPClient] 启动接收失败: " << uv_strerror(ret) << std::endl;
    return ret;
  }

  // 创建KCP连接
  connection_ = std::make_shared<KCPConnection>(
      conv, &udp_handle_, (const struct sockaddr *)&server_addr);

  // 初始化KCP参数
  connection_->init_kcp(kcp_nodelay_, kcp_interval_, kcp_resend_, kcp_nc_,
                        kcp_sndwnd_, kcp_rcvwnd_, kcp_mtu_);

  // 设置连接状态为已连接
  connection_->set_state(KCPConnection::CONNECTED);

  // 更新活跃时间
  connection_->update_active_time(get_current_ms());

  // 启动定时器
  ret = uv_timer_start(&timer_, on_timer, 0, 10);
  if (ret < 0) {
    std::cerr << "[KCPClient] 启动定时器失败: " << uv_strerror(ret)
              << std::endl;
    return ret;
  }

  running_ = true;
  std::cout << "[KCPClient] 已连接到服务器 " << server_ip << ":" << server_port
            << ", conv=" << conv << std::endl;
  return 0;
}

/**
 * 发送数据
 */
int KCPClient::send(const char *data, int len) {
  if (!connection_) {
    std::cerr << "[KCPClient] 未连接到服务器" << std::endl;
    return -1;
  }

  return connection_->send(data, len);
}

/**
 * 断开连接
 */
void KCPClient::disconnect() {
  if (!connection_) {
    return;
  }

  std::cout << "[KCPClient] 断开连接" << std::endl;

  running_ = false;

  // 停止定时器
  uv_timer_stop(&timer_);

  // 停止UDP接收
  uv_udp_recv_stop(&udp_handle_);

  // 关闭连接
  connection_->close();
  connection_.reset();
}

/**
 * 运行事件循环
 */
void KCPClient::run() {
  if (!running_) {
    std::cerr << "[KCPClient] 客户端未连接" << std::endl;
    return;
  }

  std::cout << "[KCPClient] 事件循环开始运行" << std::endl;
  uv_run(loop_, UV_RUN_DEFAULT);
  std::cout << "[KCPClient] 事件循环已退出" << std::endl;
}

/**
 * 停止事件循环
 */
void KCPClient::stop() {
  if (!running_) {
    return;
  }

  disconnect();
  uv_stop(loop_);
  std::cout << "[KCPClient] 客户端已停止" << std::endl;
}

/**
 * 设置数据接收回调函数
 */
void KCPClient::set_data_callback(KCPConnection::DataCallback cb) {
  if (connection_) {
    connection_->set_data_callback(cb);
  }
}

/**
 * 设置连接关闭回调函数
 */
void KCPClient::set_close_callback(KCPConnection::CloseCallback cb) {
  if (connection_) {
    connection_->set_close_callback(cb);
  }
}

/**
 * 设置KCP配置
 */
void KCPClient::set_kcp_config(int nodelay, int interval, int resend, int nc,
                               int sndwnd, int rcvwnd, int mtu) {
  kcp_nodelay_ = nodelay;
  kcp_interval_ = interval;
  kcp_resend_ = resend;
  kcp_nc_ = nc;
  kcp_sndwnd_ = sndwnd;
  kcp_rcvwnd_ = rcvwnd;
  kcp_mtu_ = mtu;

  std::cout << "[KCPClient] KCP配置已更新: "
            << "nodelay=" << nodelay << ", interval=" << interval
            << ", resend=" << resend << ", nc=" << nc << ", sndwnd=" << sndwnd
            << ", rcvwnd=" << rcvwnd << ", mtu=" << mtu << std::endl;
}

/**
 * 检查是否已连接
 */
bool KCPClient::is_connected() const {
  return connection_ && connection_->get_state() == KCPConnection::CONNECTED;
}

/**
 * 获取当前时间戳
 */
uint32_t KCPClient::get_current_ms() {
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  return static_cast<uint32_t>(ms.count());
}

/**
 * 内存分配回调函数
 */
void KCPClient::alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                             uv_buf_t *buf) {
  KCPClient *client = (KCPClient *)handle->data;
  *buf = uv_buf_init(client->recv_buffer_, sizeof(client->recv_buffer_));
}

/**
 * UDP接收回调函数
 */
void KCPClient::on_udp_recv(uv_udp_t *handle, ssize_t nread,
                            const uv_buf_t *buf, const struct sockaddr *addr,
                            unsigned flags) {
  KCPClient *client = (KCPClient *)handle->data;

  if (nread < 0) {
    std::cerr << "[KCPClient] UDP接收错误: " << uv_strerror(nread) << std::endl;
    return;
  }

  if (nread == 0 || !addr) {
    return;
  }

  if (flags & UV_UDP_PARTIAL) {
    std::cerr << "[KCPClient] UDP数据被截断" << std::endl;
    return;
  }

  client->handle_udp_data(buf->base, nread);
}

/**
 * 定时器回调函数
 */
void KCPClient::on_timer(uv_timer_t *handle) {
  KCPClient *client = (KCPClient *)handle->data;
  client->update_connection();
}

/**
 * 处理UDP数据
 */
void KCPClient::handle_udp_data(const char *data, int len) {
  if (!connection_) {
    return;
  }

  // 更新活跃时间
  connection_->update_active_time(get_current_ms());

  // 将数据输入到KCP
  connection_->input(data, len);

  // 尝试接收数据
  connection_->recv();
}

/**
 * 更新连接
 */
void KCPClient::update_connection() {
  if (!connection_) {
    return;
  }

  uint32_t current = get_current_ms();

  // 更新KCP状态
  connection_->update(current);

  // 尝试接收数据
  connection_->recv();
}
