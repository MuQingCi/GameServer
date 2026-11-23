#ifndef CLEARMOON_NET_NETSERVER_H
#define CLEARMOON_NET_NETSERVER_H

#include "base/buffer.h"
#include "common/msg.h"
#include "common/msgBus.h"
#include "base/proto.h"
#include "net/workerPendingTasks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief 网络线程配置
 *
 * 这里的每个上限都对应一类线上事故：
 *   - maxReadBuffer : 对端持续发而逻辑层消费不过来 -> 读缓冲无限增长 OOM
 *   - maxWriteBuffer: 慢客户端 + 推包量大 -> 写缓冲无限增长 OOM
 *   - maxDecodeErrors: 恶意/失配客户端 -> 无限重同步把网络线程 CPU 打满
 *   - maxFramesPerSec: 刷包攻击 -> 逻辑线程被单一连接饥饿
 */
struct NetConfig
{
    bool        listenEnable = false;         // 场景服通常不需要监听（反向连网关）
    std::string listenAddr   = "0.0.0.0";
    uint16_t    listenPort   = 12345;
    uint8_t     serviceId    = ServerID::kScene;  // 本服务 id（出站帧 srcServiceID）
    uint32_t    zoneId       = 1;             // 区服 id（握手时上报网关）
    uint32_t    sceneId      = 1;             // 场景 id（握手时上报网关）
    uint64_t    sceneIncarnation = 0;          // 进程世代，启动时生成一次
    uint32_t    backendChannels = 1;           // 每个 gateway 的静态 channel 数

    size_t   maxReadBufferBytes  = 1 * 1024 * 1024;
    size_t   maxWriteBufferBytes = 4 * 1024 * 1024;
    uint32_t maxFramesPerSec     = 1000;
    uint32_t maxDecodeErrors     = 64;
    WorkerPendingTasks::Limits pending;
};

/**
 * @brief 需要反向连接的对端（网关集群）
 *
 * 场景服启动时**主动**连接网关集群（可连多个做冗余），
 * 而不是等网关拿连接池来连。理由：响应无法与请求匹配、回程地址无法表达、
 * 应用层重试对非幂等请求（释放技能）危险。
 */
struct PeerConfig
{
    uint8_t     serviceId = ServerID::kGateway;
    uint32_t    gatewayId = 0;
    std::string addr = "127.0.0.1";
    uint16_t    port = 10000;
    uint32_t    channelId = 0;
};

/**
 * @brief 场景服网络线程（单线程 epoll + ET）
 *
 * 职责边界（越界就会产生数据竞争）：
 *   - 独占持有：fd、读/写缓冲、半包重组、会话->worker 亲和表；
 *   - 对外只通过 MsgBus 收发 Msg：其它线程 不允许 碰 Conn / fd / Buffer；
 *   - 出站消息的传输层字段（src/dst/链路类型/重试标记）由本层补齐，业务层只提供 Module/Method/seq/playerId/body
 *
 * 两条投递路径：
 *   出站：逻辑线程 MsgBus::sendToNet(MSGTYPE_SEND) -> 唤醒 eventfd -> 本线程编码落帧
 *   入站：本线程 DecodeFrame -> MsgBus::sendToWorker(会话亲和 或 玩家 Actor 亲和)
 */
class NetServer
{
public:
    // netIndex 标识本网络分片（0..netThreads-1）：决定 session 高位、net 队列下标与唤醒句柄
    NetServer(const NetConfig& cfg, MsgBus& bus, uint32_t netIndex = 0);
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool start();
    void stop();

    // 反向连接：只登记"要连哪个 peer"，真正的 dial 在**网络线程**里做。
    // 这样既不跨线程碰 conns_，也让断线后能按退避自动重连。
    bool connectTo(const PeerConfig& peer);

private:
    friend struct NetServerTestPeer;
    struct Conn
    {
        int      fd = -1;
        uint64_t session = 0;
        uint32_t gatewayId = 0;
        uint32_t channelId = 0;
        uint8_t  peerServiceID = ServerID::kServiceAny;
        uint8_t  linkType = NetMessageType::kSceneMsg;  // 出站帧的链路标识
        bool     outbound = false;           // 本端主动发起的连接
        bool     connecting = false;         // 非阻塞 connect 尚未完成
        bool     peerClosed = false;         // 对端已断，只允许把残余写完
        bool     writing = false;            // 是否已关注 EPOLLOUT

        uint32_t decodeErrors = 0;
        uint64_t rateWindowMs = 0;           // 帧速率统计窗口
        uint32_t framesInWindow = 0;

        Buffer sendBuffer;
        Buffer readBuffer;
    };

    void run();
    void wakeup();                                   // 唤醒 epoll（eventfd）
    void drainWakeup();

    void acceptNewConn();
    void onPeerReadable(Conn& conn);
    void onPeerWritable(Conn& conn);                 // 连接建立完成
    void onFrame(Conn& conn, const Header& head, const std::string& body);
    void handleHandshake(Conn& conn, const std::string& body);
    void processBusMsg();

    void sendTo(Conn& conn, const Msg& m);           // 编码成完整帧后追加并尝试立即写
    void doWrite(Conn& conn);                        // 唯一的写路径
    void enableWrite(Conn& conn, bool on);
    void handleClose(Conn& conn);                    // 会从 conns_ 中删除，调用方必须立即 return

    bool registerConn(Conn&& conn, uint32_t events);
    MsgBus::WorkerId workerForSession(uint64_t session, bool assignIfMissing, uint64_t playerId);

    // ---- 反向连接与自动重连（仅网络线程）----
    struct PeerState
    {
        PeerConfig peer;
        int64_t    nextAttemptMs = 0;   // 下次允许尝试的时间（NowMs 口径）
        uint32_t   attempts = 0;        // 连续失败次数，用于退避
        bool       connected = false;   // 已有可用连接（握手完成前仍视为未连通）
        bool       dialing = false;     // 已发起、尚未完成，避免重复 dial
    };
    bool dialPeer(const PeerConfig& peer);              // 真正建连（仅网络线程）
    void maintainPeers();                               // 消费登记意图 + 退避重连
    void onPeerGone(uint32_t gatewayId, uint32_t channelId, bool wasConnected);
    static int64_t peerBackoffMs(uint32_t attempts);
    int  peerWaitMs(int currentTimeoutMs) const;        // 把 epoll 超时收敛到最近的重连时刻

    void sendRetTip(const Msg& request, int32_t code, const char* text);

    MsgBus* m_bus_ = nullptr;
    NetConfig cfg_;

    std::atomic<bool> started_{false};
    std::thread thread_;

    int epFd_ = -1;
    int listenFd_ = -1;
    int wakeupFd_ = -1;

    std::atomic<uint64_t> nextSession_{1};
    uint32_t netIndex_ = 0;

    // 以下成员只允许网络线程访问
    std::unordered_map<uint64_t, Conn> conns_;
    GatewayClientIndex clients_;
    // 反向连接状态：peers_ 只允许网络线程访问；pendingPeers_ 是主线程的登记队列
    std::vector<PeerState> peers_;
    std::mutex peerMutex_;
    std::vector<PeerConfig> pendingPeers_;

    WorkerPendingTasks pendingTasks_;
    void submitPlayerTask(const Msg& message, bool lifecycle);
    uint64_t nextPendingWarningUs_=0;
    void retryClientNotifications();
    std::unordered_map<uint64_t, MsgBus::WorkerId> sessionWorker_;
    std::unordered_map<uint32_t, std::vector<Msg>> pendingLivecycle_;
    const size_t kMaxPendingTask = 1024;
};

#endif
