# Pi on RISC-V with persistent updates

Goal: RISC-V only, shell/Pi launches, modern Debian slim RV64 image with latest stable kernel, transactional `k-vm update`.
Architecture: keep TinyEMU C emulator; image builder cross-compiles Linux/OpenSBI and constructs isolated RV64 userspace. Update boots a cloned root disk without workspace/config exports, runs npm in guest, verifies Pi, and atomically replaces base disk only after successful guest status and shutdown.

- [x] Add shell/Pi-only and update regressions before implementation.
- [x] Remove active x86 build and acceleration/Codex CLI paths.
- [x] Add private guest update status export and transactional disk cloning/commit, with locking.
- [x] Build modern RV64 kernel/firmware/Debian image and validate Pi runtime.
- [x] Verify cold boot, mappings/network, update persistence/failure preservation, default and SDL builds.
- [x] Document current versions, supported host platform and Go/backend tradeoffs.

Verification (2026-10-13): default and SDL Bazel suites each passed all 9 tests. Real RV64 Pi 1.2.0 update completed npm installation, version verification, guest poweroff and atomic disk replacement; root disk inode changed and no update clone remained. The updated image passed pi_runtime_check (Pi/Node/kernel, writable /work, npm registry HTTPS and clean shell exit). Fixture integration checks also covered update failure/interruption preservation and minimal configuration exports. No authenticated agent conversation was tested. Modern shell cold boot was about 5 seconds; the real npm update took about 5 minutes under the interpreter.
