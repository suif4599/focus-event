#include "epoll.hpp"

#include <unistd.h>
#include <sys/epoll.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace epoll {

namespace {
constexpr int MAX_EVENTS = 8;
} // namespace

struct Loop::Impl {
    int epfd = -1;
    bool stopped = false;
    // fd -> handler. Owned by the loop. Removed in del() (currently unused) or
    // by destructor cleanup. In our usage we never deregister, so this is fine.
    std::unordered_map<int, std::function<void(int)>> handlers;
};

Loop::Loop() : impl_(new Impl()) {
    impl_->epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (impl_->epfd < 0) {
        throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));
    }
}

Loop::~Loop() {
    if (impl_->epfd >= 0) ::close(impl_->epfd);
    delete impl_;
}

void Loop::add(int fd, std::function<void(int)> handler) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    int rc = ::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev);
    if (rc < 0 && errno == EEXIST) {
        rc = ::epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev);
    }
    if (rc < 0) {
        throw std::runtime_error(std::string("epoll_ctl: ") + std::strerror(errno));
    }
    impl_->handlers[fd] = std::move(handler);
}

void Loop::run() {
    epoll_event events[MAX_EVENTS];
    while (!impl_->stopped) {
        int n = ::epoll_wait(impl_->epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("epoll_wait: ") + std::strerror(errno));
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            auto it = impl_->handlers.find(fd);
            if (it != impl_->handlers.end() && it->second) it->second(fd);
        }
    }
}

void Loop::stop() { impl_->stopped = true; }

} // namespace epoll
