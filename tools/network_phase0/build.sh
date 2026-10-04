#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd -P)
repo_root=$(cd "$script_dir/../.." && pwd -P)

# shellcheck source=versions.conf
source "$script_dir/versions.conf"

clang_bin=${CLANG:-clang}
lld_bin=${LLD:-ld.lld}
host_cc=${CC:-cc}
nm_bin=${NM:-nm}
objdump_bin=${OBJDUMP:-objdump}

die()
{
    printf 'network-phase0: %s\n' "$*" >&2
    exit 1
}

layout_value()
{
    local name=$1
    local value

    value=$(awk -F '[(),[:space:]]+' -v wanted="$name" '$2 == wanted { print $3 }' "$repo_root/OS_src/kernel/platform_layout.def")
    [[ "$value" =~ ^(0x[0-9A-Fa-f]+|[0-9]+)$ ]] || die "invalid or missing platform layout value: $name"
    printf '%s' "$value"
}

usage()
{
    printf '%s\n' \
        "Usage:" \
        "  $0 --check-manifest" \
        "  $0 selected <libssh2-source> <mbedtls-source> [output-directory]" \
        "  $0 all <libssh2-source> <mbedtls-source> <wolfssh-source> <wolfssl-source> [output-directory]"
}

check_manifest()
{
    local required

    for required in \
        versions.conf \
        selected/libssh2_config.h \
        selected/mini_os_phase0_mbedtls_config.h \
        selected/main.c \
        selected/platform.c \
        selected/struct_sizes.c \
        wolf/user_settings.h \
        wolf/main.c \
        wolf/platform.c \
        compiler_rt_test.c \
        include/stdint.h \
        include/sys/types.h \
        host/build.sh \
        host/heap_probe.c \
        host/libssh2_packet_probe.patch; do
        [[ -f "$script_dir/$required" ]] || die "missing manifest file: $required"
    done
    grep -q 'MINI_OS_PHASE0_TEST_ENTROPY_ONLY' "$script_dir/selected/platform.c" || die "selected probe has no test-entropy marker"
    grep -q 'MINI_OS_PHASE0_TEST_ENTROPY_ONLY' "$script_dir/wolf/platform.c" || die "wolf probe has no test-entropy marker"
    grep -q '^LIBSSH2_COMMIT=[0-9a-f]\{40\}$' "$script_dir/versions.conf" || die "invalid libssh2 commit pin"
    grep -q '^MBEDTLS_COMMIT=[0-9a-f]\{40\}$' "$script_dir/versions.conf" || die "invalid Mbed TLS commit pin"
    grep -q '^WOLFSSH_COMMIT=[0-9a-f]\{40\}$' "$script_dir/versions.conf" || die "invalid wolfSSH commit pin"
    grep -q '^WOLFSSL_COMMIT=[0-9a-f]\{40\}$' "$script_dir/versions.conf" || die "invalid wolfSSL commit pin"
    bash -n "$script_dir/build.sh"
    bash -n "$script_dir/host/build.sh"
    layout_value APP_IMAGE_BASE >/dev/null
    layout_value APP_IMAGE_SIZE >/dev/null
    [[ -f "$repo_root/transport/app.ld" ]] || die "missing application linker script"
    command -v "$host_cc" >/dev/null 2>&1 || die "required compiler not found: $host_cc"
    compiler_test_dir=$(mktemp -d)
    trap 'rm -rf "$compiler_test_dir"' EXIT
    [[ -f "$repo_root/transport/lib/compiler_rt.c" ]] || die "missing runtime compiler helpers"
    "$host_cc" -O2 -std=c11 -Wall -Wextra -Werror "$repo_root/transport/lib/compiler_rt.c" "$script_dir/compiler_rt_test.c" -o "$compiler_test_dir/compiler-rt-test"
    "$compiler_test_dir/compiler-rt-test"
    printf 'network-phase0 manifest: PASS\n'
}

require_tool()
{
    command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
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
    shift
    local source_dir source_real target_dir target_name

    [[ -n "$requested" && "$requested" != / && "$requested" != . ]] || die "unsafe output directory: $requested"
    mkdir -p "$requested"
    output_root=$(cd "$requested" && pwd -P)
    [[ "$output_root" != / && "$output_root" != "$repo_root" ]] || die "unsafe output directory: $output_root"
    for source_dir in "$@"; do
        source_real=$(cd "$source_dir" && pwd -P)
        for target_name in elf2bin libssh2-mbedtls wolfssh-wolfcrypt; do
            target_dir=$output_root/$target_name
            if paths_overlap "$target_dir" "$source_real"; then
                die "output target overlaps $source_real: $target_dir"
            fi
        done
    done
}

section_size()
{
    local image=$1
    local section=$2
    local value

    value=$($objdump_bin -h "$image" | awk -v wanted="$section" '$2 == wanted { print $3; exit }')
    [[ -n "$value" ]] || value=0
    printf '%d' "0x$value"
}

allocated_sections()
{
    local image=$1

    "$objdump_bin" -h "$image" | awk '
        $1 ~ /^[0-9]+$/ && NF >= 5 {
            address = $4
            size = $3
            name = $2
            if ($5 == "TEXT" || $5 == "DATA" || $5 == "BSS") {
                print address, size, name
                next
            }
            if (getline > 0 && $0 ~ /ALLOC/) print address, size, name
        }
    '
}

allocated_size()
{
    local image=$1
    local address_hex size_hex
    local total=0
    local found=0

    while read -r address_hex size_hex _; do
        found=1
        total=$((total + 16#$size_hex))
    done < <(allocated_sections "$image")
    (( found != 0 )) || die "could not measure allocatable image size: $image"
    printf '%d' "$total"
}

allocated_span()
{
    local image=$1
    local address_hex size_hex address size end
    local first_address= end_address=0

    while read -r address_hex size_hex _; do
        address=$(printf '%d' "0x$address_hex")
        size=$(printf '%d' "0x$size_hex")
        (( size == 0 )) && continue
        end=$((address + size))
        if [[ -z "$first_address" ]] || (( address < first_address )); then
            first_address=$address
        fi
        if (( end > end_address )); then
            end_address=$end
        fi
    done < <(allocated_sections "$image")
    [[ -n "$first_address" ]] || die "could not measure allocatable image span: $image"
    printf '%d' "$((end_address - first_address))"
}

write_metrics()
{
    local candidate_name=$1
    local candidate_dir=$2
    local report_elf=$3
    local lld_flat=$4
    local elf2bin_flat=$5
    local struct_sizes_object=${6:-}
    local object_count unique_global_names section_total section_max section_max_object
    local relocation_32 relocation_pc32 relocation_total unresolved_final
    local text_size rodata_size data_size bss_size alloc_footprint alloc_span
    local lld_size elf2bin_size runtime_count stack_record stack_bytes
    local target_session_size target_transport_size session_size_hex transport_size_hex
    local object_path section_count

    object_count=${#candidate_objects[@]}
    unique_global_names=$($nm_bin -g "${candidate_objects[@]}" | awk '
        $1 == "U" { names[$2] = 1; next }
        NF >= 3 && $2 ~ /^[A-Za-z]$/ { names[$3] = 1 }
        END { for (name in names) count++; print count + 0 }
    ')

    section_total=0
    section_max=0
    section_max_object=
    for object_path in "${candidate_objects[@]}"; do
        section_count=$($objdump_bin -h "$object_path" | awk '/^[[:space:]]+[0-9]+[[:space:]]/ { count++ } END { print count + 1 }')
        section_total=$((section_total + section_count))
        if (( section_count > section_max )); then
            section_max=$section_count
            section_max_object=$object_path
        fi
    done

    relocation_32=$($objdump_bin -r "${candidate_objects[@]}" | awk '$2 == "R_386_32" { count++ } END { print count + 0 }')
    relocation_pc32=$($objdump_bin -r "${candidate_objects[@]}" | awk '$2 == "R_386_PC32" { count++ } END { print count + 0 }')
    relocation_total=$((relocation_32 + relocation_pc32))
    unresolved_final=$($nm_bin -u "$report_elf" | awk 'NF { count++ } END { print count + 0 }')

    text_size=$(section_size "$report_elf" .text)
    rodata_size=$(section_size "$report_elf" .rodata)
    data_size=$(section_size "$report_elf" .data)
    bss_size=$(section_size "$report_elf" .bss)
    alloc_footprint=$(allocated_size "$report_elf")
    alloc_span=$(allocated_span "$report_elf")
    lld_size=$(wc -c < "$lld_flat" | tr -d ' ')
    elf2bin_size=$(wc -c < "$elf2bin_flat" | tr -d ' ')

    $nm_bin -g "${third_party_objects[@]}" | awk '
        $1 == "U" { undefined[$2] = 1; next }
        NF >= 3 && $2 ~ /^[A-Za-z]$/ && $2 != "U" { defined[$3] = 1 }
        END { for (name in undefined) if (!(name in defined)) print name }
    ' | sort > "$candidate_dir/runtime-symbols.txt"
    runtime_count=$(awk 'NF { count++ } END { print count + 0 }' "$candidate_dir/runtime-symbols.txt")

    stack_record=$(find "$candidate_dir" -name '*.su' -type f -exec awk -F '\t' '{ print $2 "\t" $1 "\t" $3 }' {} + | sort -nr | sed -n '1p')
    stack_bytes=${stack_record%%$'\t'*}
    target_session_size=0
    target_transport_size=0
    if [[ -n "$struct_sizes_object" ]]; then
        session_size_hex=$($nm_bin -S "$struct_sizes_object" | awk '$4 == "mini_os_phase0_session_size" { print $2 }')
        transport_size_hex=$($nm_bin -S "$struct_sizes_object" | awk '$4 == "mini_os_phase0_transport_size" { print $2 }')
        [[ -n "$session_size_hex" && -n "$transport_size_hex" ]] || die "could not measure libssh2 target structures"
        target_session_size=$(printf '%d' "0x$session_size_hex")
        target_transport_size=$(printf '%d' "0x$transport_size_hex")
    fi

    printf '%s\n' "${candidate_objects[@]}" > "$candidate_dir/objects.txt"
    {
        printf 'candidate=%s\n' "$candidate_name"
        printf 'text_bytes=%s\n' "$text_size"
        printf 'rodata_bytes=%s\n' "$rodata_size"
        printf 'data_bytes=%s\n' "$data_size"
        printf 'bss_bytes=%s\n' "$bss_size"
        printf 'alloc_footprint_bytes=%s\n' "$alloc_footprint"
        printf 'alloc_span_bytes=%s\n' "$alloc_span"
        printf 'lld_flat_bytes=%s\n' "$lld_size"
        printf 'elf2bin_flat_bytes=%s\n' "$elf2bin_size"
        printf 'object_count=%s\n' "$object_count"
        printf 'unique_global_names=%s\n' "$unique_global_names"
        printf 'section_headers_total=%s\n' "$section_total"
        printf 'section_headers_max_per_object=%s\n' "$section_max"
        printf 'section_headers_max_object=%s\n' "$section_max_object"
        printf 'relocations_total=%s\n' "$relocation_total"
        printf 'relocations_R_386_32=%s\n' "$relocation_32"
        printf 'relocations_R_386_PC32=%s\n' "$relocation_pc32"
        printf 'unresolved_final=%s\n' "$unresolved_final"
        printf 'runtime_symbol_count=%s\n' "$runtime_count"
        printf 'largest_static_stack_frame_bytes=%s\n' "$stack_bytes"
        printf 'largest_static_stack_frame_record=%s\n' "$stack_record"
        printf 'target_libssh2_session_bytes=%s\n' "$target_session_size"
        printf 'target_libssh2_transport_bytes=%s\n' "$target_transport_size"
    } > "$candidate_dir/metrics.txt"
}

compile_minilibc()
{
    local destination=$1

    "$clang_bin" "${target_flags[@]}" -std=gnu11 -fstack-usage -I"$repo_root/transport/lib" -c "$repo_root/transport/lib/minilibc.c" -o "$destination"
}

build_selected()
{
    local libssh2_source=$1
    local mbedtls_source=$2
    local output_root=$3
    local build_dir=$output_root/libssh2-mbedtls
    local source_name required_macro forbidden_macro
    local selected_flags libssh2_flags

    rm -rf "$build_dir"
    mkdir -p "$build_dir/mbedtls" "$build_dir/libssh2" "$build_dir/probe" "$build_dir/measure"

    selected_flags=("${target_flags[@]}" -std=gnu11 -ffunction-sections -fdata-sections -fstack-usage '-DMBEDTLS_CONFIG_FILE="mini_os_phase0_mbedtls_config.h"' -I"$script_dir/include" -I"$script_dir/selected" -I"$repo_root/transport/lib" -I"$mbedtls_source/include")
    "$clang_bin" "${selected_flags[@]}" -dM -E "$mbedtls_source/library/aes.c" > "$build_dir/mbedtls-macros.txt"
    for required_macro in MBEDTLS_AES_C MBEDTLS_CTR_DRBG_C MBEDTLS_ECP_DP_SECP256R1_ENABLED MBEDTLS_RSA_C; do
        grep -q "^#define $required_macro" "$build_dir/mbedtls-macros.txt" || die "selected Mbed TLS config did not enable $required_macro"
    done
    for forbidden_macro in MBEDTLS_AESCE_C MBEDTLS_ARIA_C MBEDTLS_CAMELLIA_C MBEDTLS_PSA_CRYPTO_C; do
        if grep -q "^#define $forbidden_macro" "$build_dir/mbedtls-macros.txt"; then
            die "selected Mbed TLS config unexpectedly enabled $forbidden_macro"
        fi
    done

    mbedtls_sources=(aes asn1parse asn1write base64 bignum bignum_core bignum_mod bignum_mod_raw cipher cipher_wrap constant_time ctr_drbg ecdh ecdsa ecp ecp_curves entropy entropy_poll error md oid pem pk pk_ecc pk_wrap pkparse platform platform_util rsa rsa_alt_helpers sha1 sha256 sha512)
    for source_name in "${mbedtls_sources[@]}"; do
        "$clang_bin" "${selected_flags[@]}" -c "$mbedtls_source/library/$source_name.c" -o "$build_dir/mbedtls/$source_name.o"
    done

    libssh2_flags=("${selected_flags[@]}" -DHAVE_CONFIG_H -DLIBSSH2_MBEDTLS -I"$libssh2_source/include" -I"$libssh2_source/src")
    libssh2_sources=(bcrypt_pbkdf channel comp chacha cipher-chachapoly crypt crypto global hostkey keepalive kex mac misc packet pem poly1305 session transport userauth userauth_kbd_packet version)
    for source_name in "${libssh2_sources[@]}"; do
        "$clang_bin" "${libssh2_flags[@]}" -c "$libssh2_source/src/$source_name.c" -o "$build_dir/libssh2/$source_name.o"
    done

    "$clang_bin" "${libssh2_flags[@]}" -Wall -Wextra -Werror -c "$script_dir/selected/main.c" -o "$build_dir/probe/main.o"
    "$clang_bin" "${libssh2_flags[@]}" -Wall -Wextra -Werror -c "$script_dir/selected/platform.c" -o "$build_dir/probe/platform.o"
    "$clang_bin" "${selected_flags[@]}" -Wall -Wextra -Werror -c "$repo_root/transport/lib/compiler_rt.c" -o "$build_dir/probe/compiler_rt.o"
    compile_minilibc "$build_dir/probe/minilibc.o"
    "$clang_bin" "${libssh2_flags[@]}" -Wall -Wextra -Werror -c "$script_dir/selected/struct_sizes.c" -o "$build_dir/measure/struct_sizes.o"

    candidate_objects=("$build_dir/probe/compiler_rt.o" "$build_dir/probe/main.o" "$build_dir/probe/platform.o" "$build_dir/libssh2/"*.o "$build_dir/mbedtls/"*.o "$build_dir/probe/minilibc.o")
    third_party_objects=("$build_dir/libssh2/"*.o "$build_dir/mbedtls/"*.o)
    "$lld_bin" "${app_lld_args[@]}" "${candidate_objects[@]}" -o "$build_dir/report.elf"
    "$lld_bin" "${app_lld_args[@]}" --oformat binary "${candidate_objects[@]}" -o "$build_dir/lld.bin"
    "$output_root/elf2bin" "${elf2bin_capacity_args[@]}" "$build_dir/elf2bin.bin" "$app_image_base" "${candidate_objects[@]}"
    write_metrics libssh2-mbedtls "$build_dir" "$build_dir/report.elf" "$build_dir/lld.bin" "$build_dir/elf2bin.bin" "$build_dir/measure/struct_sizes.o"
}

build_wolf()
{
    local wolfssh_source=$1
    local wolfssl_source=$2
    local output_root=$3
    local build_dir=$output_root/wolfssh-wolfcrypt
    local source_name
    local wolf_flags

    rm -rf "$build_dir"
    mkdir -p "$build_dir/wolfcrypt" "$build_dir/wolfssh" "$build_dir/probe"
    wolf_flags=("${target_flags[@]}" -std=gnu11 -ffunction-sections -fdata-sections -fstack-usage -DWOLFSSL_USER_SETTINGS -I"$script_dir/include" -I"$script_dir/wolf" -I"$repo_root/transport/lib" -I"$wolfssl_source" -I"$wolfssh_source")

    wolfcrypt_sources=(hash hmac random sha256 aes logging wc_port wc_encrypt signature wolfmath asn sp_c32 sp_int ecc kdf coding)
    for source_name in "${wolfcrypt_sources[@]}"; do
        "$clang_bin" "${wolf_flags[@]}" -c "$wolfssl_source/wolfcrypt/src/$source_name.c" -o "$build_dir/wolfcrypt/$source_name.o"
    done
    wolfssh_sources=(ssh internal log io port)
    for source_name in "${wolfssh_sources[@]}"; do
        "$clang_bin" "${wolf_flags[@]}" -c "$wolfssh_source/src/$source_name.c" -o "$build_dir/wolfssh/$source_name.o"
    done
    "$clang_bin" "${wolf_flags[@]}" -Wall -Wextra -Werror -c "$script_dir/wolf/main.c" -o "$build_dir/probe/main.o"
    "$clang_bin" "${wolf_flags[@]}" -Wall -Wextra -Werror -c "$script_dir/wolf/platform.c" -o "$build_dir/probe/platform.o"
    "$clang_bin" "${wolf_flags[@]}" -Wall -Wextra -Werror -c "$repo_root/transport/lib/compiler_rt.c" -o "$build_dir/probe/compiler_rt.o"
    compile_minilibc "$build_dir/probe/minilibc.o"

    candidate_objects=("$build_dir/probe/compiler_rt.o" "$build_dir/probe/main.o" "$build_dir/probe/platform.o" "$build_dir/wolfssh/"*.o "$build_dir/wolfcrypt/"*.o "$build_dir/probe/minilibc.o")
    third_party_objects=("$build_dir/wolfssh/"*.o "$build_dir/wolfcrypt/"*.o)
    "$lld_bin" "${app_lld_args[@]}" "${candidate_objects[@]}" -o "$build_dir/report.elf"
    "$lld_bin" "${app_lld_args[@]}" --oformat binary "${candidate_objects[@]}" -o "$build_dir/lld.bin"
    "$output_root/elf2bin" "${elf2bin_capacity_args[@]}" "$build_dir/elf2bin.bin" "$app_image_base" "${candidate_objects[@]}"
    write_metrics wolfssh-wolfcrypt "$build_dir" "$build_dir/report.elf" "$build_dir/lld.bin" "$build_dir/elf2bin.bin"
}

if [[ ${1:-} == --check-manifest ]]; then
    [[ $# -eq 1 ]] || die "--check-manifest takes no other arguments"
    check_manifest
    exit 0
fi

mode=${1:-}
case "$mode" in
    selected)
        [[ $# -ge 3 && $# -le 4 ]] || { usage; exit 1; }
        libssh2_source=$2
        mbedtls_source=$3
        output_root=${4:-$repo_root/build/network-phase0}
        ;;
    all)
        [[ $# -ge 5 && $# -le 6 ]] || { usage; exit 1; }
        libssh2_source=$2
        mbedtls_source=$3
        wolfssh_source=$4
        wolfssl_source=$5
        output_root=${6:-$repo_root/build/network-phase0}
        ;;
    *)
        usage
        exit 1
        ;;
esac

require_tool "$clang_bin"
require_tool "$lld_bin"
require_tool "$host_cc"
require_tool "$nm_bin"
require_tool "$objdump_bin"
require_tool git

verify_checkout libssh2 "$libssh2_source" "$LIBSSH2_TAG" "$LIBSSH2_COMMIT"
verify_checkout "Mbed TLS" "$mbedtls_source" "$MBEDTLS_TAG" "$MBEDTLS_COMMIT"
if [[ "$mode" == all ]]; then
    verify_checkout wolfSSH "$wolfssh_source" "$WOLFSSH_TAG" "$WOLFSSH_COMMIT"
    verify_checkout wolfSSL "$wolfssl_source" "$WOLFSSL_TAG" "$WOLFSSL_COMMIT"
    source_roots=("$libssh2_source" "$mbedtls_source" "$wolfssh_source" "$wolfssl_source")
else
    source_roots=("$libssh2_source" "$mbedtls_source")
fi

prepare_output_root "$output_root" "${source_roots[@]}"
"$host_cc" -O2 -std=c11 -Wall -Wextra -Werror "$repo_root/tools/elf2bin.c" -o "$output_root/elf2bin"

app_image_base=$(layout_value APP_IMAGE_BASE)
app_image_size_hex=$(layout_value APP_IMAGE_SIZE)
printf -v app_image_size '%d' "$app_image_size_hex"
elf2bin_capacity_args=(--max-output "$app_image_size" --max-objects 256 --max-symbols 4096 --max-sections 4096 --max-relocations 32768)
app_lld_args=(-m elf_i386 --orphan-handling=error "--defsym=APP_LINK_BASE=$app_image_base" "--defsym=APP_LINK_SIZE=$app_image_size_hex" -T "$repo_root/transport/app.ld" --entry=main)

target_flags=(-target i386-unknown-none-elf -m32 -march=i386 -mno-sse -mno-mmx -ffreestanding -nostdlib -O2)
mbedtls_sources=()
libssh2_sources=()
wolfcrypt_sources=()
wolfssh_sources=()
candidate_objects=()
third_party_objects=()

{
    "$clang_bin" --version | sed -n '1,2p'
    "$lld_bin" --version
    "$host_cc" --version | sed -n '1p'
    "$objdump_bin" --version | sed -n '1p'
} > "$output_root/toolchain.txt"

build_selected "$libssh2_source" "$mbedtls_source" "$output_root"
if [[ "$mode" == all ]]; then
    build_wolf "$wolfssh_source" "$wolfssl_source" "$output_root"
fi

printf 'network-phase0 build: PASS\n'
printf 'selected metrics: %s\n' "$output_root/libssh2-mbedtls/metrics.txt"
if [[ "$mode" == all ]]; then
    printf 'comparison metrics: %s\n' "$output_root/wolfssh-wolfcrypt/metrics.txt"
fi
