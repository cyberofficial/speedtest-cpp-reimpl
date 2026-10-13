// Loopback HTTP server used by `speedtest.exe --selftest --offline`.
// Serves the same route shapes as an Ookla speed test server so the engine
// plumbing can be exercised without internet access.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st {

class LoopbackServer {
public:
    LoopbackServer() = default;
    ~LoopbackServer();

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    bool Start(std::string* err);
    void Stop();

    std::uint16_t port() const { return port_; }
    std::string baseUrl() const;

    std::uint64_t latencyRequests() const { return latencyReqs_.load(); }
    std::uint64_t downloadRequests() const { return downloadReqs_.load(); }
    std::uint64_t downloadBytesServed() const { return downloadBytes_.load(); }
    std::uint64_t uploadRequests() const { return uploadReqs_.load(); }
    std::uint64_t uploadBytesReceived() const { return uploadBytes_.load(); }
    std::uint64_t badRequests() const { return badReqs_.load(); }
    std::uint64_t clientAborts() const { return clientAborts_.load(); }

private:
    void AcceptLoop();
    void WorkerLoop();
    void HandleConnection(std::uintptr_t sock);

    std::atomic<bool> stop_{false};
    std::uintptr_t listenSock_ = ~std::uintptr_t(0);
    std::uint16_t port_ = 0;

    std::atomic<std::uint64_t> latencyReqs_{0};
    std::atomic<std::uint64_t> downloadReqs_{0};
    std::atomic<std::uint64_t> downloadBytes_{0};
    std::atomic<std::uint64_t> uploadReqs_{0};
    std::atomic<std::uint64_t> uploadBytes_{0};
    std::atomic<std::uint64_t> badReqs_{0};
    std::atomic<std::uint64_t> clientAborts_{0};

    std::thread acceptThread_;
    std::vector<std::thread> workers_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::uintptr_t> queue_;
};

}  // namespace st
