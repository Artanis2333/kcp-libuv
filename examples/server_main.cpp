#include "kcp_server.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>

// 全局服务器指针，用于信号处理
KCPServer *g_server = nullptr;

/**
 * 信号处理函数
 * 捕获Ctrl+C信号，优雅地关闭服务器
 */
void signal_handler(int sig) {
  if (sig == SIGINT) {
    std::cout << "\n[Main] 收到中断信号，正在关闭服务器..." << std::endl;
    if (g_server) {
      g_server->stop();
    }
  }
}

/**
 * 服务器主程序
 * 演示如何使用KCPServer类创建一个基于KCP的UDP服务器
 */
int main(int argc, char *argv[]) {
  // 检查命令行参数
  if (argc != 2) {
    std::cout << "用法: " << argv[0] << " <端口号>" << std::endl;
    std::cout << "示例: " << argv[0] << " 8888" << std::endl;
    return 1;
  }

  int port = std::atoi(argv[1]);
  if (port <= 0 || port > 65535) {
    std::cerr << "错误：端口号必须在1-65535之间" << std::endl;
    return 1;
  }

  std::cout << "========================================" << std::endl;
  std::cout << "  KCP UDP 服务器示例程序" << std::endl;
  std::cout << "========================================" << std::endl;

  // 创建并初始化libuv事件循环
  // uv_loop_t: libuv的事件循环结构
  // 事件循环是异步IO的核心，所有的IO事件都在这个循环中处理
  uv_loop_t loop;
  int ret = uv_loop_init(&loop);
  if (ret < 0) {
    std::cerr << "初始化事件循环失败: " << uv_strerror(ret) << std::endl;
    return 1;
  }

  // 创建KCP服务器
  KCPServer server(&loop);
  g_server = &server;

  // 设置KCP参数
  // 这里使用低延迟配置，适合游戏、实时通信等场景
  // 参数说明：
  // - nodelay=1: 启用nodelay模式，降低延迟
  // - interval=10: 10ms更新一次，提高响应速度
  // - resend=2: 激进的快速重传策略
  // - nc=1: 关闭拥塞控制，适合内网或带宽充足的场景
  // - sndwnd=128: 发送窗口128个包
  // - rcvwnd=128: 接收窗口128个包
  // - mtu=1400: MTU设为1400字节，适合大多数网络环境
  server.set_kcp_config(1, 10, 2, 1, 128, 128, 1400);

  // 设置连接超时时间为30秒
  // 如果30秒内没有收到客户端数据，连接将被关闭
  server.set_timeout(30000);

  // 设置新连接回调
  // 当有新客户端连接时，会调用这个回调函数
  server.set_new_connection_callback([](KCPConnection *conn) {
    std::cout << "\n[Main] 新客户端连接，conv=" << conn->get_conv()
              << std::endl;

    // ============================================================
    // 展示高级KCP参数设置（可选）
    // ============================================================

    // 1. 设置最小RTO（重传超时时间）
    // 较小的RTO可以更快发现丢包并重传，适合低延迟场景
    conn->set_minrto(30); // 30ms最小RTO

    // 2. 设置快速重传触发次数
    // 收到2个后续ACK就触发快速重传，例如收到ACK 5,7,8时会立即重传包6
    conn->set_fastresend(2);

    // 3. 设置消息模式（默认就是0，这里仅作演示）
    // 0=消息模式（保留消息边界），1=流模式（类似TCP）
    conn->set_stream_mode(0);

    // 4. 设置最大重传次数（死链接检测）
    // 数据包重传20次后认为连接断开
    conn->set_dead_link(20);

    std::cout << "[Main] 已为连接设置高级KCP参数" << std::endl;

    // ============================================================
    // 设置数据接收回调
    // ============================================================
    conn->set_data_callback([](KCPConnection *c, const char *data, int len) {
      std::cout << "[Main] 收到客户端数据，conv=" << c->get_conv()
                << ", len=" << len << ", data=" << std::string(data, len)
                << std::endl;

      // 检查发送队列状态
      int waitsnd = c->get_waitsnd();
      if (waitsnd > 100) {
        std::cout << "[Main] 警告：发送队列积压，waitsnd=" << waitsnd
                  << " (建议<100)" << std::endl;
        // 实际应用中，这里可以考虑暂停发送或告警
      }

      // ============================================================
      // 展示两种发送方式
      // ============================================================

      std::string msg(data, len);

      // 场景1：重要的业务数据 -> 使用可靠传输（send）
      if (msg.find("ATTACK") != std::string::npos ||
          msg.find("BUY") != std::string::npos) {
        std::string reply = "服务器确认: " + msg;
        c->send(reply.c_str(), reply.length());
        std::cout << "[Main] 使用KCP可靠传输回复重要消息" << std::endl;
      }
      // 场景2：心跳包 -> 使用不可靠传输（send_udp_direct）
      else if (msg == "PING") {
        const char *pong = "PONG";
        c->send_udp_direct(pong, 4);
        std::cout << "[Main] 使用UDP直接发送回复心跳包" << std::endl;
      }
      // 场景3：普通消息 -> 使用可靠传输
      else {
        std::string reply = "服务器回复: " + msg;
        c->send(reply.c_str(), reply.length());
      }

      // ============================================================
      // 不方便在此展示的场景（用注释说明）
      // ============================================================

      // 场景4：实时状态广播（如位置同步）
      // 建议：使用send_udp_direct发送，因为最新数据优先，旧数据可丢弃
      // 示例代码：
      //   struct PlayerState { uint32_t id; float x, y, z; };
      //   PlayerState state = {123, 100.0f, 50.0f, 200.0f};
      //   c->send_udp_direct((char*)&state, sizeof(state));

      // 场景5：定期状态上报（如每秒一次的统计信息）
      // 建议：使用send_udp_direct，丢失一次不影响，下次会继续上报
      // 示例代码：
      //   const char* stats = "FPS:60,PING:30";
      //   c->send_udp_direct(stats, strlen(stats));

      // 场景6：流量控制（根据waitsnd动态调整）
      // 当waitsnd过大时，可以：
      // - 暂停发送非重要消息
      // - 降低发送频率
      // - 切换到UDP直接发送
      // 示例代码：
      //   if (c->get_waitsnd() > 500) {
      //     // 队列积压严重，切换到UDP直接发送
      //     c->send_udp_direct(data, len);
      //   } else {
      //     // 队列正常，使用可靠传输
      //     c->send(data, len);
      //   }
    });

    // 设置连接关闭回调
    conn->set_close_callback([](KCPConnection *c) {
      std::cout << "[Main] 客户端断开连接，conv=" << c->get_conv() << std::endl;
    });
  });

  // 绑定并启动服务器
  // 监听所有网络接口（0.0.0.0）和指定端口
  ret = server.bind_and_listen("0.0.0.0", port);
  if (ret < 0) {
    std::cerr << "启动服务器失败" << std::endl;
    uv_loop_close(&loop);
    return 1;
  }

  std::cout << "\n服务器配置:" << std::endl;
  std::cout << "  监听地址: 0.0.0.0:" << port << std::endl;
  std::cout << "  KCP模式: 低延迟模式" << std::endl;
  std::cout << "  连接超时: 30秒" << std::endl;

  /*
  std::cout << "\n============================================" << std::endl;
  std::cout << "  KCP update() 和 check() 调用说明" << std::endl;
  std::cout << "============================================" << std::endl;
  std::cout << "\n本框架已经在内部自动处理KCP的update调用：" << std::endl;
  std::cout << "1. 服务器启动时创建定时器（10ms间隔）" << std::endl;
  std::cout << "2. 定时器回调中调用 update_connections()" << std::endl;
  std::cout
      << "3. update_connections() 遍历所有连接并调用 conn->update(current)"
      << std::endl;
  std::cout << "\n代码位置：src/kcp_server.cpp" << std::endl;
  std::cout << "  - 定时器启动: bind_and_listen() 中的 uv_timer_start(&timer_, "
               "on_timer, 0, 10)"
            << std::endl;
  std::cout << "  - 定时器回调: on_timer() -> update_connections()"
            << std::endl;
  std::cout << "  - KCP更新: update_connections() 中的 conn->update(current)"
            << std::endl;
  std::cout << "\ncheck() 函数说明：" << std::endl;
  std::cout << "  - 作用：查询下次需要调用update的时间" << std::endl;
  std::cout << "  - 用途：优化定时器，避免不必要的update调用" << std::endl;
  std::cout << "  - 本框架使用固定10ms定时器，已足够高效" << std::endl;
  std::cout << "\n如需使用check()优化：" << std::endl;
  std::cout << "  uint32_t next = conn->check(current);" << std::endl;
  std::cout << "  uint32_t wait = next - current;" << std::endl;
  std::cout << "  // 设置定时器在wait毫秒后触发" << std::endl;
  std::cout << "============================================\n" << std::endl;
  */
  std::cout << "\n按 Ctrl+C 停止服务器\n" << std::endl;

  // 注册信号处理函数
  // 捕获Ctrl+C信号，优雅地关闭服务器
  signal(SIGINT, signal_handler);

  // 运行事件循环
  // 这是一个阻塞调用，会一直运行直到调用stop()
  server.run();

  // 清理资源
  uv_loop_close(&loop);

  std::cout << "\n服务器已关闭" << std::endl;
  return 0;
}
