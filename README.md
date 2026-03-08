# ModernTCP - C++20 Reactor 模式 TCP 服务器实现

一个基于 C++20 实现的高性能网络服务器，从零构建 Reactor 网络框架，集成 HTTP/1.1、自定义 Protobuf 协议与 gRPC 三种通信模型。该项目主要用于学习现代 C++ 网络编程实践，包括事件驱动模型、线程协作和协议解析。

## 核心特性

- **多 Reactor 架构**：主线程负责 accept，新连接通过原子轮询分发到 N 个独立 epoll 实例，跨线程投递采用 eventfd + functor 队列，IO 操作完全在 Reactor 线程内执行，无锁竞争
- **零拷贝缓冲区**：基于双下标的环形 Buffer，消费操作 O(1) 无内存移动，扩容优先回收已消费区域，输出缓冲区支持低水位自动收缩（128KB～640KB）
- **协议自适应**：同一端口通过首包探测自动识别 HTTP/1.1、Length-prefix Protobuf、Line 三种协议，识别后直接路由无重复检测
- **HTTP/1.1 实现**：增量 FSM 解析器支持分片到达，keep-alive 连接复用，Pipeline 最大排队深度 100，路由层支持中间件链式拦截
- **gRPC Unary**：基于 gRPC C++ 实现同步 Unary RPC，ServerBuilder 统一管理服务注册与线程池
- **优雅关闭**：`std::jthread` + `std::stop_token` 协作式取消，结合 `SO_LINGER` 确保残余数据发送完毕后关闭连接
- **空闲超时**：`std::multimap<time_point, fd>` 按过期时间有序存储，每轮 epoll_wait 循环末尾扫描一次，复杂度 O(log n)

## 依赖要求

- CMake ≥ 3.15
- 支持 C++20 的编译器（g++ ≥ 13, clang++ ≥ 16）
- Linux 系统（使用了 epoll、eventfd、accept4 等 Linux 特定 API）
- spdlog（通过 git submodule 引入，无需手动安装）
- Protobuf（`apt install libprotobuf-dev protobuf-compiler`）
- gRPC（`apt install libgrpc++-dev protobuf-compiler-grpc`）

## 构建步骤

### 本地构建
```bash
# 1. 克隆项目并初始化 submodule
git clone https://github.com/Junehawy/ModernCppTcpServer.git
cd ModernCppTcpServer
git submodule update --init --recursive

# 2. 配置并编译
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 构建产物在 build/bin/ 目录下
./build/bin/server
./build/bin/client
./build/bin/grpc_client
```

### Docker 构建
```bash
# 构建镜像并启动
docker compose -f docker/docker-compose.yml up --build

# 后台运行
docker compose -f docker/docker-compose.yml up --build -d

# 停止
docker compose -f docker/docker-compose.yml down
```

服务启动后：
- HTTP 接口：`http://localhost:9999`
- gRPC 接口：`localhost:50051`

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

## 项目结构

```
ModernCSProject/
│
├── CMakeLists.txt              # 顶层构建，输出 server / client / grpc_client
├── cmake/
│   ├── Dependencies.cmake      # spdlog / Protobuf / gRPC 查找
│   └── ProtoGen.cmake          # 从 .proto 自动生成 .pb.h / .grpc.pb.h
│
├── proto/
│   ├── message.proto           # 基础消息：Request / Response
│   └── service.proto           # gRPC 服务定义（Echo）
│
├── include/                    # 所有公共头文件，与 src/ 结构镜像
│   ├── common/
│   │   ├── config.h            # 全局常量（buffer size、超时、gRPC 参数）
│   │   └── string_utils.h      # trim / to_lower / rtrim_cr
│   ├── net/
│   │   ├── buffer.h            # 零拷贝环形缓冲区
│   │   ├── net_utils.h         # Socket / Epoll RAII 封装，syscall 检查
│   │   ├── base_connection.h   # 连接基类，管理生命周期和 IO 缓冲
│   │   ├── epoll_connection.h  # 具体连接：协议分发、HTTP pipelining
│   │   ├── sub_reactor.h       # 单个 epoll 事件循环 + 连接表
│   │   ├── tcp_server.h        # Acceptor + SubReactor 集群管理
│   │   ├── tcp_client.h        # 同步 TCP 客户端（测试用）
│   │   └── client_handler.h    # 连接工厂回调类型定义
│   ├── http/
│   │   ├── http_types.h        # Request / Response 结构体，HttpParser
│   │   └── http_router.h       # 路由注册，Middleware 类型定义
│   └── grpc/
│       └── grpc_server.h       # GrpcServer 封装（生命周期管理）
│
├── src/
│   ├── net/                    # 网络层实现
│   │   ├── base_connection.cpp
│   │   ├── epoll_connection.cpp
│   │   ├── sub_reactor.cpp
│   │   └── tcp_server.cpp
│   ├── http/                   # HTTP 层实现
│   │   └── http_types.cpp      # HttpParser FSM
│   ├── grpc/                   # gRPC 服务实现
│   │   ├── grpc_server.cpp
│   │   └── echo_service_impl.{h,cpp}    # Unary RPC
│   ├── server/
│   │   ├── main.cpp            # 程序入口，双服务并行启动
│   │   └── routes.cpp          # HTTP 路由 + 中间件注册
│   └── client/
│       ├── main.cpp            # TCP 客户端 CLI（text / protobuf 模式）
│       ├── tcp_client.cpp
│       └── grpc_client_main.cpp # gRPC 三种 RPC 演示客户端
│
├── docker/
│   ├── Dockerfile              # 多阶段构建（builder + runtime）
│   └── docker-compose.yml
│
└── scripts/
    └── gen_proto.sh            # 手动重新生成 proto 代码
```