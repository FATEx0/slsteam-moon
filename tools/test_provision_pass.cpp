#include "feats/provision_pass.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

int main()
{
    std::mutex passMutex;
    AppInfoProvision::ProvisionPassCoordinator coordinator(passMutex);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    std::thread network([&] {
        coordinator.network([&] {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        });
    });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    if (!entered.load(std::memory_order_acquire))
    {
        std::fprintf(stderr, "network phase did not start\n");
        release.store(true, std::memory_order_release);
        network.join();
        return 1;
    }

    if (!passMutex.try_lock())
    {
        std::fprintf(stderr, "pass mutex is held during network phase\n");
        release.store(true, std::memory_order_release);
        network.join();
        return 1;
    }
    passMutex.unlock();

    release.store(true, std::memory_order_release);
    network.join();
    std::puts("provision pass lock-scope tests passed");
    return 0;
}
