# Linux release build plan

Goal: reproducible Linux x86_64/aarch64 artifacts using hermetic_cc_toolchain 4.3.0, fixed glibc 2.28 or static musl; preserve native system builds and document quick start.

- [x] Add opt-in hermetic toolchains and local-only build profile, retaining guest SLIRP networking.
- [x] Build and inspect glibc and musl artifacts; run native tests and guest checks.
- [x] Add pull-request/push artifact CI and tag release publishing with checksums.
- [x] Move detailed usage to docs, write concise Chinese README, initialize clean tracked source repository.

Release profile excludes legacy host HTTP resources to avoid target-specific curl/OpenSSL dependencies; guest HTTPS/Pi remain enabled. No remote repository supplied: initialize locally, do not push.

Verification: all four hermetic release configurations built and passed ELF/package checks; amd64 glibc and musl each passed 9 core test targets. Both libc paths passed reference guest mapping/network/update checks. System-toolchain suite passed 13 tests (4 Windows-only skipped). ARM64 static executable help ran with qemu-aarch64-static; native ARM64 execution remains for CI. actionlint and buildifier passed. Remote GitHub workflow/release was not run because no remote repository was provided.
