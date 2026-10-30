# 场景服务器（sceneServer）

## 目录

- [本服务器简介](#本服务器简介)
- [构建与运行](#构建与运行)
- [配置](#配置)
- [职责边界](#职责边界)
- [约定](#约定)
- [内部组件](#内部组件)
- [测试与数据](#测试与数据)
    - [单元测试与进程测试](#单元测试与进程测试)
    - [真实数据库持久化验收](#真实数据库持久化验收)

## 本服务器简介

场景服处理玩家状态、进场、移动、退出、AOI 和存档。C++ 管理玩家对象与基础校验，Lua 处理移动等示例玩法；战斗尚未实现。

客户端通过网关访问场景服，不直接连接这里。构建和启动方法见[构建与运行](#构建与运行)。

## 构建与运行

场景服运行前需要可用的网关。构建依赖见[主 README](../../README.md#各子服务器的构建命令)，以下命令从仓库根目录执行。

```bash
cmake -S Zone -B out/zone -DSCENE_BUILD_TESTS=ON
cmake --build out/zone --target sceneServer gatewayServer -j 2
```

Lua 绑定使用 5.4/5.3 API。升级依赖后请重新生成构建配置，避免 Protobuf 生成文件与运行库混用。

从场景服目录启动以使用相对路径的 `./lua_script` 和日志目录。默认共享配置 `./config/zoneConfig.yaml` **不在此目录**，所以务必提供第一个位置参数；第二个参数显式指定场景配置：

```bash
(
  cd Zone/sceneServer &&
  ../../out/zone/bin/sceneServer \
    ../config/zoneConfig.yaml \
    ./config/sceneConfig.yaml
)
```

日志写入 `ZoneServer.log.dir` 指定的目录（相对路径基于工作目录）；发送 `SIGUSR1` 重载 `luaDir` 下脚本，`SIGTERM` 或 `SIGINT` 触发有序退出。

## 配置

共享配置管理区服与数据库连接，场景配置管理当前实例和玩法参数。

- 共享配置 `Zone/config/zoneConfig.yaml`：`zoneId`、数据库、网关地址、日志。
- 专属配置 `Zone/sceneServer/config/sceneConfig.yaml`：`sceneId/serviceId`、逻辑线程数、`luaDir`、可选监听及网络/定时器/AOI 参数。`net.listenEnable` 默认 `false`，Lua 脚本根目录默认 `./lua_script`。
- AOI 当前仅支持 `nineGrid`，`tickMs/cellSize` 必须大于零，`viewRange` 为地图坐标单位，`cellPerLogicThread` 必须为 `0`；十字链表参数不支持。共享项不能重复放入场景配置，未知配置键会报错。
- **网关地址：**共享配置默认 `gateways: 127.0.0.1:13145`，与网关 `privateConfig` 内网监听一致；`10000` 是客户端公网端口，不应用于场景服反向连接。跨机器部署须改成实际可达的内网地址，并确保 `zoneId` 一致、`serviceId=8` 位于网关 `allowedServices` 中。
- 示例 `ZoneServer.db.enable: true`；实际运行需要可用的 MySQL、Redis 及对应账号。无数据库联调可以按需禁用 DB 通道，但相关持久化功能不工作。

`SceneServer.mapPath` 可指定地图文件，默认 `map.yaml`，相对路径按场景配置文件目录解析。启动前加载地图并由 SceneServer 持有，向全部 worker 注入只读引用；地图加载失败不启动网络线程。

`luaDir` 和相对日志路径按进程工作目录解析，`mapPath` 按场景配置文件目录解析。

## 职责边界

网络线程管理连接，逻辑 worker 管理玩家和玩法状态；两者通过消息投递协作。

- 客户端只连接网关的 `ClientFrame` 端口；场景服处理服务间 `GateFrame`，不负责客户端鉴权、登录票据验证或公网接入。
- `NetServer` 持有网络连接与编解码，`LogicThread` 持有各自的 Lua VM 和玩家分片；数据库与定时器线程各司其职，跨线程只通过 `MsgBus` 投递消息。
- 按 `playerId` 路由玩家请求到所属 worker；没有玩家 ID 时按会话分配。无效的 `(module, method)` 在创建玩家 Actor 之前拒绝。
- 仅实现路由表中登记的玩法请求。进场/退出由 C++ 处理，移动/示例路径由 Lua 处理；其他未实现处理器明确返回错误。`net.listenEnable` 仅供本地联调/工具，不应作为公网客户端入口。

## 约定

- 场景服是内部服务，不直接接收客户端 `ClientFrame`。默认主动连接网关，`ConnNew/ConnClose` 表示内部 TCP 链路事件，不能等同于某个玩家上下线。
- 玩家上线/下线由网关版本化绑定信封表达；网络层校验后通过消息总线投递到玩家所属逻辑 worker。普通玩法请求只能使用已有绑定，不能隐式创建 Player。
- 当前入站玩法为进场、移动、示例路径和退出；已接入 AOI 观察者差集/全量视野推送，战斗尚未实现。
- 客户端 `ViewResyncReq` 已登记入站路由，由所属 worker 标记视野重同步；它与跨 worker owner 快照恢复不是同一协议。

## 内部组件

| 组件 | 职责 |
| --- | --- |
| `main.cc` / `ZoneConfig` / `SceneConfig` | 加载共享与场景专属配置、合并校验、初始化日志和路由表、等待信号。 |
| `SceneServer` | 组装和管理总线、网络、定时器、DB 和逻辑 worker；热更新 Lua。 |
| `NetServer` | 网关反向连接、可选本地监听、握手及 `GateFrame` 收发。 |
| `MsgBus` | 多条 worker 队列、网络发送队列、MySQL 队列与 Redis 分片队列。 |
| `LogicThread` / `PlayerManager` | 玩家 Actor 生命周期、路由校验、Lua 玩法处理；每个 worker 独占自己的 Lua VM。 |
| `RouteTable` / `LuaEnv` | `(module,method)` 与 protobuf 类型、Lua 函数的映射和调度。 |
| `DBThread` / `TimerThread` | 异步 DB 任务及定时器回投到所属逻辑线程。 |
| `PlayerService` / `AoiService` | 权威玩家位置、实体生命周期、观察者状态与 AOI tick。 |
| `WorkerPushSink` / `AoiServiceRouter` / `AoiWire` | 内部 AOI 事件编码、跨 worker 投递、可信发送方校验与快照恢复。 |

主要接口：`SceneServer::start/stop` 控制生命周期，`reloadLua` 广播热更新，`stats` 输出各 worker、定时器与 DB 快照；`MsgBus::sendToWorkerByPlayerId/sendToNet/sendToDb` 执行跨线程投递。生命周期细节见 `Zone/sceneServer/src-cpp/sceneServer.h`。


## 测试与数据

单元测试、无 DB 进程测试和真实数据库验收分别检查不同问题。

### 单元测试与进程测试

```bash
cmake --build out/zone -j 4
ctest --test-dir out/zone \
  --output-on-failure -R '^(scene_tests|scene_smoke_invalid_route|gateway_real_scene)$'
```

`scene_tests` 包含玩家绑定、AOI 路由/服务/副本/视野状态及 Lua 绑定等单元测试；`scene_smoke_invalid_route` 通过临时配置验证真实网络帧经过总线和逻辑线程后，无效路由不会创建 Actor；`gateway_real_scene` 验证真实玩家主链路。进程测试不证明外部 DB、可靠持久化或完整客户端视野协议可用。

### 真实数据库持久化验收

持久化验收不会默认加入 CTest，也不会使用 `Zone/config/zoneConfig.yaml` 中的数据库。它由脚本创建临时 MariaDB/Redis、回环端口、独立数据库和受限账号，结束时停止本次进程并删除临时数据。需要本机提供 MariaDB 初始化/服务端/客户端工具以及 Redis 服务端。

以下命令均以 `GameServer` 为根目录执行：

```bash
cmake -S Zone \
  -B out/zone
cmake --build out/zone \
  --target sceneServer gatewayServer persistence_dao_probe -j2
python3 Zone/sceneServer/tests/persistence_e2e.py \
  out/zone
```

历史验收通过的检查包括：DAO 加载/回滚/同版本重放/旧版本拒绝，非默认出生点加载，离线保存后立即重新绑定加载，在线周期快照，优雅停服最终保存，场景服重启恢复，以及复用网关连接时另一玩家断开后的独立持久化。

该验收覆盖正常时序和受控的队列/结果处理，不证明进程崩溃后内存任务不丢失、跨重启任务恢复、COMMIT 断链后的未知结果自动对账、跨网关接管、死锁不存在或所有 MySQL/MariaDB 版本兼容。若因 `SIGKILL` 或主机故障无法清理，应先确认残留进程属于本次临时目录，再手工清理，不能停止其他数据库实例。

>最新测试结果统一维护在[主 README](../../README.md)，本节保留 2026-10-19 的历史记录：
