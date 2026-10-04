# SSH Library Boundary

Sources added to this directory are compiled with the modern-C library flags and are linked only into the `ssh` application.

The `ssh` application also receives the network-library objects and the deliberately maintained compiler-support object.

Public SSH headers must expose a strict-C90-compatible interface even when the implementation and imported dependencies use modern C.
