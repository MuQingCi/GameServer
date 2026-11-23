#ifndef CLEARMOON_COMMON_QUEUE_H
#define CLEARMOON_COMMON_QUEUE_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <thread>

/**
 * @brief 阻塞MPSC消息队列封装

语义约束（必须遵守）：
 *   - 多生产者：任意线程可调用 try_push / try_push_bulk
 *   - 单消费者：try_pop / wait_pop / wait_pop_for / try_pop_bulk
 *     只能由 [唯一] 消费者线程调用
 * 
 */
template<typename T>
class MsgQueue
{
public:
    // maxDepth == 0 表示"无界"（保持既有行为）；>0 时超过上限的 try_push 返回 false，
    // 使 MsgBus 的 kBackpressure 分支真正可达。
    // 说明：只在有界时才维护深度计数，避免给无界热路径增加原子竞争。
    explicit MsgQueue(size_t initialCapacity = 1024, size_t maxDepth = 0)
        : queue_(initialCapacity),                 // 1. 底层队列
          consumerToken_(queue_),                   // 2. 消费者 token（单消费者，可放内部）
          maxDepth_(maxDepth)
    {}
     
    // ---- 生产者侧接口 ----
    // 需要调用方传入自己线程持有的 ProducerToken
    bool try_push(moodycamel::ProducerToken& ptok, T&& msg) 
    {
        if (!reserve(1)) return false;
        if (!queue_.try_enqueue(ptok, std::move(msg))) { unreserve(1); return false; }
        return true;
    }
    bool try_push_bulk(moodycamel::ProducerToken& ptok, T* msgs, size_t n) 
    {
        if (!reserve(n)) return false;
        const size_t pushed = queue_.try_enqueue_bulk(ptok, msgs, n);
        unreserve(n - pushed);
        return pushed == n;
    }

    // 便捷接口：不带 Token。
    // moodycamel 的隐式生产者路径在多生产者竞争下可能**瞬时**入队失败（块池竞争），
    // 这并不等于"队列已满"。上层（MsgBus::sendToWorker/sendToNet）会把 false 当成
    // 背压并丢弃消息，于是一次瞬时失败就变成一次业务失败（实测 10k 并发下 1,521 条）。
    // 这里做有限次让出重试：真满/真无内存时仍会返回 false。
    // 说明：moodycamel 保证 try_enqueue 失败时**不会移动**参数，因此可以安全重试。
    bool try_push(T&& msg) 
    {
        if (!reserve(1)) return false;
        constexpr int kMaxAttempts = 8;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            if (queue_.try_enqueue(std::move(msg)))
                return true;
            std::this_thread::yield();
        }
        unreserve(1);
        return false;
    }

    // ---- 消费者侧接口（只能在唯一的消费者线程调用）----
    bool try_pop(T& out) 
    {
        if (!queue_.try_dequeue(consumerToken_, out)) return false;
        unreserve(1);
        return true;
    }
    size_t try_pop_bulk(T* out, size_t maxN) 
    {
        const size_t popped = queue_.try_dequeue_bulk(consumerToken_, out, maxN);
        unreserve(popped);
        return popped;
    }

    void wait_pop(T& out) 
    {
        queue_.wait_dequeue(consumerToken_, out);
        unreserve(1);
    }

    bool wait_pop_for(T& out, std::chrono::milliseconds timeout) {
        const bool ok = queue_.wait_dequeue_timed(
            consumerToken_, out, std::chrono::duration_cast<std::chrono::microseconds>(timeout));
        if (ok) unreserve(1);
        return ok;
    }


    bool empty() const { return queue_.size_approx() == 0; }

    size_t current_size() const { return queue_.size_approx(); }

    // 近似深度：有界时用精确计数，无界时退回近似值（仅用于观测）
    size_t depth() const
    {
        return maxDepth_ == 0 ? queue_.size_approx() : depth_.load(std::memory_order_relaxed);
    }
    size_t maxDepth() const { return maxDepth_; }

    moodycamel::ProducerToken createProducerToken() 
    {
        return moodycamel::ProducerToken(queue_);
    }
private:
    bool reserve(size_t n)
    {
        if (maxDepth_ == 0) return true;
        if (depth_.load(std::memory_order_relaxed) + n > maxDepth_) return false;
        depth_.fetch_add(n, std::memory_order_relaxed);
        return true;
    }
    void unreserve(size_t n)
    {
        if (maxDepth_ == 0 || n == 0) return;
        depth_.fetch_sub(n, std::memory_order_relaxed);
    }

    ////非阻塞队列成员
    // moodycamel::CncurrentQueue<T> queue_;
    moodycamel::BlockingConcurrentQueue<T> queue_;
    moodycamel::ConsumerToken consumerToken_;
    const size_t maxDepth_ = 0;
    std::atomic<size_t> depth_{0};
};
#endif