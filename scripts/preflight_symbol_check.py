#!/usr/bin/env python3
"""
Pre-flight symbol and ABI validator for out-of-tree kernel modules.
Validates all undefined symbols in target .ko files against Module.symvers
(or /proc/kallsyms) and sibling module exports to prevent kernel oops/boot-loops
caused by missing or mismatched kernel symbols.
"""

import sys
import os
import subprocess
import argparse

# Compiler/architecture intrinsics and known non-exported symbols that do not require kernel export resolution
IGNORED_PREFIXES = (
    "__fentry__",
    "__pfx_",
    "__x86_",
    "__stack_chk_",
    "__gnu_",
    "_GLOBAL_OFFSET_TABLE_",
    "mcount",
    "__asan_",
    "__ubsan_",
)

def get_kernel_symbols(symvers_path=None):
    symbols = set()
    
    # 1. Try Module.symvers first (highest fidelity: exact exported symbols)
    if symvers_path and os.path.exists(symvers_path):
        try:
            with open(symvers_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) >= 2:
                        symbols.add(parts[1])
            if symbols:
                return symbols, f"Module.symvers ({symvers_path})"
        except Exception as e:
            print(f"[WARN] Failed reading {symvers_path}: {e}", file=sys.stderr)

    # 2. Fall back to /proc/kallsyms if running on target system
    kallsyms_path = "/proc/kallsyms"
    if os.path.exists(kallsyms_path):
        try:
            with open(kallsyms_path, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) >= 3:
                        symbols.add(parts[2])
            if symbols:
                return symbols, f"/proc/kallsyms ({len(symbols)} symbols)"
        except Exception as e:
            print(f"[WARN] Failed reading {kallsyms_path}: {e}", file=sys.stderr)

    return symbols, "None"

def collect_exports(ko_files):
    exports = set()
    for ko in ko_files:
        if not os.path.exists(ko):
            continue
        try:
            res = subprocess.run(["nm", "-g", "--defined-only", ko], capture_output=True, text=True, check=True)
            for line in res.stdout.splitlines():
                parts = line.strip().split()
                if len(parts) >= 3 and parts[1] in ("T", "D", "B", "R"):
                    exports.add(parts[2])
        except Exception as e:
            print(f"[WARN] Could not inspect exports in {ko}: {e}", file=sys.stderr)
    return exports

def check_modules(ko_files, known_symbols, local_exports):
    all_ok = True
    results = {}
    
    for ko in ko_files:
        if not os.path.exists(ko):
            print(f"[ERROR] Module file not found: {ko}", file=sys.stderr)
            all_ok = False
            continue
            
        try:
            res = subprocess.run(["nm", "-u", ko], capture_output=True, text=True, check=True)
        except Exception as e:
            print(f"[ERROR] Failed to run nm on {ko}: {e}", file=sys.stderr)
            all_ok = False
            continue

        missing = []
        for line in res.stdout.splitlines():
            sym = line.strip().split()[-1]
            if any(sym.startswith(prefix) for prefix in IGNORED_PREFIXES):
                continue
            if sym not in known_symbols and sym not in local_exports:
                missing.append(sym)

        if missing:
            all_ok = False
            results[ko] = missing
            print(f"  [FAIL] {os.path.basename(ko)}: {len(missing)} unresolved symbol(s)!")
            for s in missing[:10]:
                print(f"         - {s}")
            if len(missing) > 10:
                print(f"         ... and {len(missing) - 10} more")
        else:
            print(f"  [PASS] {os.path.basename(ko)}: all symbols resolved.")

    return all_ok

def main():
    parser = argparse.ArgumentParser(description="Pre-flight symbol validator for kernel modules")
    parser.add_argument("modules", nargs="+", help="Path(s) to .ko files or directories containing .ko files")
    parser.add_argument("--symvers", help="Path to Module.symvers (default: /lib/modules/$(uname -r)/build/Module.symvers)")
    args = parser.parse_args()

    # Discover .ko files
    ko_files = []
    for item in args.modules:
        if os.path.isdir(item):
            for root, _, files in os.walk(item):
                for f in files:
                    if f.endswith(".ko"):
                        ko_files.append(os.path.join(root, f))
        elif os.path.isfile(item) and item.endswith(".ko"):
            ko_files.append(item)

    if not ko_files:
        print("[ERROR] No .ko files provided or found to inspect.", file=sys.stderr)
        sys.exit(1)

    symvers = args.symvers
    if not symvers:
        kver = os.uname().release
        default_symvers = f"/lib/modules/{kver}/build/Module.symvers"
        if os.path.exists(default_symvers):
            symvers = default_symvers

    known_symbols, source = get_kernel_symbols(symvers)
    if not known_symbols:
        print("[ERROR] No kernel symbols available to validate against! Cannot proceed safely.", file=sys.stderr)
        sys.exit(1)

    print(f"[PRE-FLIGHT] Checking {len(ko_files)} module(s) against kernel symbols from {source}...")
    local_exports = collect_exports(ko_files)
    if local_exports:
        print(f"[PRE-FLIGHT] Collected {len(local_exports)} exported symbol(s) between target modules.")

    success = check_modules(ko_files, known_symbols, local_exports)
    if success:
        print("[PRE-FLIGHT] ALL MODULES PASSED STATIC SYMBOL VERIFICATION.")
        sys.exit(0)
    else:
        print("[PRE-FLIGHT] HARD STOP: Unresolved kernel symbols detected. Halting to prevent boot loop / oops.", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
