#!/usr/bin/env bash
# cangjie-runtime#59: the product tree is compiled as gnu++14
# (runtime/config.cmake:368). There an in-class `static constexpr` declaration
# is not a definition, so every Uncommitter constant that gets odr-used (e.g.
# std::min(const T&, const T&) in ChunkLimit) needs an out-of-class definition
# in zUncommitter.cpp, or the -O0 (Debug) link of libcangjie-runtime.so fails
# with "undefined reference to MapleRuntime::Uncommitter::k...". -O2 hides the
# defect by constant folding, which is why the Release gate never sees it.
#
# Two objects, both compiled with the product standard and the product -O0:
#   consumer.o : a generated TU that odr-uses (takes the address of) every
#                `static constexpr` member declared in zUncommitter.hpp.
#                Positive control: nm -u must list each member, otherwise this
#                probe cannot observe the mechanism it claims to test.
#   provider.o : runtime/src/Heap/z/zUncommitter.cpp itself. nm --defined-only
#                must list each member; a missing one is exactly the #59 link
#                failure, reported per symbol.
set -uo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
runtime_dir="$(cd "${script_dir}/.." && pwd)"
header="${runtime_dir}/src/Heap/z/zUncommitter.hpp"
provider_src="${runtime_dir}/src/Heap/z/zUncommitter.cpp"
out_dir="${UNCOMMITTER_ODR_PROBE_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/uncommitter-constexpr-odr.XXXXXX")}"
mkdir -p "${out_dir}"
if [[ -z "${UNCOMMITTER_ODR_PROBE_OUT:-}" ]]; then
    trap 'rm -rf "${out_dir}"' EXIT
fi
prefix="UNCOMMITTER_CONSTEXPR_ODR_PROBE"

# Same standard, optimisation level and defines as the Debug product compile
# of this TU (runtime/config.cmake:368,372; runtime/src/Heap/CMakeLists.txt).
cxx_flags=(
    -std=gnu++14 -O0 -fPIC -fno-exceptions -fno-rtti -w
    -DCANGJIE_GWPASAN_SUPPORT -DCJ_SDK_VERSION='"0.0.1"' -DDISABLE_VERSION_CHECK
    -DMRT_USE_CJTHREAD_RENAME -DMRT_USE_COPYGC -DMRT_DEBUG=1
    -I"${runtime_dir}/third_party/third_party_bounds_checking_function/include"
    -I"${runtime_dir}/include"
    -I"${runtime_dir}/src"
    -I"${runtime_dir}/src/Heap"
)

# Every `static constexpr <type> <name> = ...;` member of class Uncommitter.
mapfile -t members < <(sed -n '/^class Uncommitter {/,/^};/p' "${header}" \
    | sed -nE 's/^[[:space:]]*static constexpr [A-Za-z_0-9:<>]+[[:space:]]+([A-Za-z_][A-Za-z_0-9]*)[[:space:]]*=.*/\1/p')
if [[ ${#members[@]} -eq 0 ]]; then
    echo "${prefix}_FAIL: no static constexpr member found in ${header}" >&2
    exit 2
fi

consumer_src="${out_dir}/consumer.cpp"
{
    echo '#include "Heap/z/zUncommitter.hpp"'
    echo 'namespace MapleRuntime {'
    echo 'const void* const uncommitterConstexprOdrUses[] = {'
    for m in "${members[@]}"; do echo "    &Uncommitter::${m},"; done
    echo '};'
    echo '}'
} > "${consumer_src}"

cxx="${CXX:-clang++}"
if ! "${cxx}" "${cxx_flags[@]}" -c "${consumer_src}" -o "${out_dir}/consumer.o" > "${out_dir}/consumer.build.log" 2>&1; then
    echo "${prefix}_FAIL: consumer TU did not compile (see ${out_dir}/consumer.build.log)" >&2
    head -20 "${out_dir}/consumer.build.log" >&2
    exit 2
fi
if ! "${cxx}" "${cxx_flags[@]}" -c "${provider_src}" -o "${out_dir}/provider.o" > "${out_dir}/provider.build.log" 2>&1; then
    echo "${prefix}_FAIL: ${provider_src} did not compile (see ${out_dir}/provider.build.log)" >&2
    head -20 "${out_dir}/provider.build.log" >&2
    exit 2
fi

nm -C -u "${out_dir}/consumer.o" > "${out_dir}/consumer.undefined.txt"
nm -C --defined-only "${out_dir}/provider.o" > "${out_dir}/provider.defined.txt"

rc=0
for m in "${members[@]}"; do
    sym="MapleRuntime::Uncommitter::${m}"
    if ! grep -qE "[[:space:]]${sym}\$" "${out_dir}/consumer.undefined.txt"; then
        echo "${prefix}_FAIL: positive control broken: consumer.o does not odr-use ${sym}" >&2
        rc=1
        continue
    fi
    if grep -qE "[[:space:]]${sym}\$" "${out_dir}/provider.defined.txt"; then
        echo "[  PASS  ] ${sym} is defined by zUncommitter.cpp (gnu++14 -O0)"
    else
        echo "${prefix}_FAIL: ${sym} is odr-used but zUncommitter.cpp defines no out-of-class ${m} (Debug link would fail, #59)" >&2
        rc=1
    fi
done
echo "RESULT=$([[ ${rc} -eq 0 ]] && echo PASS || echo FAIL) members=${#members[@]} out=${out_dir}"
exit "${rc}"
