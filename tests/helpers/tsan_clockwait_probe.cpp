// Standalone detector compatibility reproducer; no project code or suppression.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>

int main() {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    std::thread worker([&] {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            if (!condition.wait_for(lock, std::chrono::seconds(3), [&] { return released; }))
                std::terminate();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::puts("RELOCKED");
        }
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }))
            std::terminate();
        released = true;
        condition.notify_all();
    }
    worker.join();
}
