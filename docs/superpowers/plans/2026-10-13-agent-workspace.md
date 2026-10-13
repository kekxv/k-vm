# Agent workspace launch

Goal: `k-vm sandbox --agent shell|codex|pi` exports cwd as writable /work, imports only selected agent config files, and executes chosen guest program.

Architecture: separate workspace policy module exports work + a private, readonly launch bundle. Guest PID1 mounts VirtIO 9P exports, configures network, copies configuration into guest-local home, changes to /work and runs the selected process. A guest bootstrap is installed into images during preparation. Existing raw VM mode stays compatible.

- [x] Test selected config allowlist and generated launch bundle; never export all HOME or agent history directories.
- [x] Add workspace policy and sandbox options, default image discovery via K_VM_IMAGE / user data directory.
- [x] Install guest bootstrap into prepared images; keep image files unchanged during launch.
- [x] Provide image preparation for modern x86_64 agent runtime and offline existing-image installation.
- [x] Validate real shell cwd, rw project, readonly config source, config exclusions, and Ctrl+C behavior.
- [x] Document defaults, agent args, supported images and host dependencies; run full regression checks.

Verification: Bazel default and SDL suites (7 targets) passed; real RISC-V guest verified automatic /work, writes, shell config exclusions, Ctrl+C, SLIRP HTTP, and base-disk preservation. Fixture Codex/Pi programs verified exact config file sets, argument forwarding, and return to shell. Modern image build verified codex-cli 0.162.1 and Pi 1.2.0. Real modern agent boot and x86 initrd guest tests remain unverified on this host because /dev/kvm is absent; KVM-dependent tests are skipped. No claim of full modern agent runtime validation.
