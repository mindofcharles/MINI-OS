# Network Library Boundary

Sources added to this directory are compiled with the modern-C library flags and are linked only into `ping`, `netcat`, and `ssh` applications.

Public headers added here must remain usable by applications compiled under strict C90.

The current platform layer supplies `net_platform.h`, wrap-safe relative-time helpers, production monotonic/random adapters, and the nonblocking cancellation callback adapter.

The implemented raw transport supplies `raw.h`, shared `raw.def` layout constants, and modern-C wrappers for the three polling NE2000 frame syscalls.

Protocol parsing remains deferred and must build above the normalized raw-frame boundary without importing hardware registers or packet-memory addresses.
