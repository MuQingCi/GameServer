#ifndef CLEARMOON_COMMON_NETROUTE_H
#define CLEARMOON_COMMON_NETROUTE_H

#include <cstdint>

/**
 * @brief 网络分片路由：session 高 8 位编码"属于哪个网络线程"
 *
 * 出站消息（逻辑线程 -> 网络线程）只带 session，因此必须能从 session 反解出
 * 拥有该连接的网络分片，否则消息会投到错误的 epoll 线程。
 *
 * 约束：
 *   - 分片数 <= 64（kMaxThreads），高 8 位足够；
 *   - session 的低 56 位是各分片自己的计数器，保证分片内唯一；
 *   - 非法/未编码的 session（例如测试里手写的 0）一律落到 0 号分片。
 */
namespace netroute
{
constexpr unsigned kMaxThreads   = 64;
constexpr unsigned kIndexShift   = 56;
constexpr uint64_t kCounterMask  = (uint64_t(1) << kIndexShift) - 1;

inline uint64_t MakeSession(unsigned netIndex, uint64_t counter)
{
    return (static_cast<uint64_t>(netIndex) << kIndexShift) | (counter & kCounterMask);
}

inline unsigned IndexOf(uint64_t session)
{
    return static_cast<unsigned>(session >> kIndexShift);
}

inline unsigned IndexOfClamped(uint64_t session, unsigned netThreads)
{
    if (netThreads == 0)
        return 0;
    const unsigned idx = IndexOf(session);
    return idx < netThreads ? idx : 0;
}
}  // namespace netroute

#endif
