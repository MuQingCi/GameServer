# 网关服务器（gatewayServer）

## 目录

- [本服务器简介](#本服务器简介)
- [构建与运行](#构建与运行)
- [职责边界](#职责边界)
- [功能与协议](#功能与协议)
- [内部组件](#内部组件)
- [内部消息处理流程](#内部消息处理流程)
- [测试](#测试)
- [常见排障与交付边界](#常见排障与交付边界)
- [事件驱动压测客户端](#事件驱动压测客户端)
- [小规模内存生命周期诊断](#小规模内存生命周期诊断)

## 本服务器简介

网关负责客户端接入、会话管理和请求转发，不执行玩法，也不提供正式登录服务。

| 入口 | 默认地址 | 协议 |
| --- | --- | --- |
| 客户端 | `127.0.0.1:10000` | `ClientFrame` |
| 后端服务 | `127.0.0.1:13145` | `GateFrame` |

两种协议不能混用。下文图示说明数据如何流转，路径以 `GameServer` 为根目录，构建和测试命令在根目录执行，运行示例会注明工作目录。

## 构建与运行

依赖与公共配置见[主 README](../../README.md#各子服务器的构建命令)。以下命令在仓库根目录执行。

网关配置为 `Zone/gatewayServer/config/gatewayConfig.yaml`。`publicConfig` 只接客户端，`privateConfig` 只接后端；`allowedServices` 必须显式配置，`defaultServiceId` 也必须位于白名单中。`maxClientSessions`、`maxPendingRoutes`、`routeTtlMs` 控制容量与清理。

默认本机配置已对齐：`publicConfig` 的客户端入口为 `127.0.0.1:10000`，`privateConfig` 的后端入口为 `127.0.0.1:13145`；共享配置 `Zone/config/zoneConfig.yaml` 的 `ZoneServer.gateways` 指向后者。跨机器部署仍需将内网监听和网关列表改为可达的内网地址，并限制内网访问。

默认 `zoneId=1`、`gatewayId=1`、`clientToken=998877`、`defaultServiceId=8`，白名单仅含 `8`。要运行聊天/全局服务，须保留 `8` 并加入 `3`、`4`，且使用分行列表；轻量 YAML 解析器不支持 `[8, 3, 4]`：

```yaml
allowedServices:
    - 8
    - 3
    - 4
```

从仓库根目录构建，运行时切换到网关目录。默认配置路径是相对当前工作目录的 `./config/gatewayConfig.yaml`：

```bash
cmake -S Zone \
  -B out/zone -DSCENE_BUILD_TESTS=ON
cmake --build out/zone --target gatewayServer -j 4
(
  cd Zone/gatewayServer &&
  ../../out/zone/bin/gatewayServer ./config/gatewayConfig.yaml
)
```

可以把配置文件路径作为第一个参数传入（相对进程工作目录解析）。日志在运行时生成；当前 `SIGTERM` 直接终止网关进程，优雅停服待实现

## 职责边界

网关校验连接和会话身份，后端负责业务处理。以下约束决定请求是否可以转发：

- 网关负责连接、会话、服务注册和转发；不执行场景玩法逻辑，不解析业务 protobuf，也不直接访问玩家数据库
- 公网和内网分别使用独立的端口、连接角色与帧魔数；收到另一套协议的魔数会断开，拒绝混用
- 玩家身份取自鉴权后的会话，客户端业务帧不能指定可信玩家或任意目标地址。`BindService` 只能绑定白名单内的服务；预定义的聊天/全局模块会定向路由，其余业务按会话目标服务转发。
- 当前 `clientToken` 是开发期共享票据，不是正式登录令牌。设为 `0` 会关闭票据校验，**仅限联调**。

## 功能与协议

网关按客户端控制帧、业务请求和后端响应分别处理消息。

- **面向客户端**：公网端口接收 `ClientFrame`，处理鉴权、绑定目标服务、Ping/Pong 和业务请求；维护会话与玩家身份。
- **面向后端**：内网端口接受场景服、聊天服、全局服的反向连接，验证握手、区服 ID 和服务号白名单。
- **双向转发**：将客户端请求转成 `GateFrame`；依据回程路由返回响应，或按玩家 ID 转发后端推送。
- **场景玩家绑定**：向服务号 `8` 发送 ONLINE/OFFLINE 和版本化绑定信封；首次声明提交 ONLINE，稳定绑定后的普通业务不再重复声明。


## 内部组件

| 组件 | 职责 |
| --- | --- |
| `main.cpp` / `GatewayConfig` | 加载并校验配置，初始化日志、主事件循环及网关。 |
| `GatewayServer` / `Acceptor` | 两个监听器、连接索引、IO 线程池及定期路由清理。 |
| `TcpConnection` | TCP 缓冲、连接角色和连接级流量状态。 |
| `GatewayDispatcher` | 两种帧的处理、鉴权、握手、转发、回包和统计。 |
| `ClientSessionTable` | 会话、玩家绑定、目标服务及顶号。 |
| `ServiceRegistry` | 待注册后端及服务号到连接的映射。 |
| `ReturnRouteTable` | 内部 `seq` 到原客户端请求的映射；容量、TTL 与会话删除。 |

核心接口：`GatewayServer::start/stop` 管理传输层；`GatewayDispatcher::OnClientConnected/Closed`、`OnBackendConnected/Closed` 维护状态；`OnClientMessage/OnBackendMessage` 处理帧，`SweepRoutes` 清理超时请求。`GatewayServer` 在主 `baseLoop` 维护连接索引；连接数据在所属 IO 线程的回调中分发。

## 内部消息处理流程

从启动到断开连接，消息按以下顺序处理。

1. `main.cpp` 加载配置，启动两个 `Acceptor` 及 IO 线程池；按端口赋予 Client/Backend 角色。
2. 收到数据先检查魔数和角色，再按对应帧格式解码，并执行读缓冲、解码错误和帧速率限制。
3. 客户端控制帧完成鉴权、绑定或 Pong；业务帧检查会话与后端，记录有界回程路由后转发。
4. 后端握手后注册；响应由内部序号查回程路由，推送按玩家找会话；连接断开或 TTL 到期时清理状态。

## 测试

测试目标覆盖协议、路由和服务交互，需要 Python 3 及相应后端可执行文件。

```bash
cmake --build out/zone -j 4
ctest --test-dir out/zone \
  --output-on-failure -R '^gateway_'
```

`gateway_tests` 验证协议、配置与路由表；`gateway_smoke` 验证双端口；`gateway_e2e` 使用模拟后端；`gateway_real_scene` 使用真实场景进程；`gateway_chat_global` 验证聊天/全局联调。测试用临时配置，不验证正式鉴权或外部数据库。



仅重跑等待策略局部测试时，从仓库根目录执行（以下不启动负载）：

```bash
cmake --build out/zone --target load_poll_wait_test -j 2
ctest --test-dir out/zone \
  -R '^load_poll_wait_test$' --output-on-failure
```

## 常见排障与交付边界

| 现象 | 检查项 |
| --- | --- |
| 后端注册失败 | 连接的是内网端口、zoneId 一致、白名单含对应服务、握手身份正确 |
| 客户端业务被拒绝 | 已 Auth、Request 类型正确、requestId 非零、目标后端已注册、路由容量未满 |
| 响应被丢弃 | seq 是否有效、来源与 playerId/module/method 是否匹配、会话 epoch 是否过期 |
| 玩家进不了场 | AuthAck 不等于进场；还需发送场景 `3/4` 的 EnterSceneReq |
| 聊天无推送 | 展示名/body 格式、目标是否在已配置网关在线、聊天进程链路是否存活 |

排障时注意：TTL 只清理路由，send 成功只表示本地提交，聊天 `accepted` 也不代表对方收到或已读。

## 事件驱动压测客户端

`gateway_load_client` 已统一使用 Linux 非阻塞 socket + 水平触发 epoll，不再提供“一连接一线程”执行路径。新增 `--io-threads N`，默认 1，实际线程数为 `min(N, connections)`；保留主线程和按需启用的独立指标采样线程。每个连接仍有独立 TCP、身份、请求号、坐标和在途请求。

实现入口：`Zone/gatewayServer/tests/loadClientReactor.cc`。状态机负责认证、进场、START/ABORT、预热、业务收发、推送、排空、最终 AOI 屏障和登出。

| 项目 | 限制 |
| --- | --- |
| 准备阶段 | 每个 I/O 线程最多同时推进 64 个连接；已准备连接继续接收推送，不逐连接阻塞等待 |
| 连接缓冲 | 输入最多 4 MiB，待写最多 1 MiB；超限报错，不重连 |
| 每轮每连接预算 | 最多生成 16 个请求、解码 64 帧，收发各 64 KiB |
| 在途请求 | Scene 单请求；ping/通用 request 可多请求 |
| 超时 | 连接/认证各 3 秒，进场/登出各 5 秒，发送无进展 2 秒，业务响应 5 秒 |

收到 SIGINT/SIGTERM 后停止生成请求，按 `--drain` 限时排空。等待 START 受 `--preparation-timeout` 限制。周期采样只读取计数；最终 JSON 和延迟样本仍计算精确分位数。

>真实服务测试见[主 README](../../README.md)：

仅构建客户端并运行相关局部验证：

```bash
cmake -S Zone -B out/zone -DSCENE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build out/zone \
  --target gateway_load_client gateway_load_schedule_tests -j2
ctest --test-dir out/zone \
  -R '^gateway_load_(schedule_tests|reactor_behavior|client_behavior)$' --output-on-failure
```

## 小规模内存生命周期诊断

压测客户端提供两个可选阶段，参数均默认 0，范围 0～300 秒：

| 参数 | 行为 |
| --- | --- |
| `--idle-before SEC` | 进场/START 后保持在线，不发送业务；结束后开始预热和正式测试 |
| `--hold-after SEC` | 业务排空后保持在线、接收推送；结束后执行最终 AOI 屏障和登出 |

启用任一参数后，各连接的阶段时间追加到 `<output>.phases.jsonl`，单位为单调时钟毫秒。请使用新的输出目录。正式指标的 elapsedMs 仍从正式业务开始计算，不包含 idle 阶段。

保护脚本：`Zone/sceneServer/tests/scene_resource_guard.sh`。它只用于隔离的小规模诊断，不能代替容量验收。

LoginServer 尚未实现，规划职责是账号认证、登录凭证、区服列表和角色归属查询；当前网关未接入正式登录。区服关系见[项目架构](../../README.md)。
