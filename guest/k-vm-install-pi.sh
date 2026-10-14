#!/bin/sh
# Install only in a staged image if changes must persist across sandbox exits.
set -eu
version=latest
packages=
update=false
offline=false
node_version=24.21.0
prefix=${K_VM_INSTALL_PREFIX:-/usr/local}
fail() { echo "k-vm init: $*" >&2; exit 1; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --version) [ "$#" -ge 2 ] || fail 'missing version'; version=$2; shift 2;;
        --packages) [ "$#" -ge 2 ] || fail 'missing packages directory'; packages=$2; shift 2;;
        --update) update=true; shift;;
        --offline) offline=true; shift;;
        *) fail "unknown argument: $1";;
    esac
done
case "$version" in ''|*[!a-zA-Z0-9.-]*) fail 'invalid Pi version';; esac
export PATH="$prefix/bin:$PATH"
if [ "$update" = false ] && command -v pi >/dev/null 2>&1; then
    installed=$(pi --version) || fail 'installed Pi failed verification'
    if [ "$version" = latest ] || [ "$installed" = "$version" ]; then
        echo "k-vm init: Pi $installed is already installed"
        exit 0
    fi
fi
temporary=$(mktemp -d /tmp/k-vm-install.XXXXXX)
trap 'rm -rf "$temporary"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export npm_config_cache="$temporary/npm-cache"
export npm_config_update_notifier=false
if ! [ -x "$prefix/bin/node" ]; then
    name="node-v$node_version-linux-riscv64.tar.xz"
    if [ -n "$packages" ]; then
        cp "$packages/$name" "$temporary/$name"
        cp "$packages/SHASUMS256.txt" "$temporary/SHASUMS256.txt"
    else
        base="https://unofficial-builds.nodejs.org/download/release/v$node_version"
        curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 \
            "$base/$name" -o "$temporary/$name"
        curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 \
            "$base/SHASUMS256.txt" -o "$temporary/SHASUMS256.txt"
    fi
    checksum=$(awk -v name="$name" '$2 == name {print $1}' "$temporary/SHASUMS256.txt")
    [ "${#checksum}" = 64 ] || fail 'missing or invalid Node checksum'
    case "$checksum" in *[!a-f0-9]*) fail 'invalid Node checksum';; esac
    (cd "$temporary" && printf '%s  %s\n' "$checksum" "$name" | sha256sum -c -) \
        || fail 'Node checksum mismatch'
    mkdir -p "$prefix"
    tar -xJf "$temporary/$name" -C "$prefix" --strip-components=1
fi
node --version
if [ "$offline" = true ]; then
    [ -n "$packages" ] && [ -d "$packages/npm-cache" ] || fail 'offline packages require a populated npm-cache'
fi
if [ -n "$packages" ] && [ -d "$packages/npm-cache" ]; then
    cp -R "$packages/npm-cache" "$temporary/npm-cache"
fi
set --
if [ "$offline" = true ]; then set -- --offline; fi
npm install -g --prefix "$prefix" "@earendil-works/pi-coding-agent@$version" \
    "$@" --no-progress --no-audit --no-fund
pi --version
echo 'k-vm init: Node.js and Pi are ready'
