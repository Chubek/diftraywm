#!/bin/sh
set -eu

prog=${0##*/}
base_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
generator=${AUROCKSGLR_GENERATOR:-$base_dir/AurocksGLR.pl}
if [ ! -f "$generator" ] && [ -f "$base_dir/../lib/AurocksGLR.pl" ]; then
    generator=$base_dir/../lib/AurocksGLR.pl
fi
targets_dir=${AUROCKSGLR_TARGET_PATH:-}
target=
macro_file=
output=
entrypoint=
grammar=

usage() {
    cat >&2 <<EOF
usage: $prog [OPTIONS] grammar.g
  -T, --target NAME       target backend (C, Go, Python, Ruby, Pascal, Lua)
  -m, --macros FILE       use a custom m4 macro set
      --targets-dir DIR   prepend a target search directory
  -o, --output FILE       write output to FILE instead of stdout
      --entrypoint NAME   override the generated parser entrypoint
EOF
    exit 2
}

while [ "$#" -gt 0 ]; do
    case $1 in
        -T|--target) [ "$#" -gt 1 ] || usage; target=$2; shift 2 ;;
        -m|--macros) [ "$#" -gt 1 ] || usage; macro_file=$2; shift 2 ;;
        --targets-dir) [ "$#" -gt 1 ] || usage
            if [ -n "$targets_dir" ]; then targets_dir=$2,$targets_dir; else targets_dir=$2; fi
            shift 2 ;;
        -o|--output) [ "$#" -gt 1 ] || usage; output=$2; shift 2 ;;
        --entrypoint) [ "$#" -gt 1 ] || usage; entrypoint=$2; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "$prog: unknown option: $1" >&2; usage ;;
        *) [ -z "$grammar" ] || { echo "$prog: more than one grammar file" >&2; exit 2; }
           grammar=$1; shift ;;
    esac
done
[ -n "$grammar" ] || usage

if [ -z "$macro_file" ]; then
    [ -n "$target" ] || target=C
    case $target in
        c|C) macro_name=ToC.m4 ;;
        go|Go|GO) macro_name=ToGo.m4 ;;
        python|Python|PYTHON|py) macro_name=ToPython.m4 ;;
        ruby|Ruby|RUBY|rb) macro_name=ToRuby.m4 ;;
        pascal|Pascal|PASCAL|pas) macro_name=ToPascal.m4 ;;
        lua|Lua|LUA) macro_name=ToLua.m4 ;;
        *) macro_name=$target; case $macro_name in *.m4) ;; *) macro_name=$macro_name.m4 ;; esac ;;
    esac
    old_ifs=$IFS; IFS=,
    found=
    for d in "$base_dir/target" "$base_dir/../lib/aurocksglr-target" $targets_dir; do
        [ -n "$d" ] || continue
        if [ -f "$d/$macro_name" ]; then found=$d/$macro_name; break; fi
        case $macro_name in
            *.m4) ;;
            *) [ -f "$d/$macro_name.m4" ] && { found=$d/$macro_name.m4; break; } ;;
        esac
    done
    IFS=$old_ifs
    [ -n "$found" ] || { echo "$prog: target '$target' not found (looked for $macro_name)" >&2; exit 1; }
    macro_file=$found
fi

case $macro_file in
    /*) ;;
    *) macro_file=$(CDPATH= cd -- "$(dirname -- "$macro_file")" && pwd)/$(basename -- "$macro_file") ;;
esac
macro_dir=$(dirname -- "$macro_file")
tmp=$(mktemp "${TMPDIR:-/tmp}/aurocksglr.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM

if [ -n "$entrypoint" ]; then
    perl "$generator" --entrypoint "$entrypoint" "$grammar" >"$tmp"
else
    perl "$generator" "$grammar" >"$tmp"
fi

default_target_dir=$base_dir/target
installed_target_dir=$base_dir/../lib/aurocksglr-target
if [ -n "$output" ]; then
    m4 -I "$macro_dir" -I "$default_target_dir" -I "$installed_target_dir" "$macro_file" "$tmp" >"$output"
else
    m4 -I "$macro_dir" -I "$default_target_dir" -I "$installed_target_dir" "$macro_file" "$tmp"
fi
