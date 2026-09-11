#include "subprocess.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace subprocess {

namespace {

std::vector<char*> c_argv(const std::vector<std::string>& argv, std::vector<std::string>& storage) {
    storage = argv;
    std::vector<char*> ptrs;
    ptrs.reserve(storage.size() + 1);
    for (auto& a : storage) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    return ptrs;
}

} // namespace

pid_t spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;

    // Double-fork so the actual command is reparented to PID 1 (or the
    // subreaper). The immediate child exits immediately, which lets us reap it
    // synchronously here; the grandchild outlives us and never becomes a
    // zombie we own.
    int syncpipe[2];
    if (::pipe2(syncpipe, O_CLOEXEC) < 0) return -1;

    pid_t pid = ::fork();
    if (pid < 0) { ::close(syncpipe[0]); ::close(syncpipe[1]); return -1; }
    if (pid > 0) {
        // Parent: close write end, wait for immediate child to exit, then
        // read the grandchild's pid back through the pipe.
        ::close(syncpipe[1]);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        pid_t grandpid = -1;
        ssize_t n = ::read(syncpipe[0], &grandpid, sizeof(grandpid));
        (void)n;
        ::close(syncpipe[0]);
        return grandpid;
    }

    // First child.
    if (::setsid() < 0) ::_exit(127);

    pid_t grandpid = ::fork();
    if (grandpid < 0) ::_exit(127);
    if (grandpid > 0) {
        // Write the grandchild's pid back to the parent, then exit.
        ssize_t w = ::write(syncpipe[1], &grandpid, sizeof(grandpid));
        (void)w;
        ::close(syncpipe[1]);
        ::_exit(0);
    }

    // Grandchild: detach stdio and exec.
    ::close(syncpipe[0]);
    ::close(syncpipe[1]);

    int devnull_in = ::open("/dev/null", O_RDONLY);
    int devnull_out = ::open("/dev/null", O_WRONLY);
    if (devnull_in >= 0) { ::dup2(devnull_in, STDIN_FILENO); ::close(devnull_in); }
    if (devnull_out >= 0) {
        ::dup2(devnull_out, STDOUT_FILENO);
        ::dup2(devnull_out, STDERR_FILENO);
        ::close(devnull_out);
    }

    std::vector<std::string> storage;
    std::vector<char*> ptrs = c_argv(argv, storage);
    ::execvp(ptrs[0], ptrs.data());
    ::_exit(127);
}

} // namespace subprocess
