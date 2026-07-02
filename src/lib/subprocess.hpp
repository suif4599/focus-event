// Subprocess helpers.
//
// launch_pipe_stdout(): fork+exec a program with its stdout captured to a pipe
//   read end (returned). The caller owns the fd and is responsible for reaping
//   the child via SIGCHLD handling (we install a no-op SIGCHLD reap inside
//   main()).
//
// run_capture_stdout(): fork+exec a program, read its entire stdout into a
//   string, wait for exit, and return the captured bytes. Used for one-shot
//   `niri msg -j windows` calls.
//
// spawn_detached(): fork+setsid+exec a program without waiting. Used to fire
//   configured actions (e.g. pactl). The child becomes its own session leader
//   so it survives our exit and isn't tied to our controlling terminal.

#pragma once

#include <string>
#include <vector>

namespace subprocess {

struct PipeResult {
    int read_fd = -1;   // caller-owned; close when done
    pid_t pid = -1;
};

// Launch argv[0]...argv[N-1] with stdout connected to a pipe. Returns the read
// end of the pipe. stderr is inherited; stdin is /dev/null.
PipeResult launch_pipe_stdout(const std::vector<std::string>& argv);

// Run argv to completion, capturing stdout. Returns the captured bytes.
// Throws std::runtime_error on fork/exec failure or non-zero exit.
std::string run_capture_stdout(const std::vector<std::string>& argv);

// Fire-and-forget spawn. Returns the child pid (or -1 on failure). The child
// detaches into its own session.
pid_t spawn_detached(const std::vector<std::string>& argv);

} // namespace subprocess
