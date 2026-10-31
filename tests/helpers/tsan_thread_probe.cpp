// Exactly two application threads: calibrate additional TSan runtime threads.
#include <cerrno>
#include <cstdio>
#include <thread>
#include <unistd.h>

int main() {
    ssize_t count = -1;
    std::thread reader([&] {
        std::puts("THREAD_PROBE_MAIN_ENTERED");
        std::fflush(stdout);
        char release = 0;
        do { count = ::read(STDIN_FILENO, &release, 1); } while (count < 0 && errno == EINTR);
    });
    reader.join();
    return count == 1 ? 0 : 1;
}
