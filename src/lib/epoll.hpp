// Thin epoll wrapper. The loop drives a single fd (the stdout of `niri msg`).
// Subscribers receive a callback whenever the fd is readable; they own their
// own read buffer.

#pragma once

#include <cstdint>
#include <functional>

namespace epoll {

class Loop {
public:
    Loop();
    ~Loop();
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    // Register an fd for level-triggered EPOLLIN. handler is invoked each time
    // the fd becomes readable. Returns the epoll fd (for advanced uses).
    void add(int fd, std::function<void(int)> handler);

    // Run forever until stop() is called or no fds remain registered.
    void run();

    void stop();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace epoll
