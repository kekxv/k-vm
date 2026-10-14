# Base image and transactional Pi initialization

**Goal:** Distribute a Debian RV64 shell image by default; install Node.js/Pi on demand and preserve successful installations.

**Architecture:** The guest installer owns downloads, checksums, npm installation and cache cleanup. `init` uses the existing isolated disk transaction. A disk fingerprint sidecar records successful initialization; automatic Pi startup runs an initialization subprocess only when needed, then restores the normal snapshot launch.

**Constraints:** Preserve native Win32, macOS and Linux. Never export work or credentials during initialization. Preserve existing complete Pi images and update behavior. Local packages are a read-only directory containing the Node archive, SHASUMS256.txt and a populated npm-cache.

## Steps

- [x] Add CLI tests for initialization success/failure, prohibited exports, and offline automatic-start refusal; run against the old binary to confirm missing behavior.
- [x] Add `guest/k-vm-install-pi.sh`, test its validation and checksum failure against local fixture archives before installing anything.
- [x] Split Docker stages into base and pi, make the preparation default base and add `--with-pi`; retain download checksums for complete images.
- [x] Add init launch policy and isolated guest install bundle; reuse disk commit/rollback on POSIX and Win32.
- [x] Add disk readiness fingerprint and native subprocess launch for automatic initialization; invalidate readiness when disk changes.
- [x] Run Bazel tests, real guest initialization/rollback/automatic launch checks, fresh base-image measurement, and Win32 cross compilation.
- [x] Update README and usage with initialization, local packages, persistence and measured sizes.

Host curl prefetch is an implementation improvement: native host download avoids slow simulated TLS for the Node archive, while the guest verifies SHA256 before extraction. Missing host curl falls back to guest HTTPS. Guest `/tmp` is tmpfs during maintenance so caches do not inflate persistent sparse disks. Review findings about Windows hard-link stamps and invalid-launch side effects have regression tests.

Verified: Linux Bazel 15 passing tests, 5 Windows-only skipped; musl initialization/launch tests and real guest init/update fixtures; native Win32 cross build and hard-link regression under Wine (vulnerable version failed, fixed version passed); real offline and online Node/Pi installation; idempotent initialization; Pi, Node, workspace and HTTPS runtime checks. Fresh base directory: 304 MiB, complete Pi image: 665 MiB, online initialized image: 643 MiB. Host Node archive download took about 4 seconds; online guest npm installation about 4 minutes on this host.

Useful commands: `bazel test //...`, `python3 tests/pi_update_check.py`, `python3 tools/build_win32.py`.
