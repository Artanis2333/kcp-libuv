#include "kcp_connection.h"
#include <cstring>
#include <iostream>

/**
 * 构造函数实现
 * 初始化KCP连接对象，创建KCP控制块
 */
KCPConnection::KCPConnection(uint32_t conv, uv_udp_t *udp_handle,
                             const struct sockaddr *addr)
    : conv_(conv), udp_handle_(udp_handle), state_(CONNECTING),
      last_active_time_(0) {
  // 复制对端地址
  memcpy(&addr_, addr, sizeof(struct sockaddr_storage));

  // 创建KCP控制块
  // conv: 会话ID，必须在通信双方保持一致
  // this: 用户数据指针，在回调函数中可以获取到KCPConnection对象
  kcp_ = ikcp_create(conv, (void *)this);

  // 设置KCP的输出回调函数
  // 当KCP需要发送数据时，会调用这个函数
  kcp_->output = udp_output;

  std::cout << "[KCPConnection] 创建连接，conv=" << conv << std::endl;
}

/**
 * 析构函数实现
 * 释放KCP控制块资源
 */
KCPConnection::~KCPConnection() {
  if (kcp_) {
    // 释放KCP控制块
    // 会释放KCP内部分配的所有资源（发送队列、接收队列等）
    ikcp_release(kcp_);
    kcp_ = nullptr;
  }
  std::cout << "[KCPConnection] 销毁连接，conv=" << conv_ << std::endl;
}

/**
 * 初始化KCP参数
 * 设置KCP的工作模式和性能参数
 */
void KCPConnection::init_kcp(int nodelay, int interval, int resend, int nc,
                             int sndwnd, int rcvwnd, int mtu) {
  // 设置nodelay模式
  // nodelay=1, interval=10, resend=2, nc=1 是低延迟配置
  // nodelay=0, interval=40, resend=0, nc=0 是标准配置
  ikcp_nodelay(kcp_, nodelay, interval, resend, nc);

  // 设置发送和接收窗口大小
  // 窗口越大，可以同时发送/接收的数据包越多，吞吐量越高
  // 但也会占用更多内存
  ikcp_wndsize(kcp_, sndwnd, rcvwnd);

  // 设置MTU（最大传输单元）和MSS（最大分片大小）
  // MTU: UDP包的最大大小
  // MSS: 自动计算为 MTU - 24 (KCP协议头)
  ikcp_setmtu(kcp_, mtu);

  std::cout << "[KCPConnection] 初始化KCP参数: "
            << "nodelay=" << nodelay << ", interval=" << interval
            << ", resend=" << resend << ", nc=" << nc << ", sndwnd=" << sndwnd
            << ", rcvwnd=" << rcvwnd << ", mtu=" << mtu << std::endl;
}

/**
 * 设置最小RTO时间
 */
void KCPConnection::set_minrto(int minrto) {
  if (!kcp_) {
    return;
  }

  // 直接设置KCP结构体的rx_minrto字段
  // rx_minrto: 最小重传超时时间（单位毫秒）
  // 这个值决定了RTO的下限，避免RTO过小导致不必要的重传
  kcp_->rx_minrto = minrto;

  std::cout << "[KCPConnection] 设置最小RTO: " << minrto << "ms" << std::endl;
}

/**
 * 设置快速重传触发次数
 */
void KCPConnection::set_fastresend(int fastresend) {
  if (!kcp_) {
    return;
  }

  // 设置快速重传参数
  // fastresend: 触发快速重传的ACK跨越次数
  // 0：关闭快速重传
  // >0：收到fastresend个后续ACK就触发快速重传
  // 例如：fastresend=2，收到ACK 5,7,8时会重传包6
  kcp_->fastresend = fastresend;

  std::cout << "[KCPConnection] 设置快速重传触发次数: " << fastresend
            << std::endl;
}

/**
 * 设置流模式
 */
void KCPConnection::set_stream_mode(int stream) {
  if (!kcp_) {
    return;
  }

  // 设置流模式
  // stream=0: 消息模式（默认），每次recv接收完整的消息
  //           发送时会保留消息边界，接收时也会按消息边界接收
  // stream=1: 流模式，类似TCP，数据连续无边界
  //           数据会被连续发送和接收，没有消息边界的概念
  kcp_->stream = stream;

  std::cout << "[KCPConnection] 设置流模式: "
            << (stream ? "流模式" : "消息模式") << std::endl;
}

/**
 * 设置最大重传次数（死链接检测）
 */
void KCPConnection::set_dead_link(int dead_link) {
  if (!kcp_) {
    return;
  }

  // 设置死链接检测参数
  // dead_link: 最大重传次数
  // 当一个数据包的重传次数达到这个值时，认为连接已断开
  // 默认值是20次
  // 设置过小会导致在网络抖动时误判连接断开
  // 设置过大会延迟连接断开的检测时间
  kcp_->dead_link = dead_link;

  std::cout << "[KCPConnection] 设置最大重传次数: " << dead_link << std::endl;
}

/**
 * 获取等待发送的包数量
 */
int KCPConnection::get_waitsnd() const {
  if (!kcp_) {
    return 0;
  }

  // 调用ikcp_waitsnd获取等待发送的包数量
  // 返回值：发送队列和发送缓冲区中的包总数
  // 可以用于：
  // 1. 流量控制：当waitsnd过大时暂停发送
  // 2. 拥塞检测：判断网络是否拥塞
  // 3. 队列管理：避免内存占用过大
  return ikcp_waitsnd(kcp_);
}

/**
 * 发送数据（可靠传输，通过KCP）
 * 将数据发送到KCP发送队列
 */
int KCPConnection::send(const char *data, int len) {
  if (!kcp_ || state_ != CONNECTED) {
    return -1;
  }

  // 调用ikcp_send将数据加入发送队列
  // KCP会自动进行分片、编号、加入发送队列
  // 返回值：0表示成功，<0表示失败（如发送队列满）
  int ret = ikcp_send(kcp_, data, len);
  if (ret < 0) {
    std::cerr << "[KCPConnection] 发送失败，conv=" << conv_ << ", ret=" << ret
              << std::endl;
    return ret;
  }

  std::cout << "[KCPConnection] 发送数据（KCP可靠），conv=" << conv_
            << ", len=" << len << std::endl;
  return 0;
}

/**
 * 直接发送UDP数据（不可靠传输）
 * 绕过KCP，直接通过UDP发送数据
 */
int KCPConnection::send_udp_direct(const char *data, int len) {
  if (!udp_handle_ || state_ != CONNECTED) {
    return -1;
  }

  // 检查数据长度，避免IP分片
  if (len > 1472) {
    std::cerr << "[KCPConnection] UDP直接发送数据过大，len=" << len
              << " (建议<1472字节)" << std::endl;
    // 不阻止发送，但给出警告
  }

  // 直接调用output函数发送UDP数据
  // 注意：这里不经过KCP，没有可靠性保证
  // - 数据可能丢失
  // - 数据可能乱序
  // - 没有重传机制
  // - 没有流量控制
  int ret = output(data, len);
  if (ret < 0) {
    std::cerr << "[KCPConnection] UDP直接发送失败，conv=" << conv_
              << ", ret=" << ret << std::endl;
    return ret;
  }

  std::cout << "[KCPConnection] UDP直接发送（不可靠），conv=" << conv_
            << ", len=" << len << std::endl;
  return 0;
}

/**
 * 输入UDP数据到KCP
 * 将从UDP接收到的数据输入到KCP协议栈处理
 */
int KCPConnection::input(const char *data, int len) {
  if (!kcp_) {
    return -1;
  }

  // 调用ikcp_input将UDP数据输入到KCP
  // KCP会解析协议头，处理ACK、重传等逻辑
  // 将数据包加入接收队列或处理确认信息
  // 返回值：0表示成功，<0表示数据格式错误
  int ret = ikcp_input(kcp_, data, len);
  if (ret < 0) {
    std::cerr << "[KCPConnection] 输入数据失败，conv=" << conv_
              << ", ret=" << ret << std::endl;
    return ret;
  }

  return 0;
}

/**
 * 更新KCP状态
 * 必须定期调用此函数来驱动KCP协议运行
 */
void KCPConnection::update(uint32_t current) {
  if (!kcp_) {
    return;
  }

  // 调用ikcp_update更新KCP状态
  // current: 当前时间戳（毫秒）
  // KCP会根据时间戳处理：
  // 1. 超时重传：检查是否有包超时需要重传
  // 2. 快速重传：根据配置进行快速重传
  // 3. 拥塞控制：更新拥塞窗口
  // 4. 发送数据：将发送队列中的数据发送出去
  ikcp_update(kcp_, current);
}

/**
 * 检查下次需要update的时间
 * 可以用于优化，避免过于频繁的update调用
 */
uint32_t KCPConnection::check(uint32_t current) {
  if (!kcp_) {
    return current;
  }

  // 调用ikcp_check获取下次需要update的时间戳
  // 返回值是下次应该调用ikcp_update的时间（毫秒）
  // 可以据此设置定时器，在合适的时间调用update
  return ikcp_check(kcp_, current);
}

/**
 * 接收数据
 * 从KCP接收队列读取数据
 */
bool KCPConnection::recv() {
  if (!kcp_) {
    return false;
  }

  bool has_data = false;

  // 循环接收，直到接收队列为空
  while (true) {
    // 调用ikcp_recv从接收队列读取数据
    // recv_buffer_: 接收缓冲区
    // sizeof(recv_buffer_): 缓冲区大小
    // 返回值：>0表示接收到的字节数，<0表示没有数据或错误
    int len = ikcp_recv(kcp_, recv_buffer_, sizeof(recv_buffer_));
    if (len < 0) {
      // 没有更多数据
      break;
    }

    has_data = true;

    // 调用数据接收回调函数
    if (data_callback_) {
      data_callback_(this, recv_buffer_, len);
    }

    std::cout << "[KCPConnection] 接收数据，conv=" << conv_ << ", len=" << len
              << std::endl;
  }

  return has_data;
}

/**
 * 检查连接是否超时
 */
bool KCPConnection::is_timeout(uint32_t current, uint32_t timeout) const {
  // 如果当前时间 - 最后活跃时间 > 超时时长，则认为连接超时
  // 注意：需要考虑时间戳溢出的情况
  return (current - last_active_time_) > timeout;
}

/**
 * 关闭连接
 * 状态转换：CONNECTED -> DISCONNECTING -> DISCONNECTED
 */
void KCPConnection::close() {
  if (state_ == DISCONNECTED) {
    std::cout << "[KCPConnection] 连接已经关闭，conv=" << conv_ << std::endl;
    return;
  }

  std::cout << "[KCPConnection] 开始关闭连接，conv=" << conv_
            << ", 当前状态=" << state_ << std::endl;

  // 如果还在连接中或已连接，先进入DISCONNECTING状态
  if (state_ == CONNECTING || state_ == CONNECTED) {
    state_ = DISCONNECTING;
    std::cout << "[KCPConnection] 状态转换: -> DISCONNECTING, conv=" << conv_
              << std::endl;

    // 检查发送队列
    int waitsnd = get_waitsnd();
    if (waitsnd > 0) {
      std::cout << "[KCPConnection] 等待发送队列清空，waitsnd=" << waitsnd
                << ", conv=" << conv_ << std::endl;
      // 注意：实际应用中应该在update中检查队列是否清空
      // 这里演示状态转换逻辑
      // 在update_connections中应该检查DISCONNECTING状态的连接
      // 当waitsnd==0时再转为DISCONNECTED
    } else {
      std::cout << "[KCPConnection] 发送队列已空，立即断开, conv=" << conv_
                << std::endl;
    }
  }

  // 转为DISCONNECTED状态
  state_ = DISCONNECTED;
  std::cout << "[KCPConnection] 状态转换: -> DISCONNECTED, conv=" << conv_
            << std::endl;

  // 调用关闭回调
  if (close_callback_) {
    close_callback_(this);
  }

  std::cout << "[KCPConnection] 连接已关闭，conv=" << conv_ << std::endl;
}

/**
 * KCP输出回调函数（静态）
 * KCP需要发送数据时会调用此函数
 */
int KCPConnection::udp_output(const char *buf, int len, ikcpcb *kcp,
                              void *user) {
  // user参数是在ikcp_create时传入的用户数据指针
  // 这里就是KCPConnection对象指针
  KCPConnection *conn = (KCPConnection *)user;
  if (!conn) {
    return -1;
  }

  // 调用实际的输出函数
  return conn->output(buf, len);
}

/**
 * 实际的UDP数据发送函数
 * 通过libuv的UDP接口发送数据
 */
int KCPConnection::output(const char *buf, int len) {
  if (!udp_handle_) {
    return -1;
  }

  // 分配发送请求对象
  // uv_udp_send_t是libuv的异步发送请求结构
  uv_udp_send_t *send_req = new uv_udp_send_t;

  // 创建缓冲区
  // uv_buf_t是libuv的缓冲区结构，包含base指针和len长度
  // 注意：这里需要复制数据，因为异步发送时原始缓冲区可能已经被释放
  char *send_buf = new char[len];
  memcpy(send_buf, buf, len);
  uv_buf_t buffer = uv_buf_init(send_buf, len);

  // 设置请求的data字段，用于在回调中释放资源
  send_req->data = send_buf;

  // 调用uv_udp_send发送UDP数据
  // send_req: 发送请求对象
  // udp_handle_: UDP句柄
  // &buffer: 缓冲区数组（这里只有一个缓冲区）
  // 1: 缓冲区数量
  // get_addr(): 目标地址
  // lambda: 发送完成回调函数
  int ret = uv_udp_send(send_req, udp_handle_, &buffer, 1, get_addr(),
                        [](uv_udp_send_t *req, int status) {
                          // 发送完成回调
                          // status: 0表示成功，<0表示失败
                          if (status < 0) {
                            std::cerr << "[KCPConnection] UDP发送失败: "
                                      << uv_strerror(status) << std::endl;
                          }

                          // 释放发送缓冲区
                          char *buf = (char *)req->data;
                          delete[] buf;

                          // 释放发送请求对象
                          delete req;
                        });

  if (ret < 0) {
    // 发送失败，立即释放资源
    std::cerr << "[KCPConnection] uv_udp_send失败: " << uv_strerror(ret)
              << std::endl;
    delete[] send_buf;
    delete send_req;
    return ret;
  }

  return 0;
}
