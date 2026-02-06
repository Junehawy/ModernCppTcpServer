# ModernTCP - C++20 Reactor 模式 TCP 服务器实现

一个基于 C++20 实现的 Reactor 模式 TCP 服务器练习项目，支持原始 TCP 行协议和 HTTP/1.1 双模式切换、多线程事件处理以及基本的连接管理功能。该项目主要用于学习现代 C++ 网络编程实践，包括事件驱动模型、线程协作和协议解析。

## 核心特性

- **多 Reactor 架构**：主线程负责 accept，新连接轮询分发到多个工作线程的独立 epoll 实例处理（支持配置 Reactor 数量）
- **自定义 Buffer**：实现零拷贝环形缓冲区，支持输出缓冲区低水位自动收缩（128KB-640KB 范围）
- **协议自适应**：根据连接首包内容自动检测并切换到 HTTP/1.1 或行级文本协议处理
- **HTTP 支持**：实现基本的 HTTP/1.1 解析器，支持 keep-alive 连接复用和请求流水线（最大排队深度 100）
- **优雅关闭**：使用 `std::jthread` 和 `std::stop_token` 实现协作式线程取消，结合 `SO_LINGER` 确保残余数据发送
- **空闲连接超时**：基于 `std::multimap` 实现 O(log n) 复杂度的空闲超时检测和自动关闭

## 依赖要求

- CMake ≥ 3.15
- 支持 C++20 的编译器（g++ ≥ 10, clang++ ≥ 10）
- Linux 系统（使用了 epoll、eventfd 等 Linux 特定 API）
- spdlog（通过 git submodule 引入，无需手动安装）

## 构建步骤

```bash
# 1. 克隆项目并初始化 submodule
git clone https://github.com/Junehawy/ModernCppTcpServer.git
cd ModernCppTcpServer
git submodule update --init --recursive

# 2. 创建构建目录
mkdir build && cd build

# 3. 配置并编译
cmake ..
cmake --build .   # 或者 make -j$(nproc)

# 构建产物在 bin/ 目录下：server 和 client
```

## 运行示例

### 启动服务器（默认 8 个 Reactor）

```bash
./bin/server
```

### 行协议回显测试（使用内置客户端）

```bash
./bin/client
```

- 输入任意文本，按回车发送
- 服务器会原样回显（自动添加换行）
- 输入 `quit` 退出客户端

### HTTP 测试（使用 curl 或浏览器）

```bash
# 根路径欢迎页
curl http://127.0.0.1:9999/

# Hello 示例
curl http://127.0.0.1:9999/hello

# JSON 示例
curl http://127.0.0.1:9999/json

# 查看请求信息
curl http://127.0.0.1:9999/info

# POST echo 示例
curl -X POST -d "hello world" http://127.0.0.1:9999/echo
```

## 项目结构（简要）

```
.
├── include/              # 公共头文件
├── src/
│   ├── lib_src/          # 核心网络库实现（TcpServer、EpollConnection、HttpParser 等）
│   ├── main_server.cpp   # 服务器入口
│   └── main_client.cpp   # 简单行协议客户端
├── extern/spdlog         # spdlog 子模块
├── CMakeLists.txt
└── README.md
```