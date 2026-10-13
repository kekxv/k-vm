#!/usr/bin/env python3
"""Run the Linux k-vm binary from Windows using WSL2, without a shell boundary."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PATH_OPTIONS = {"--image", "--config-dir", "--map"}
VALUE_OPTIONS = PATH_OPTIONS | {"--agent", "--network", "--disk-mode", "--memory", "--append", "--version"}
FLAG_OPTIONS = {"--help", "--no-workspace"}


def canonical_option(key):
    if not key.startswith("--") or key in VALUE_OPTIONS | FLAG_OPTIONS:
        return key
    matches = [option for option in VALUE_OPTIONS | FLAG_OPTIONS if option.startswith(key)]
    if len(matches) > 1:
        raise ValueError(f"ambiguous option: {key}")
    return matches[0] if matches else key


def host_path(path, translate):
    # Explicit Linux absolute paths identify files inside the selected WSL distro.
    return path if path.startswith("/") else translate(path)


def wsl_prefix(distro):
    return ["wsl.exe"] + (["--distribution", distro] if distro else [])


def split_options(args):
    if "--" in args:
        index = args.index("--")
        return args[:index], args[index:]
    return list(args), []


def options(args):
    """Respect values that happen to look like option names."""
    index = 1
    while index < len(args):
        token = args[index]
        key, separator, value = token.partition("=")
        key = canonical_option(key)
        if key in VALUE_OPTIONS:
            if not separator:
                index += 1
                if index == len(args):
                    raise ValueError(f"{key} requires a value")
                value = args[index]
            yield key, value
        elif key in FLAG_OPTIONS:
            yield key, True
        index += 1


def translate_arguments(args, translate):
    before, after = split_options(args)
    result = before[:1]
    index = 1
    while index < len(before):
        token = before[index]
        key, separator, value = token.partition("=")
        key = canonical_option(key)
        if key not in VALUE_OPTIONS:
            result.append(key + ("=" + value if separator else ""))
            index += 1
            continue
        if not separator:
            index += 1
            if index == len(before):
                raise ValueError(f"{key} requires a value")
            value = before[index]
        if key in PATH_OPTIONS:
            if key == "--map":
                tag, equal, path = value.partition("=")
                if not equal or not path:
                    raise ValueError("mapping syntax: TAG=HOST_PATH[:ro|rw]")
                suffix = path[-3:] if path.endswith((":ro", ":rw")) else ""
                if suffix:
                    path = path[:-3]
                value = tag + "=" + host_path(path, translate) + suffix
            else:
                value = host_path(value, translate)
        if separator:
            result.append(key + "=" + value)
        else:
            result.extend([key, value])
        index += 1
    return result + after


def prepare_arguments(args, environ, home, translate):
    before, after = split_options(args)
    selected = dict(options(before))
    if args[0] in ("sandbox", "update") and "--help" not in selected:
        if "--image" not in selected:
            image = environ.get("K_VM_IMAGE") or str(Path(environ.get("XDG_DATA_HOME", home / ".local/share")) / "k-vm/images/pi-riscv64")
            before.extend(["--image", image])
        if args[0] == "sandbox" and selected.get("--agent") == "pi" and "--config-dir" not in selected and "--no-workspace" not in selected:
            config = environ.get("PI_CODING_AGENT_DIR") or str(home / ".pi/agent")
            before.extend(["--config-dir", config])
    return translate_arguments(before + after, translate)


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if os.name != "nt":
        print("k-vm WSL launcher must be run on Windows; use the native binary on Linux/macOS", file=sys.stderr)
        return 2
    if not args:
        args = ["sandbox"]
    prefix = wsl_prefix(os.environ.get("K_VM_WSL_DISTRO"))

    def translate(path):
        absolute = str(Path(path).expanduser().resolve())
        result = subprocess.run(prefix + ["--exec", "wslpath", "-a", "-u", absolute],
                                check=True, stdout=subprocess.PIPE, encoding="utf-8")
        return result.stdout.rstrip("\r\n")

    temporary = None
    try:
        environment = dict(os.environ)
        before, _ = split_options(args)
        selected = dict(options(before))
        if args[0] == "sandbox" and selected.get("--agent") == "pi" and "--config-dir" not in selected and not environment.get("PI_CODING_AGENT_DIR") and not (Path.home() / ".pi/agent").exists():
            # Do not fall back to a different user's configuration inside WSL.
            temporary = tempfile.TemporaryDirectory(prefix="k-vm-empty-config-")
            environment["PI_CODING_AGENT_DIR"] = temporary.name
        guest_args = prepare_arguments(args, environment, Path.home(), translate)
        command = prefix + ["--cd", translate(os.getcwd()), "--exec",
                            os.environ.get("K_VM_WSL_BINARY", "/usr/local/bin/k-vm")] + guest_args
        process = subprocess.Popen(command)
        # Console Ctrl+C is delivered to WSL as well. Do not kill the VM when
        # Python receives it; the guest foreground process handles the key.
        while True:
            try:
                return process.wait()
            except KeyboardInterrupt:
                continue
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"k-vm WSL: {error}\nInstall WSL2 and k-vm in the selected distribution first.", file=sys.stderr)
        return 2
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    sys.exit(main())
