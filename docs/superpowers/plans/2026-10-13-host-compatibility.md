# Host compatibility implementation plan

Goal: native macOS file sharing and Linux operation, with Windows integration through WSL2. Preserve minimal Pi configuration imports and transactional local image updates.
Architecture: separate Linux descriptor operations from POSIX/macOS descriptor operations behind a small internal interface. Select implementations in Bazel; run the macOS-compatible implementation on Linux for regression coverage and add native macOS CI. Windows launcher translates only host path arguments and preserves agent arguments.

- [x] Add independent backend tests for symlink/parent escapes, existing file truncation, special files and writable/read-only exports; run against the new unavailable backend to establish failure.
- [x] Extract Linux safe host IO helpers; implement macOS/POSIX backend with no-follow directory resolution, identity checks and deferred truncation.
- [x] Replace Linux-only configuration copying, image-clone, stat fields, TAP includes and linker flags with platform selections.
- [x] Add Windows WSL2 launcher with argument/path translation tests. No native Windows support claim.
- [x] Run default/SDL and portable backend suites, real Pi/workspace smoke checks; add macOS CI and document verification limits.

macOS backend must reject symlink traversal and special files. POSIX final opens use nonblocking no-follow opens, check inode/device identity and only truncate after validation. Unlike Linux O_PATH, macOS does not have a descriptor-reopen primitive; adversarial host replacement with a device between checks can still invoke its open operation. This limitation must be explicit, not represented as equivalent Linux confinement.

Verification: default and SDL suites passed all 13 test targets. Portable backend mapping/configuration/update tests passed. Real modern RV64 Pi CLI, Node, HTTPS, /work and clean exit passed on Linux. A temporary binary linked with host_io_posix passed the real /work/config exclusion/Ctrl+C check. Linux fixture update persistence, failure and interruption preservation passed. Zig cross-compiled 50 native objects for each of Darwin ARM64 and x86_64, including RV32/64/128 interpreters; OpenSSL/curl headers were supplied from the Linux installation for compilation checking, so this does not prove native dependency linking. Windows launcher passed 8 argument/path cases, including guest argument preservation, abbreviated long options, WSL-native absolute paths and update exclusion. Independent review found no blocking issues after launcher fixes. Native macOS and Windows/WSL execution remains unverified in this Linux environment; CI is supplied but has not run. No Git repository exists, so no commit/PR or workflow execution was possible.
