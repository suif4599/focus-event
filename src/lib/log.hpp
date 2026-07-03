// Compile-time log gating.
//
// Three levels, each a strict superset of the lower:
//   error (1) — failures, rejections, malformed input. Always on by default.
//   info  (2) — + spawn dispatch ("spawn (from pid=N): cmd"), exit status
//               lines, normal child output. Useful for seeing what the daemon
//               is doing without flooding the journal.
//   debug (3) — + per-connection lifecycle, env snapshot details, hello
//               messages. Loud; use only when diagnosing.
//
// The level is fixed at compile time via -DLOG_LEVEL=N so that strings the
// macro would emit aren't even linked into the binary at lower levels. The
// NixOS module wires this through a CMake cache variable
// (`-DLOG_LEVEL=error|info|debug`).
//
// Usage:
//   LOG_ERROR("executor: failed: " << reason << "\n");
//   LOG_INFO ("executor: spawn: " << cmd << "\n");
//   LOG_DEBUG("executor: env PATH=" << path << "\n");
//
// The argument is a streamed expression; it's only evaluated if the level is
// enabled, so calls with side-effecting arguments are safe.

#pragma once

#include <iostream>

#ifndef LOG_LEVEL
#define LOG_LEVEL 1
#endif

#if LOG_LEVEL >= 1
#define LOG_ERROR(x) do { std::cerr << x; } while (0)
#else
#define LOG_ERROR(x) do { (void)0; } while (0)
#endif

#if LOG_LEVEL >= 2
#define LOG_INFO(x) do { std::cerr << x; } while (0)
#else
#define LOG_INFO(x) do { (void)0; } while (0)
#endif

#if LOG_LEVEL >= 3
#define LOG_DEBUG(x) do { std::cerr << x; } while (0)
#else
#define LOG_DEBUG(x) do { (void)0; } while (0)
#endif
