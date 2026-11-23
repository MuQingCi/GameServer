#pragma once

#include "loadStats.h"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

struct LoadClientReactorOptions
{
    std::string host = "127.0.0.1";
    uint16_t port = 10000;
    uint32_t connections = 1;
    uint32_t ioThreads = 1;
    uint32_t rpsPerConnection = 10;
    uint32_t durationSec = 10;
    uint32_t drainSec = 0;
    uint32_t warmupSec = 0;
    uint32_t idleBeforeSec = 0;
    uint32_t holdAfterSec = 0;
    uint32_t preparationTimeoutSec = 30;
    bool stagger = false;
    // 源 IP 分散：0 = 不生效。压测单机受 ip_local_port_range（本机 4096 个端口）限制，
    // 绑定 127.0.0.(2 + index%N) 可把可用五元组扩大 N 倍；仅压测脚本使用。
    uint32_t sourceIpSpread = 0;
    // 源 IP 基数：实际绑定 127.0.0.(base + index % spread)。分轮次错开可避开上一轮 TIME_WAIT。
    uint32_t sourceIpBase = 2;
    // 连接阶段重试次数（仅 TCP 建立超时）：单机万级连接时网关 acceptor 面对 SYN 突发
    // 可能丢包，重试可避免整轮准备失败；0 = 不重试（默认，保持既有语义）。
    uint32_t connectRetries = 0;
    // 准备阶段单连接超时（秒）：万级连接时网关需逐个处理 Auth/Enter，3s/5s 过紧
    uint32_t authTimeoutSec = 3;
    uint32_t enterTimeoutSec = 5;
    uint64_t playerBase = 100000;
    uint64_t ticket = 998877;
    uint16_t module = 1;
    uint16_t method = 6;
    std::string body;
    std::string output = "load-summary.json";
    bool ping = true;
    bool scene = false;
    std::string gatewayMetrics;
    std::string sceneMetrics;
    std::vector<std::string> additionalSceneMetrics;
    std::string metricsOutput;
    uint32_t metricsIntervalSec = 1;
    std::string barrierDir;
};

// 所有连接只归所属 reactor 写入；调用返回前关闭 socket 并 join 全部 I/O 线程。
bool RunLoadClientReactors(const LoadClientReactorOptions& options, LoadStats& stats,
                          std::atomic<int64_t>& startEpochMs,
                          std::atomic<uint64_t>& windowSuccessful,
                          const volatile std::sig_atomic_t& stopRequested);