#include "kcp_client.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <string>
#include <thread>

// 全局客户端指针，用于信号处理
KCPClient *g_client = nullptr;

/**
 * 信号处理函数
 * 捕获Ctrl+C信号，优雅地关闭客户端
 */
void signal_handler(int sig) {
  if (sig == SIGINT) {
    std::cout << "\n[Main] 收到中断信号，正在关闭客户端..." << std::endl;
    if (g_client) {
      g_client->stop();
    }
  }
}

/**
 * 客户端主程序
 * 演示如何使用KCPClient类创建一个基于KCP的UDP客户端
 */
int main(int argc, char *argv[]) {
  // 检查命令行参数
  if (argc != 3) {
    std::cout << "用法: " << argv[0] << " <服务器IP> <服务器端口>" << std::endl;
    std::cout << "示例: " << argv[0] << " 127.0.0.1 8888" << std::endl;
    return 1;
  }

  std::string server_ip = argv[1];
  int server_port = std::atoi(argv[2]);
  if (server_port <= 0 || server_port > 65535) {
    std::cerr << "错误：端口号必须在1-65535之间" << std::endl;
    return 1;
  }

  std::cout << "========================================" << std::endl;
  std::cout << "  KCP UDP 客户端示例程序" << std::endl;
  std::cout << "========================================" << std::endl;

  // 创建并初始化libuv事件循环
  uv_loop_t loop;
  int ret = uv_loop_init(&loop);
  if (ret < 0) {
    std::cerr << "初始化事件循环失败: " << uv_strerror(ret) << std::endl;
    return 1;
  }

  // 创建KCP客户端
  KCPClient client(&loop);
  g_client = &client;

  // 设置KCP参数
  // 使用与服务器相同的低延迟配置
  client.set_kcp_config(1, 10, 2, 1, 128, 128, 1400);

  // 设置数据接收回调
  // 当收到服务器数据时，会调用这个回调函数
  client.set_data_callback([](KCPConnection *conn, const char *data, int len) {
    std::cout << "[Main] 收到服务器回复: " << std::string(data, len)
              << std::endl;

    // 展示：检查等待发送的包数量
    int waitsnd = conn->get_waitsnd();
    std::cout << "[Main] 当前发送队列: waitsnd=" << waitsnd << std::endl;
  });

  // 设置连接关闭回调
  client.set_close_callback([](KCPConnection *conn) {
    std::cout << "[Main] 连接已关闭" << std::endl;
  });

  // 连接到服务器
  // 这里使用固定的conv值1234
  // 在实际应用中，可以使用随机数、时间戳等方式生成唯一的conv
  uint32_t conv = 1234;
  ret = client.connect(server_ip, server_port, conv);
  if (ret < 0) {
    std::cerr << "连接服务器失败" << std::endl;
    uv_loop_close(&loop);
    return 1;
  }

  // ============================================================
  // 展示：连接成功后设置高级KCP参数
  // ============================================================

  // 注意：需要在连接建立后设置，因为connection_是在connect时创建的
  // 实际应用中，可以在连接成功后立即设置这些参数

  // 这里我们无法直接访问connection_，因为它是private的
  // 但在实际应用中，可以在set_data_callback或其他回调中设置
  // 或者提供公开的接口来设置这些参数

  std::cout << "\n客户端配置:" << std::endl;
  std::cout << "  服务器地址: " << server_ip << ":" << server_port << std::endl;
  std::cout << "  会话ID: " << conv << std::endl;
  std::cout << "  KCP模式: 低延迟模式" << std::endl;
  std::cout << "\n按 Ctrl+C 断开连接\n" << std::endl;

  // 注册信号处理函数
  signal(SIGINT, signal_handler);

  // 在单独的线程中发送测试数据
  // 这样可以同时运行事件循环和发送数据
  std::thread send_thread([&client]() {
    // 等待1秒，确保连接建立
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "\n============================================" << std::endl;
    std::cout << "开始发送测试消息，演示不同的使用场景" << std::endl;
    std::cout << "============================================\n" << std::endl;

    // ============================================================
    // 场景1：发送心跳包（使用UDP直接发送，不可靠）
    // ============================================================
    std::cout << "[场景1] 发送心跳包（UDP直接发送）" << std::endl;
    // 注意：这需要修改KCPClient来暴露connection_
    // 这里用注释说明如何使用
    // client.get_connection()->send_udp_direct("PING", 4);
    std::cout << "  代码示例: conn->send_udp_direct(\"PING\", 4);" << std::endl;
    std::cout << "  说明: 心跳包可以丢失，使用UDP直接发送更高效\n" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ============================================================
    // 场景2：发送重要业务数据（使用KCP可靠传输）
    // ============================================================
    std::cout << "[场景2] 发送重要业务数据（KCP可靠传输）" << std::endl;
    const char *attack_msg = "ATTACK: Enemy #123";
    int ret = client.send(attack_msg, strlen(attack_msg));
    if (ret == 0) {
      std::cout << "  已发送: " << attack_msg << std::endl;
      std::cout << "  说明: 重要数据使用KCP可靠传输，保证送达\n" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ============================================================
    // 场景3：发送购买请求（使用KCP可靠传输）
    // ============================================================
    std::cout << "[场景3] 发送购买请求（KCP可靠传输）" << std::endl;
    const char *buy_msg = "BUY: Item #456, Price: 100";
    ret = client.send(buy_msg, strlen(buy_msg));
    if (ret == 0) {
      std::cout << "  已发送: " << buy_msg << std::endl;
      std::cout << "  说明: 交易数据必须使用可靠传输\n" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // ============================================================
    // 场景4：连续发送多条普通消息
    // ============================================================
    std::cout << "[场景4] 连续发送多条普通消息" << std::endl;
    for (int i = 1; i <= 5; i++) {
      std::string message = "测试消息 #" + std::to_string(i);
      ret = client.send(message.c_str(), message.length());
      if (ret < 0) {
        std::cerr << "  发送失败" << std::endl;
        break;
      }
      std::cout << "  已发送: " << message << std::endl;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "  说明: 普通消息也使用可靠传输，保证顺序\n" << std::endl;

    // ============================================================
    // 不方便在示例中展示的场景（用注释说明）
    // ============================================================
    std::cout << "\n[其他使用场景说明]" << std::endl;

    std::cout << "\n场景5: 实时位置同步（UDP直接发送）" << std::endl;
    std::cout << "  适用: 游戏中的位置、姿态更新" << std::endl;
    std::cout << "  代码示例:" << std::endl;
    std::cout << "    struct Position { float x, y, z; };" << std::endl;
    std::cout << "    Position pos = {100.0f, 50.0f, 200.0f};" << std::endl;
    std::cout << "    conn->send_udp_direct((char*)&pos, sizeof(pos));"
              << std::endl;
    std::cout << "  说明: 位置数据高频更新，最新数据优先，旧数据可丢弃"
              << std::endl;

    std::cout << "\n场景6: 流量控制（根据waitsnd动态选择）" << std::endl;
    std::cout << "  适用: 网络拥塞时的自适应策略" << std::endl;
    std::cout << "  代码示例:" << std::endl;
    std::cout << "    if (conn->get_waitsnd() > 500) {" << std::endl;
    std::cout << "      // 队列积压，降级为UDP直接发送" << std::endl;
    std::cout << "      conn->send_udp_direct(data, len);" << std::endl;
    std::cout << "    } else {" << std::endl;
    std::cout << "      // 正常情况，使用可靠传输" << std::endl;
    std::cout << "      conn->send(data, len);" << std::endl;
    std::cout << "    }" << std::endl;
    std::cout << "  说明: 根据发送队列状态动态调整发送策略" << std::endl;

    std::cout << "\n场景7: 高级参数设置（连接建立后）" << std::endl;
    std::cout << "  代码示例:" << std::endl;
    std::cout << "    conn->set_minrto(30);         // 设置最小RTO"
              << std::endl;
    std::cout << "    conn->set_fastresend(2);      // 设置快速重传"
              << std::endl;
    std::cout << "    conn->set_stream_mode(0);     // 设置消息模式"
              << std::endl;
    std::cout << "    conn->set_dead_link(20);      // 设置死链接检测"
              << std::endl;
    std::cout << "  说明: 这些参数在服务器端已演示，客户端使用方法相同"
              << std::endl;

    std::cout << "\n============================================" << std::endl;
    std::cout << "测试完成，按 Ctrl+C 退出" << std::endl;
    std::cout << "============================================\n" << std::endl;
  });

  // 运行事件循环
  // 这是一个阻塞调用，会一直运行直到调用stop()
  client.run();

  // 等待发送线程结束
  if (send_thread.joinable()) {
    send_thread.join();
  }

  // 清理资源
  uv_loop_close(&loop);

  std::cout << "\n客户端已关闭" << std::endl;
  return 0;
}
