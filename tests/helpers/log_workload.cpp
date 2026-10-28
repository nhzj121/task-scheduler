// Deterministic binary workload. Ordinary blocking writes are intentional:
// real Worker pipe backpressure must eventually block and then release them.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <unistd.h>
namespace {
bool output(int fd, std::uint64_t bytes, unsigned char mask = 0) {
    std::array<unsigned char, 4096> buffer{};
    std::uint64_t offset = 0;
    while (offset < bytes) {
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), bytes - offset));
        for (std::size_t i = 0; i < length; ++i) buffer[i] = static_cast<unsigned char>(static_cast<unsigned char>((offset + i) % 251) ^ mask);
        std::size_t done = 0;
        while (done < length) {
            const auto count = ::write(fd, buffer.data() + done, length - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            done += static_cast<std::size_t>(count);
        }
        offset += length;
    }
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const auto bytes = std::stoull(argv[2]);
    if (std::string(argv[1]) == "staged") {
        if (!output(STDOUT_FILENO, bytes)) return 3;
        const int fd = ::open(argv[3], O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 4;
        char byte = 0; const auto count = ::read(fd, &byte, 1); ::close(fd);
        if (count != 1 || !output(STDOUT_FILENO, bytes)) return 5;
    } else {
        if (!output(STDOUT_FILENO, bytes) || !output(STDERR_FILENO, std::stoull(argv[3]), 0xa5)) return 6;
    }
    return 0;
}
