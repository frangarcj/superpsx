#!/usr/bin/env python3
"""Verify that implemented optimizations are visible in generated native code."""
import struct, sys
from collections import defaultdict

def parse_dump(path):
    with open(path, "rb") as f:
        magic = f.read(4)
        assert magic == b"JITD"
        (bc,) = struct.unpack("<I", f.read(4))
        blocks = []
        for _ in range(bc):
            hdr = f.read(20)
            if len(hdr) < 20: break
            pc, ic, nc, cc, ec = struct.unpack("<5I", hdr)
            psx = list(struct.unpack(f"<{ic}I", f.read(ic * 4)))
            native = list(struct.unpack(f"<{nc}I", f.read(nc * 4)))
            blocks.append({"pc": pc, "ic": ic, "nc": nc, "ec": ec,
                           "psx": psx, "native": native})
    return blocks

def OP(w): return (w >> 26) & 0x3F
def RS(w): return (w >> 21) & 0x1F
def RT(w): return (w >> 16) & 0x1F
def RD(w): return (w >> 11) & 0x1F
def FUNC(w): return w & 0x3F
def SIMM(w):
    v = w & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

PRO = 22  # prologue words

def check_block(b):
    """Analyze a block's native code for optimization effectiveness."""
    pc = b["pc"]
    psx = b["psx"]
    native = b["native"]
    pp = native[PRO:]  # post-prologue
    n = len(pp)
    
    # Count PSX instruction types
    loads = sum(1 for w in psx if OP(w) in (0x20,0x21,0x22,0x23,0x24,0x25,0x26))
    stores = sum(1 for w in psx if OP(w) in (0x28,0x29,0x2A,0x2B,0x2E))
    lwc2 = sum(1 for w in psx if OP(w) == 0x32)
    swc2 = sum(1 for w in psx if OP(w) == 0x3A)
    total_mem = loads + stores + lwc2 + swc2
    
    # P6: Alignment check (ANDI Tx, T8, mask where mask = 1 or 3)
    align_checks = 0
    for i, w in enumerate(pp):
        if OP(w) == 0x0C:  # ANDI
            rs, rt, imm = RS(w), RT(w), w & 0xFFFF
            if imm in (1, 3) and rs == 24:  # ANDI from T8, mask 1 or 3
                align_checks += 1
    
    # SMRV range check: SRL AT, T9, 21
    range_checks = 0
    for w in pp:
        if w & 0xFFFF07FF == 0x00190542:  # SRL $at, $t9, 21
            range_checks += 1
    
    # AND mask with S3: AND Rx, Ry, S3 (func=0x24, rt field = S3=19)
    and_masks = 0
    for w in pp:
        if FUNC(w) == 0x24 and RT(w) == 19:  # AND with S3
            and_masks += 1
    
    # ADDU with S1: ADDU Rx, Ry, S1 (func=0x21, involving S1=17)
    addu_s1 = 0
    for w in pp:
        if FUNC(w) == 0x21 and (RS(w) == 17 or RT(w) == 17):
            # Exclude prologue MOVE S0,A0 etc
            rd = RD(w)
            if rd not in (16, 17, 18):  # not S0,S1,S2
                addu_s1 += 1
    
    # ISC check: LW AT, 0xBC(S0) = 0x8E0100BC
    isc_checks = sum(1 for w in pp if w == 0x8E0100BC)
    
    # P23 offset folding: LUI+ORI forming a host address (0x0050xxxx-0x00FFxxxx range)
    p23_folds = 0
    for i in range(len(pp) - 1):
        if OP(pp[i]) == 0x0F:  # LUI
            hi = pp[i] & 0xFFFF
            if 0x0030 <= hi <= 0x00FF:  # plausible host RAM address
                if OP(pp[i+1]) == 0x0D and RS(pp[i+1]) == RT(pp[i]):  # ORI same reg
                    p23_folds += 1
    
    # SMC page-gen check: LUI+ORI loading page_gen addr + LW + BEQ
    smc_checks = 0
    for i in range(len(pp) - 4):
        if OP(pp[i]) == 0x0F:  # LUI
            rt0 = RT(pp[i])
            if i+1 < n and OP(pp[i+1]) == 0x0D and RS(pp[i+1]) == rt0 and RT(pp[i+1]) == rt0:
                if i+2 < n and OP(pp[i+2]) == 0x23 and RS(pp[i+2]) == rt0:
                    if i+3 < n and OP(pp[i+3]) == 0x04 and RT(pp[i+3]) == 0:  # BEQ, zero
                        smc_checks += 1
    
    # Check for stores WITHOUT ISC guard (potential missing P22)
    stores_without_isc = 0
    for i, w in enumerate(pp):
        if OP(w) == 0x2B:  # SW
            # Check if preceded by BNE (ISC skip) within 4 instructions
            has_isc_guard = False
            for j in range(max(0, i-5), i):
                pw = pp[j]
                # LW AT, XX(SP) followed by BNE AT, ZERO
                if OP(pw) == 0x23 and RS(pw) == 29:  # LW from SP
                    if j+1 < n and OP(pp[j+1]) == 0x05 and RS(pp[j+1]) == RT(pw):
                        has_isc_guard = True
            if not has_isc_guard and total_mem > 0:
                stores_without_isc += 1
    
    # Guarded JR: ORI $at, $zero, imm; BNE $t8, $at
    guarded_jr = 0
    for i in range(len(pp) - 1):
        if OP(pp[i]) == 0x0D and RS(pp[i]) == 0 and RT(pp[i]) == 1:  # ORI $at, $zero
            if OP(pp[i+1]) == 0x05 and RS(pp[i+1]) == 24 and RT(pp[i+1]) == 1:  # BNE $t8, $at
                guarded_jr += 1
    
    return {
        "pc": pc, "psx_insns": b["ic"], "ee_words": b["nc"], "exec": b["ec"],
        "loads": loads, "stores": stores, "total_mem": total_mem,
        "align_checks": align_checks,
        "range_checks": range_checks,
        "and_masks": and_masks,
        "addu_s1": addu_s1,
        "isc_checks": isc_checks,
        "p23_folds": p23_folds,
        "smc_checks": smc_checks,
        "stores_without_isc": stores_without_isc,
        "guarded_jr": guarded_jr,
    }


def main():
    blocks = parse_dump(sys.argv[1])
    
    # Analyze top 30 blocks by exec impact
    hot = sorted(blocks, key=lambda b: -b["ec"] * b["nc"])[:30]
    
    print("=" * 120)
    print(" OPTIMIZATION VERIFICATION — checking generated native code for expected patterns")
    print("=" * 120)
    
    # Summary counters
    total_checks = defaultdict(int)
    
    print(f"\n {'PC':>10} {'Exec':>7} {'PSX':>4} {'EE':>4} {'Ld':>3} {'St':>3} "
          f"{'Align':>5} {'Range':>5} {'AND':>4} {'ISC':>4} {'P23':>4} {'SMC':>4} "
          f"{'StNoISC':>7} {'GuardJR':>7}")
    print("-" * 120)
    
    for b in hot:
        r = check_block(b)
        total_checks["align"] += r["align_checks"] * r["exec"]
        total_checks["range"] += r["range_checks"] * r["exec"]
        total_checks["and"] += r["and_masks"] * r["exec"]
        total_checks["isc"] += r["isc_checks"] * r["exec"]
        total_checks["p23"] += r["p23_folds"] * r["exec"]
        total_checks["smc"] += r["smc_checks"] * r["exec"]
        total_checks["st_no_isc"] += r["stores_without_isc"] * r["exec"]
        total_checks["guarded_jr"] += r["guarded_jr"] * r["exec"]
        
        # Flag anomalies
        flags = []
        if r["stores"] > 0 and r["stores_without_isc"] > 0:
            flags.append("SW_NO_ISC!")
        if r["total_mem"] > 0 and r["and_masks"] > 0 and r["p23_folds"] == 0:
            if any(OP(w) == 0x0F for w in blocks[0]["psx"]):  # has LUI
                flags.append("NO_P23?")
        if r["guarded_jr"] > 0:
            flags.append("GUARDED_JR")
        
        flag_str = " ".join(flags)
        print(f" 0x{r['pc']:08X} {r['exec']:>7} {r['psx_insns']:>4} {r['ee_words']:>4} "
              f"{r['loads']:>3} {r['stores']:>3} "
              f"{r['align_checks']:>5} {r['range_checks']:>5} {r['and_masks']:>4} "
              f"{r['isc_checks']:>4} {r['p23_folds']:>4} {r['smc_checks']:>4} "
              f"{r['stores_without_isc']:>7} {r['guarded_jr']:>7}  {flag_str}")
    
    print(f"\n{'='*120}")
    print(" SUMMARY (exec-weighted across top 30 blocks)")
    print(f"{'='*120}")
    print(f" Alignment checks remaining: {total_checks['align']:>12,} (P6 should minimize these)")
    print(f" Range checks remaining:     {total_checks['range']:>12,} (SMRV should minimize these)")
    print(f" AND mask operations:         {total_checks['and']:>12,} (P23 folding should minimize these)")
    print(f" ISC checks:                  {total_checks['isc']:>12,} (one per block with mem access)")
    print(f" P23 offset folds:            {total_checks['p23']:>12,} (should be present for const-addr)")
    print(f" SMC page-gen checks:         {total_checks['smc']:>12,} (one per store)")
    print(f" Stores WITHOUT ISC guard:    {total_checks['st_no_isc']:>12,} (P22 bug if >0 for RAM stores)")
    print(f" Guarded JR direct links:     {total_checks['guarded_jr']:>12,} (our new optimization)")
    
    # Specific checks
    print(f"\n{'='*120}")
    print(" POTENTIAL ISSUES")
    print(f"{'='*120}")
    
    issues = []
    if total_checks["st_no_isc"] > 0:
        issues.append(f"  [!] {total_checks['st_no_isc']:,} store-executions have no ISC guard — "
                      f"possible P22 ISC check missing for offset-folded stores")
    if total_checks["align"] > 0:
        # Check if any aligned loads still have alignment checks
        for b in hot:
            r = check_block(b)
            if r["align_checks"] > 0 and r["loads"] + r["stores"] > 0:
                issues.append(f"  [?] Block 0x{r['pc']:08X}: {r['align_checks']} alignment checks "
                              f"for {r['loads']}L+{r['stores']}S — check if P6 is active")
    
    if not issues:
        print("  No issues detected — all optimizations appear active.")
    else:
        for issue in issues[:10]:
            print(issue)


if __name__ == "__main__":
    main()
