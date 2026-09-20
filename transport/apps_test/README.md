# Application Tests

Software tests whose primary subject is a strict-C90 program under `transport/apps/` belong in this directory.

Tests that primarily exercise the kernel, system-wide QEMU behavior, or `transport/lib/` remain under the repository-level `tests/` directory.

`rawchat/` contains both the private protocol unit test and the Raw Chat end-to-end test.

The latter happens to launch QEMU, but its primary subject is the application, so it remains with the application's other tests.
