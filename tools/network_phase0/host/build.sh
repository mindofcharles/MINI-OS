#!/usr/bin/env bash

set -euo pipefail

host_dir=$(cd "$(dirname "$0")" && pwd -P)
phase0_dir=$(cd "$host_dir/.." && pwd -P)
repo_root=$(cd "$phase0_dir/../.." && pwd -P)

# shellcheck source=../versions.conf
source "$phase0_dir/versions.conf"

cc_bin=${CC:-cc}

die()
{
    printf 'network-phase0-host: %s\n' "$*" >&2
    exit 1
}

verify_checkout()
{
    local label=$1
    local source_dir=$2
    local expected_tag=$3
    local expected_commit=$4
    local actual_commit checkout_status inside_work_tree tag_commit

    [[ -d "$source_dir" ]] || die "$label source directory does not exist: $source_dir"
    inside_work_tree=$(git -C "$source_dir" rev-parse --is-inside-work-tree 2>/dev/null) || die "$label source is not a Git checkout: $source_dir"
    [[ "$inside_work_tree" == true ]] || die "$label source is not a Git work tree: $source_dir"
    actual_commit=$(git -C "$source_dir" rev-parse HEAD) || die "$label checkout has no resolvable HEAD"
    [[ "$actual_commit" == "$expected_commit" ]] || die "$label checkout is $actual_commit, expected $expected_commit"
    tag_commit=$(git -C "$source_dir" rev-parse "$expected_tag^{}" 2>/dev/null) || die "$label checkout has no tag $expected_tag"
    [[ "$tag_commit" == "$expected_commit" ]] || die "$label tag $expected_tag is $tag_commit, expected $expected_commit"
    checkout_status=$(git -C "$source_dir" status --porcelain --untracked-files=normal) || die "$label checkout status failed"
    [[ -z "$checkout_status" ]] || die "$label checkout is not clean"
}

paths_overlap()
{
    local left=${1%/}
    local right=${2%/}

    [[ "$left" == "$right" || "$left" == "$right/"* || "$right" == "$left/"* ]]
}

prepare_output_root()
{
    local requested=$1
    local source_dir source_real target_dir

    shift
    [[ -n "$requested" && "$requested" != / && "$requested" != . ]] || die "unsafe output directory: $requested"
    mkdir -p "$requested"
    output_root=$(cd "$requested" && pwd -P)
    [[ "$output_root" != / && "$output_root" != "$repo_root" ]] || die "unsafe output directory: $output_root"
    target_dir=$output_root/host-probe
    for source_dir in "$@"; do
        source_real=$(cd "$source_dir" && pwd -P)
        if paths_overlap "$target_dir" "$source_real"; then
            die "output target overlaps $source_real: $target_dir"
        fi
    done
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'Usage: %s <libssh2-source> <mbedtls-source> [output-directory]\n' "$0" >&2
    exit 1
fi

libssh2_source=$1
mbedtls_source=$2
output_root=${3:-$repo_root/build/network-phase0}

command -v "$cc_bin" >/dev/null 2>&1 || die "required compiler not found: $cc_bin"
command -v patch >/dev/null 2>&1 || die "required tool not found: patch"
command -v git >/dev/null 2>&1 || die "required tool not found: git"
verify_checkout libssh2 "$libssh2_source" "$LIBSSH2_TAG" "$LIBSSH2_COMMIT"
verify_checkout "Mbed TLS" "$mbedtls_source" "$MBEDTLS_TAG" "$MBEDTLS_COMMIT"
prepare_output_root "$output_root" "$libssh2_source" "$mbedtls_source"
build_dir=$output_root/host-probe

rm -rf "$build_dir"
mkdir -p "$build_dir/mbedtls" "$build_dir/libssh2" "$build_dir/probe" "$build_dir/patched/src"
cp "$libssh2_source/src/transport.c" "$build_dir/patched/src/transport.c"
patch -s -d "$build_dir/patched" -p1 < "$host_dir/libssh2_packet_probe.patch"

common_flags=(-O2 -std=c11 -ffunction-sections -fdata-sections '-DMBEDTLS_CONFIG_FILE="mini_os_phase0_host_mbedtls_config.h"' -I"$host_dir" -I"$mbedtls_source/include")
mbedtls_sources=(aes asn1parse asn1write base64 bignum bignum_core bignum_mod bignum_mod_raw cipher cipher_wrap constant_time ctr_drbg ecdh ecdsa ecp ecp_curves entropy entropy_poll error md oid pem pk pk_ecc pk_wrap pkparse platform platform_util rsa rsa_alt_helpers sha1 sha256 sha512)
for source_name in "${mbedtls_sources[@]}"; do
    "$cc_bin" "${common_flags[@]}" -c "$mbedtls_source/library/$source_name.c" -o "$build_dir/mbedtls/$source_name.o"
done

libssh2_flags=("${common_flags[@]}" -DHAVE_CONFIG_H -DLIBSSH2_MBEDTLS -DMINI_OS_PHASE0_PACKET_PROBE -I"$libssh2_source/include" -I"$libssh2_source/src")
libssh2_sources=(bcrypt_pbkdf channel comp chacha cipher-chachapoly crypt crypto global hostkey keepalive kex mac misc packet pem poly1305 session transport userauth userauth_kbd_packet version)
for source_name in "${libssh2_sources[@]}"; do
    source_path=$libssh2_source/src/$source_name.c
    if [[ "$source_name" == transport ]]; then
        source_path=$build_dir/patched/src/transport.c
    fi
    "$cc_bin" "${libssh2_flags[@]}" -c "$source_path" -o "$build_dir/libssh2/$source_name.o"
done

"$cc_bin" "${libssh2_flags[@]}" -Wall -Wextra -Werror -c "$host_dir/heap_probe.c" -o "$build_dir/probe/heap_probe.o"
if [[ $(uname -s) == Darwin ]]; then
    linker_gc=(-Wl,-dead_strip)
else
    linker_gc=(-Wl,--gc-sections)
fi
"$cc_bin" "${linker_gc[@]}" "$build_dir/probe/heap_probe.o" "$build_dir/libssh2/"*.o "$build_dir/mbedtls/"*.o -o "$build_dir/heap-probe"

printf 'network-phase0 host probe: PASS\n'
printf 'Run: %s <IPv4> <port> <user> <public-key> <private-key>\n' "$build_dir/heap-probe"
