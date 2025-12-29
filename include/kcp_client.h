#ifndef KCP_CLIENT_H
#define KCP_CLIENT_H

#include "kcp_connection.h"
#include <memory>
#include <string>
#include <uv.h>

/**
 * KCP客户端类
 * 管理单个KCP连接，提供客户端的连接管理功能
 */
class KCPClient {
public:
  /**
   * 构造函数
   * @param loop - libuv事件循环指针
   *               含义：所有的异步IO操作都在这个事件循环中执行
   *               一个线程通常只有一个事件循环
   *               通过uv_loop_init()或uv_default_loop()创建
   */
  KCPClient(uv_loop_t *loop);

  /**
   * 析构函数
   * 释放所有资源，断开连接
   */
  ~KCPClient();

  /**
   * 连接到服务器
   * @param server_ip - 服务器IP地址
   *                    例如："127.0.0.1"、"192.168.1.1"等
   *
   * @param server_port - 服务器端口号
   *                      建议范围：1024-65535
   *
   * @param conv - KCP会话ID（Conversation ID）
   *               含义：用于标识唯一的KCP连接，必须与服务器使用相同的conv
   *               建议范围：1 - 0xFFFFFFFF（不能为0）
   *               使用建议：可以由客户端生成，也可以由服务器分配
   *
   * @return 成功返回0，失败返回负数
   */
  int connect(const std::string &server_ip, int server_port, uint32_t conv);

  /**
   * 发送数据到服务器
   * @param data - 数据缓冲区指针
   * @param len - 数据长度
   *              建议范围：1字节 - 任意长度（KCP会自动分片）
   * @return 成功返回0，失败返回负数
   */
  int send(const char *data, int len);

  /**
   * 断开连接
   */
  void disconnect();

  /**
   * 运行事件循环
   * 这是一个阻塞调用，会一直运行直到循环停止
   */
  void run();

  /**
   * 停止事件循环
   */
  void stop();

  /**
   * 设置数据接收回调函数
   * @param cb - 回调函数对象
   */
  void set_data_callback(KCPConnection::DataCallback cb);

  /**
   * 设置连接关闭回调函数
   * @param cb - 回调函数对象
   */
  void set_close_callback(KCPConnection::CloseCallback cb);

  /**
   * 设置KCP参数
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
   * 检查是否已连接
   * @return 已连接返回true，否则返回false
   */
  bool is_connected() const;

  /**
   * 获取当前时间戳（毫秒）
   * @return 当前时间戳（毫秒）
   */
  static uint32_t get_current_ms();

private:
  /**
   * UDP接收回调函数（静态）
   * @param handle - UDP句柄
   * @param nread - 接收到的字节数
   * @param buf - 数据缓冲区
   * @param addr - 发送方地址
   * @param flags - 标志位
   */
  static void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                          const struct sockaddr *addr, unsigned flags);

  /**
   * 内存分配回调函数（静态）
   * @param handle - UDP句柄
   * @param suggested_size - 建议的缓冲区大小
   * @param buf - 输出的缓冲区结构
   */
  static void alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                           uv_buf_t *buf);

  /**
   * 定时器回调函数（静态）
   * @param handle - 定时器句柄
   */
  static void on_timer(uv_timer_t *handle);

  /**
   * 处理接收到的UDP数据
   * @param data - UDP数据
   * @param len - 数据长度
   */
  void handle_udp_data(const char *data, int len);

  /**
   * 更新连接状态
   */
  void update_connection();

private:
  uv_loop_t *loop_;                           // libuv事件循环
  uv_udp_t udp_handle_;                       // UDP句柄
  uv_timer_t timer_;                          // 定时器
  bool running_;                              // 运行状态
  std::shared_ptr<KCPConnection> connection_; // KCP连接

  // KCP配置参数
  int kcp_nodelay_;  // nodelay模式
  int kcp_interval_; // 更新间隔
  int kcp_resend_;   // 快速重传
  int kcp_nc_;       // 拥塞控制
  int kcp_sndwnd_;   // 发送窗口
  int kcp_rcvwnd_;   // 接收窗口
  int kcp_mtu_;      // MTU大小

  char recv_buffer_[65536]; // UDP接收缓冲区（64KB）
};

#endif // KCP_CLIENT_H
