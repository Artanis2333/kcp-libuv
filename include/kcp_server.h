#ifndef KCP_SERVER_H
#define KCP_SERVER_H

#include "kcp_connection.h"
#include <map>
#include <memory>
#include <string>
#include <uv.h>

/**
 * KCP服务器类
 * 管理多个KCP连接，提供服务器端的连接管理功能
 */
class KCPServer {
public:
  // 新连接回调：参数(连接指针)
  using NewConnectionCallback = std::function<void(KCPConnection *)>;

  /**
   * 构造函数
   * @param loop - libuv事件循环指针
   *               含义：所有的异步IO操作都在这个事件循环中执行
   *               一个线程通常只有一个事件循环
   *               通过uv_loop_init()或uv_default_loop()创建
   */
  KCPServer(uv_loop_t *loop);

  /**
   * 析构函数
   * 释放所有资源，关闭所有连接
   */
  ~KCPServer();

  /**
   * 绑定并启动服务器
   * @param ip - 绑定的IP地址
   *             "0.0.0.0" 表示监听所有网络接口
   *             "127.0.0.1" 表示只监听本地回环接口
   *             也可以绑定到特定的网卡IP
   *
   * @param port - 绑定的端口号
   *               建议范围：1024-65535
   *               注意：1024以下的端口需要root权限
   *               推荐：使用1024以上的端口避免权限问题
   *
   * @return 成功返回0，失败返回负数
   */
  int bind_and_listen(const std::string &ip, int port);

  /**
   * 运行事件循环
   * 这是一个阻塞调用，会一直运行直到循环停止
   * 在事件循环中处理所有的IO事件、定时器等
   */
  void run();

  /**
   * 停止事件循环
   * 会停止所有定时器和IO操作，退出事件循环
   */
  void stop();

  /**
   * 设置新连接回调函数
   * @param cb - 回调函数对象
   */
  void set_new_connection_callback(NewConnectionCallback cb) {
    new_connection_callback_ = cb;
  }

  /**
   * 设置KCP参数（应用于所有新连接）
   * @param nodelay - nodelay模式，建议值：0或1
   * @param interval - 更新间隔，建议范围：10-100ms
   * @param resend - 快速重传，建议范围：0-2
   * @param nc - 拥塞控制，建议值：0或1
   * @param sndwnd - 发送窗口，建议范围：32-512
   * @param rcvwnd - 接收窗口，建议范围：128-1024
   * @param mtu - MTU大小，建议范围：512-1472
   */
  void set_kcp_config(int nodelay, int interval, int resend, int nc, int sndwnd,
                      int rcvwnd, int mtu);

  /**
   * 设置连接超时时间
   * @param timeout - 超时时长，单位毫秒
   *                  建议范围：10000-60000ms（10-60秒）
   *                  默认值：30000ms（30秒）
   */
  void set_timeout(uint32_t timeout) { timeout_ = timeout; }

  /**
   * 获取当前时间戳（毫秒）
   * 使用单调时钟，不受系统时间调整影响
   * @return 当前时间戳（毫秒）
   */
  static uint32_t get_current_ms();

private:
  /**
   * UDP接收回调函数（静态）
   * libuv接收到UDP数据时会调用此函数
   *
   * @param handle - UDP句柄
   * @param nread - 接收到的字节数，<0表示错误
   * @param buf - 数据缓冲区
   * @param addr - 发送方地址
   * @param flags - 标志位（如UV_UDP_PARTIAL表示数据被截断）
   */
  static void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                          const struct sockaddr *addr, unsigned flags);

  /**
   * 内存分配回调函数（静态）
   * libuv需要接收缓冲区时会调用此函数
   *
   * @param handle - UDP句柄
   * @param suggested_size - 建议的缓冲区大小
   *                         libuv根据接收情况建议的大小
   *                         通常为65536字节（UDP最大包大小）
   * @param buf - 输出的缓冲区结构
   */
  static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                           uv_buf_t *buf);

  /**
   * 定时器回调函数（静态）
   * 定期更新所有KCP连接的状态
   *
   * @param handle - 定时器句柄
   */
  static void on_timer(uv_timer_t *handle);

  /**
   * 处理接收到的UDP数据
   * 根据conv查找或创建连接，将数据输入到KCP
   *
   * @param data - UDP数据
   * @param len - 数据长度
   * @param addr - 发送方地址
   */
  void handle_udp_data(const char *data, int len, const struct sockaddr *addr);

  /**
   * 查找或创建连接
   * 根据conv查找现有连接，如果不存在则创建新连接
   *
   * @param conv - 会话ID
   * @param addr - 对端地址
   * @return 连接指针
   */
  KCPConnection *find_or_create_connection(uint32_t conv,
                                           const struct sockaddr *addr);

  /**
   * 移除连接
   * @param conv - 会话ID
   */
  void remove_connection(uint32_t conv);

  /**
   * 更新所有连接
   * 定期调用所有连接的update函数，处理超时检测
   */
  void update_connections();

  /**
   * 连接数据接收回调
   * @param conn - 连接指针
   * @param data - 数据缓冲区
   * @param len - 数据长度
   */
  void on_connection_data(KCPConnection *conn, const char *data, int len);

  /**
   * 连接关闭回调
   * @param conn - 连接指针
   */
  void on_connection_close(KCPConnection *conn);

private:
  uv_loop_t *loop_;     // libuv事件循环
  uv_udp_t udp_handle_; // UDP句柄
  uv_timer_t timer_;    // 定时器（用于KCP update）
  bool running_;        // 服务器运行状态
  uint32_t next_conv_; // 下一个可用的会话ID（服务器端可以生成conv）
  uint32_t timeout_; // 连接超时时间（毫秒）

  // KCP配置参数
  int kcp_nodelay_;  // nodelay模式
  int kcp_interval_; // 更新间隔
  int kcp_resend_;   // 快速重传
  int kcp_nc_;       // 拥塞控制
  int kcp_sndwnd_;   // 发送窗口
  int kcp_rcvwnd_;   // 接收窗口
  int kcp_mtu_;      // MTU大小

  // 连接管理
  // 使用map存储所有连接，key是conv，value是连接的智能指针
  std::map<uint32_t, std::shared_ptr<KCPConnection>> connections_;

  NewConnectionCallback new_connection_callback_; // 新连接回调

  char recv_buffer_[65536]; // UDP接收缓冲区（64KB）
};

#endif // KCP_SERVER_H
