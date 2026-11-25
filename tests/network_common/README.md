# Shared Network Test Backend

`deterministic_backend.c` and `deterministic_backend.h` provide host-only network test facilities shared by the Ethernet, ARP, IPv4, ICMP, and TCP-foundation suites.

Stage-specific assertions remain under `tests/network_phase_d/` and `tests/network_phase_e/`, while shared controls use the `network_test_backend_*` prefix and `NETWORK_TEST_BACKEND_QUEUE_CAPACITY` constant.

The backend replaces raw-frame and network-platform entry points with copied transmit capture, scheduled receive frames and errors, deterministic time and random bytes, cancellation, and fake-clock idle advancement.

Clock reads normally leave time unchanged, while an optional clock-read step advances before each read to reproduce deadlines crossed inside task startup rather than only during receive operations.

Zero disables that step, and backend reset also clears it so existing tests retain their original timing behavior.

Transmit history is bounded and explicitly drainable, and receive events are consumed only by a receive operation rather than by idle advancement.

Tests must link this backend instead of the production `raw.c` and `platform.c` adapters, and must not link both implementations into one executable.

Its deterministic random bytes are test data, not a production entropy source, and fake idle does not implement or validate target hardware wakeup.

The single `MINI_OS_NETWORK_DETERMINISTIC_BACKEND_ONLY` marker must appear in the D and E host-test executables and must not appear in the production image, as checked by `tests/test_build.py`.

The same regression checks that changes to the shared header rebuild every consuming host and sanitizer test.

These helpers are private test infrastructure and do not extend the C90 application API or the kernel syscall ABI.
