#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <cassert>
#include "logger.h"

class ThreadPool
{
public:
    explicit ThreadPool(size_t inum_threads=std::thread::hardware_concurrency())
    {
        queue_.resize(inum_threads);
        for (size_t i = 0; i < inum_threads; ++i) {
            threads_.push_back(std::thread(&ThreadPool::WorkLoop, this, i));
        }

        assert(threads_.size() == queue_.size() && "Each thread should has it's own queue");
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    ~ThreadPool()
    {
        {
            std::lock_guard l(mu_);
            for (size_t i = 0; i < threads_.size(); i++) {

                queue_[i].push(nullptr);

                cond_.notify_all();
            }
        }
        for (auto &t : threads_) {
            t.join();
        }
		SPDLOG_INFO("All thread done");
    }

    void Schedule(std::function<void()> &&func)
    {
        assert(func != nullptr);
		static std::atomic_int idx{0};
		auto slot = (idx++) % threads_.size();

        std::lock_guard l(mu_);
        assert(slot < queue_.size());
        queue_[slot].push(std::move(func));
        cond_.notify_all();
    }

private:
    bool WorkAvailable(size_t slot) const
    {
        return !queue_[slot].empty();
    }

    void WorkLoop(size_t myslot)
    {
        while (true) {
            std::function<void()> func;
            {
                std::unique_lock l(mu_);
                cond_.wait(l, [this, myslot] { return ThreadPool::WorkAvailable(myslot); });
                func = std::move(queue_[myslot].front());
                queue_[myslot].pop();
            }
            if (func == nullptr) { // Shutdown signal.
				SPDLOG_INFO("Finish thread with index {}", myslot);
                break;
            }

            func();

        }
    }

    std::mutex mu_;
    std::condition_variable cond_;
    std::vector<std::queue<std::function<void()>>> queue_;
    std::vector<std::thread> threads_;
};
