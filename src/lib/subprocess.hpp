// Subprocess helpers.
//
// spawn_detached(): fork+setsid+exec a program without waiting. Used to fire
//   configured actions (e.g. pactl). The child becomes its own session leader
//   so it survives our exit and isn't tied to our controlling terminal.

#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

namespace subprocess {

// Fire-and-forget spawn. Returns the child pid (or -1 on failure). The child
// detaches into its own session.
pid_t spawn_detached(const std::vector<std::string>& argv);

} // namespace subprocess
