#include <socketpp/watcher.hpp>

#include "support.hpp"

#include <array>

int main() {
    return example::report([] {
        sockpp::CSocketWatcher watcher;
        int callbacks = 0;
        watcher.push([&] { ++callbacks; });
        watcher.push([&] { ++callbacks; watcher.stop(); });
        watcher.run();
        std::cout << "Processed " << callbacks << " callbacks; backend=" << watcher.backend() << '\n';
    });
}
