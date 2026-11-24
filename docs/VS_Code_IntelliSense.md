# VS Code IntelliSense

If `make` succeeds but VS Code marks headers or symbols as missing, or if the IntelliSense provider list only shows `(none)`, start here.

Open the MINI-OS repository root in VS Code with the Microsoft C/C++ and [Makefile Tools](https://github.com/microsoft/vscode-makefile-tools) extensions installed. Prepare the build dependencies if needed. See [Build and Run](Build_and_Run.md#3-build-commands).

- With the default build target (`all`), run `Makefile: Clean Configure` from the Command Palette. This collects the compiler flags without running `make clean`.
- Once configuration finishes, run `C/C++: Change Configuration Provider...` and select **Makefile Tools**.

If the provider list only shows `(none)`, run **Clean Configure** first. If Makefile Tools is still missing, check its channel in the **Output** panel for a configuration error.

To refresh automatically, enable `makefile.configureOnOpen` in workspace settings and leave `makefile.configureOnEdit` enabled. If diagnostics are stale after switching branches or adding source files, run **Clean Configure** again. See the [Makefile Tools settings reference](https://github.com/microsoft/vscode-makefile-tools/blob/main/docs/makefile-settings.md#settings-reference).

If the build succeeds but the editor reports a missing `mbedtls/mbedtls_config.h`, refresh IntelliSense. MINI-OS selects its own Mbed TLS configuration through a compiler flag. The editor needs the build's flags, so there is no need to add the upstream default header or change the C standard.
