# RISC-V only Implementation Plan

Goal: expose only RV32/RV64/RV128 machines, with no KVM requirement or x86 image preparation target.
Architecture: retain upstream x86 sources as reference while removing build targets, registration and CLI acceleration options. Preserve working RISC-V shell and agent dispatch, without claiming the old BusyBox image contains modern agents.

- [x] Add rejection tests for pc images and removed acceleration CLI options; run red test.
- [x] Remove x86 dependency, CONFIG_X86EMU, pc class registration and acceleration options.
- [x] Retire x86 agent image preparation target; document existing image versions and modern RISC-V agent limitations.
- [x] Run default/SDL suites and real RISC-V shell, dispatch and network checks.

No Git repository exists; no commits or worktrees are applicable.
