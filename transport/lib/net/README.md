# Network Library Boundary

Sources added to this directory are compiled with the modern-C library flags and are linked only into `ping`, `netcat`, and `ssh` applications.

Public headers added here must remain usable by applications compiled under strict C90.

The current platform layer supplies `net_platform.h`, wrap-safe relative-time helpers, production monotonic/random adapters, and the nonblocking cancellation callback adapter.

Protocol implementation begins after the raw-frame driver exists.
