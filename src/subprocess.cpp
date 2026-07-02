#include "subprocess.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
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

PipeResult launch_pipe_stdout(const std::vector<std::string>& argv) {
    if (argv.empty()) throw std::runtime_error("launch_pipe_stdout: empty argv");

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) < 0) {
        throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // child
        ::close(fds[0]);
        if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(127);
        ::close(fds[1]);
        int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }

        std::vector<std::string> storage;
        std::vector<char*> ptrs = c_argv(argv, storage);
        ::execvp(ptrs[0], ptrs.data());
        // exec failed
        std::string err = std::string("execvp ") + argv[0] + ": " + std::strerror(errno);
        ssize_t w = ::write(STDERR_FILENO, err.data(), err.size());
        (void)w;
        ::_exit(127);
    }
    // parent
    ::close(fds[1]);
    return PipeResult{fds[0], pid};
}

std::string run_capture_stdout(const std::vector<std::string>& argv) {
    PipeResult pr = launch_pipe_stdout(argv);
    std::string out;
    char buf[8192];
    while (true) {
        ssize_t n = ::read(pr.read_fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            ::close(pr.read_fd);
            throw std::runtime_error(std::string("read: ") + std::strerror(errno));
        }
    }
    ::close(pr.read_fd);

    int status = 0;
    while (::waitpid(pr.pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error(std::string("waitpid: ") + std::strerror(errno));
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        throw std::runtime_error("command `" + argv[0] +
                                 "` exited with status " + std::to_string(code));
    }
    return out;
}

pid_t spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid > 0) return pid;

    // child
    if (::setsid() < 0) ::_exit(127);

    // Detach stdio from our terminal.
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
