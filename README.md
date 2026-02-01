# MordernTCP - 轻量级高并发TCP服务器框架
基于 C++20 的 Reactor 模式 TCP 服务器，支持自动协议检测（原始 TCP / HTTP 双模式）、多线程负载均衡和优雅关闭.

## 核心特性

- **多 Reactor 架构**：主线程 accept + 多工作线程 epoll，支持 CPU 亲和性
- **零拷贝缓冲**：自实现 Buffer 类，支持内存水位自动收缩 (128KB-640KB)
- **协议自适应**：根据首包内容自动识别 HTTP/1.1 或行级文本协议
- **HTTP 管线化**：支持 keep-alive 和请求流水线 (最大 100 个排队)
- **优雅关闭**：`std::jthread` + `std::stop_token` 协作式取消，SO_LINGER 确保数据发送完成
- **连接超时**：基于 multimap 的 O(log n) 空闲连接检测