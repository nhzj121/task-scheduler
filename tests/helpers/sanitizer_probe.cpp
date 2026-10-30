#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
int main() {
    std::cout << "PROBE_MAIN_ENTERED\n" << std::flush;
    std::atomic<int> count{0};
    std::vector<int> owned(100, 7);
    std::thread producer([&] { for (const auto value : owned) count.fetch_add(value); });
    producer.join();
    return count.load() == 700 ? 0 : 1;
}
