#ifndef KCP_CONNECTION_H
#define KCP_CONNECTION_H

#include "ikcp.h"
#include <cstdint>
#include <functional>
#include <string>
#include <uv.h>

/**
 * KCP连接类
 * 封装单个KCP连接的管理，包括KCP状态、地址信息、超时检测等
 */
class KCPConnection {
public:
  // 连接状态枚举
  enum State {
    CONNECTING,    // 连接中（初始状态，等待握手完成）
    CONNECTED,     // 已连接（可以正常收发数据）
    DISCONNECTING, // 断开中（正在关闭，等待发送队列清空）
    DISCONNECTED   // 已断开（连接已关闭，不再可用）
  };

  // 数据接收回调：参数(连接指针, 数据缓冲区, 数据长度)
  using DataCallback = std::function<void(KCPConnection *, const char *, int)>;

  // 连接关闭回调：参数(连接指针)
  using CloseCallback = std::function<void(KCPConnection *)>;

  /**
   * 构造函数
   * @param conv - KCP会话ID（Conversation ID）
   *               含义：用于标识唯一的KCP连接，通信双方必须使用相同的conv
   *               建议范围：1 - 0xFFFFFFFF（不能为0）
   *               使用建议：可以使用连接ID、时间戳、哈希值等唯一标识
   *
   * @param udp_handle - libuv的UDP句柄指针，用于底层UDP数据发送
   *
   * @param addr - 对端的地址信息（IP和端口）
   *               用于向对端发送UDP数据包
   */
  KCPConnection(uint32_t conv, uv_udp_t *udp_handle,
                const struct sockaddr *addr);

  /**
   * 析构函数
   * 释放KCP资源和其他相关资源
   */
  ~KCPConnection();

  /**
   * 初始化KCP参数
   * 设置KCP的工作模式、窗口大小、MTU等关键参数
   *
   * @param nodelay - 是否启用nodelay模式
   *                  0：禁用（默认模式，适合普通应用）
   *                  1：启用（低延迟模式，适合游戏、实时通信）
   *
   * @param interval - KCP内部更新时钟间隔，单位毫秒
   *                   含义：KCP内部flush的时间间隔，影响数据发送频率
   *                   默认值：100ms
   *                   建议范围：10-100ms
   *                   推荐值：普通模式100ms，快速模式20-40ms，极速模式10ms
   *
   * @param resend - 快速重传模式
   *                 0：关闭快速重传（默认）
   *                 1：启用快速重传（发现丢包立即重传）
   *                 2：更激进的快速重传策略
   *                 建议范围：0-2
   *                 推荐：网络不稳定时使用1或2
   *
   * @param nc - 是否关闭拥塞控制
   *             0：启用拥塞控制（默认，适合公网）
   *             1：关闭拥塞控制（适合内网或带宽充足环境）
   *
   * @param sndwnd - 发送窗口大小
   *                 含义：最多可以发送多少个未确认的包
   *                 默认值：32
   *                 建议范围：32-512
   *                 推荐值：低延迟场景32-64，高吞吐场景128-256，极高吞吐256-512
   *
   * @param rcvwnd - 接收窗口大小
   *                 含义：最多可以接收多少个未处理的包
   *                 默认值：128
   *                 建议范围：128-1024
   *                 推荐值：一般设为sndwnd的2-4倍
   *
   * @param mtu - 最大传输单元大小，单位字节
   *              含义：每个UDP包的最大载荷大小
   *              默认值：1400字节
   *              建议范围：512-1472字节
   *              推荐值：公网1200-1400字节，内网1472字节，移动网络1200字节以下
   *              注意：需要考虑网络环境避免IP分片（MTU 1500 - IP头20 - UDP头8 =
   * 1472）
   */
  void init_kcp(int nodelay = 1, int interval = 10, int resend = 2, int nc = 1,
                int sndwnd = 128, int rcvwnd = 128, int mtu = 1400);

  /**
   * 设置KCP最小RTO（重传超时）时间
   * @param minrto - 最小RTO时间，单位毫秒
   *                 含义：RTO的最小值，即使RTT很小，RTO也不会低于此值
   *                 默认值：100ms
   *                 建议范围：10-100ms
   *                 推荐值：低延迟30ms，标准100ms
   *                 注意：设置过小可能导致不必要的重传
   */
  void set_minrto(int minrto);

  /**
   * 设置快速重传触发次数
   * @param fastresend - 快速重传触发ACK跨越次数
   *                     含义：收到多少个后续ACK就触发快速重传
   *                     默认值：0（关闭）
   *                     建议范围：0-5
   *                     推荐值：标准模式0，快速模式2，极速模式1
   *                     注意：与nodelay的resend参数配合使用
   */
  void set_fastresend(int fastresend);

  /**
   * 设置流模式
   * @param stream - 是否启用流模式
   *                 0：消息模式（默认），每次recv接收完整消息
   *                 1：流模式，类似TCP，数据连续无边界
   *                 建议：大多数情况使用消息模式（0）
   */
  void set_stream_mode(int stream);

  /**
   * 设置最大重传次数（死链接检测）
   * @param dead_link - 最大重传次数
   *                    含义：数据包重传达到此次数后认为连接断开
   *                    默认值：20次
   *                    建议范围：10-100
   *                    推荐值：稳定网络20，不稳定网络50
   *                    注意：次数过大会延迟连接断开检测
   */
  void set_dead_link(int dead_link);

  /**
   * 获取等待发送的包数量
   * @return 发送队列中等待发送的包数量
   *         用于流量控制，避免发送队列过长
   */
  int get_waitsnd() const;

  /**
   * 发送数据（可靠传输，通过KCP）
   * @param data - 数据缓冲区指针
   * @param len - 数据长度
   *              建议范围：1字节 - 任意长度（KCP会自动分片）
   *              注意：大数据会被自动分成多个包发送
   * @return 成功返回0，失败返回负数
   */
  int send(const char *data, int len);

  /**
   * 直接发送UDP数据（不可靠传输，不经过KCP）
   * @param data - 数据缓冲区指针
   * @param len - 数据长度
   *              建议范围：1-1472字节（避免IP分片）
   *              注意：超过MTU的数据不会自动分片
   *
   * @return 成功返回0，失败返回负数
   *
   * 使用场景：
   * - 心跳包：定期发送，丢失不影响业务
   * - 实时状态同步：位置、姿态等，最新数据优先
   * - 不重要的通知：可以容忍丢失的消息
   * - 广播消息：向多个目标发送
   *
   * 优点：
   * - 低延迟：没有重传和确认机制
   * - 低带宽：不占用KCP发送队列
   * - 简单高效：直接UDP传输
   *
   * 注意事项：
   * - 不保证送达：数据可能丢失
   * - 不保证顺序：可能乱序到达
   * - 不自动分片：需要业务层控制包大小
   * - 建议大小：小于1400字节
   */
  int send_udp_direct(const char *data, int len);

  /**
   * 输入UDP数据到KCP
   * 当从UDP接收到数据时，需要调用此函数将数据输入到KCP协议栈
   * @param data - UDP数据缓冲区
   * @param len - 数据长度
   * @return 成功返回0，失败返回负数
   */
  int input(const char *data, int len);

  /**
   * 更新KCP状态
   * KCP需要定期调用此函数来处理超时重传、窗口更新等
   * @param current - 当前时间戳，单位毫秒
   *                  含义：KCP内部用于计算超时和重传的时间基准
   *                  必须是单调递增的时间戳
   *                  建议：使用系统启动时间或统一的时间基准
   */
  void update(uint32_t current);

  /**
   * 检查下次需要update的时间
   * @param current - 当前时间戳，单位毫秒
   * @return 下次需要调用update的时间戳（毫秒）
   */
  uint32_t check(uint32_t current);

  /**
   * 接收数据
   * 从KCP接收缓冲区读取数据
   * @return 如果有数据返回true，否则返回false
   */
  bool recv();

  /**
   * 设置数据接收回调函数
   * @param cb - 回调函数对象
   */
  void set_data_callback(DataCallback cb) { data_callback_ = cb; }

  /**
   * 设置连接关闭回调函数
   * @param cb - 回调函数对象
   */
  void set_close_callback(CloseCallback cb) { close_callback_ = cb; }

  /**
   * 获取会话ID
   */
  uint32_t get_conv() const { return conv_; }

  /**
   * 获取连接状态
   */
  State get_state() const { return state_; }

  /**
   * 设置连接状态
   */
  void set_state(State state) { state_ = state; }

  /**
   * 更新最后活跃时间
   * 用于连接超时检测
   * @param current - 当前时间戳，单位毫秒
   */
  void update_active_time(uint32_t current) { last_active_time_ = current; }

  /**
   * 检查连接是否超时
   * @param current - 当前时间戳，单位毫秒
   * @param timeout - 超时时长，单位毫秒
   *                  建议范围：10000-60000ms（10-60秒）
   *                  推荐值：根据应用场景调整，游戏可设置较短（15-30秒），其他应用可设置较长
   * @return 如果超时返回true，否则返回false
   */
  bool is_timeout(uint32_t current, uint32_t timeout) const;

  /**
   * 关闭连接
   */
  void close();

  /**
   * 获取对端地址
   */
  const struct sockaddr *get_addr() const {
    return (const struct sockaddr *)&addr_;
  }

private:
  /**
   * KCP输出回调函数（静态）
   * KCP需要发送数据时会调用此函数
   * @param buf - 要发送的数据缓冲区
   * @param len - 数据长度
   * @param kcp - KCP控制块指针
   * @param user - 用户数据指针（这里是KCPConnection对象指针）
   * @return 成功返回0，失败返回负数
   */
  static int udp_output(const char *buf, int len, ikcpcb *kcp, void *user);

  /**
   * 实际的UDP数据发送函数
   * @param buf - 数据缓冲区
   * @param len - 数据长度
   * @return 成功返回0，失败返回负数
   */
  int output(const char *buf, int len);

private:
  ikcpcb *kcp_;                  // KCP控制块指针
  uint32_t conv_;                // 会话ID
  uv_udp_t *udp_handle_;         // UDP句柄
  struct sockaddr_storage addr_; // 对端地址
  State state_;                  // 连接状态
  uint32_t last_active_time_;    // 最后活跃时间（毫秒）

  DataCallback data_callback_;   // 数据接收回调
  CloseCallback close_callback_; // 连接关闭回调

  char recv_buffer_[1024 * 64]; // 接收缓冲区（64KB，建议范围：4KB-128KB）
};

#endif // KCP_CONNECTION_H
