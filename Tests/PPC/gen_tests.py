#!/usr/bin/env python3
"""Assemble the PowerPC interpreter's test cases into Tests/ToolboxTests/PPCVectors.swift.

Each case: assembly (ending in blr), the registers it starts with, and the
registers (or memory words) it must end with.  Needs Retro68's
powerpc-apple-macos-as; the generated file is committed, so the tests don't.
    python3 Tests/PPC/gen_tests.py
"""
import os, subprocess, tempfile, struct

TOOL = os.path.expanduser("~/Developer/classic-oracle/Retro68-build/toolchain/bin/powerpc-apple-macos-")

M = 0xFFFFFFFF
CASES = [
    # name, asm, start {reg: value}, expect {reg: value}; registers r0-r31, cr, xer, ctr, lr; m<addr> memory word
    ("addi_addis", "li r3,-5\n addis r4,r3,1\n addi r5,0,0x7fff", {}, {"r3": M - 4, "r4": 0x0000FFFB, "r5": 0x7FFF}),
    ("add_carry_chain", "addc r5,r3,r4\n adde r6,r7,r8", {"r3": 0xFFFFFFFF, "r4": 2, "r7": 1, "r8": 1}, {"r5": 1, "r6": 3, "xer": 0}),
    ("subf_family", "subf r5,r3,r4\n subfc r6,r3,r4\n subfe r7,r4,r3", {"r3": 10, "r4": 3}, {"r5": M - 6, "r6": M - 6, "r7": 6, "xer": 0x20000000}),
    ("neg_and_overflow", "nego. r5,r3", {"r3": 0x80000000}, {"r5": 0x80000000, "xer": 0xC0000000, "cr": 0x90000000}),
    ("mul", "mullw r5,r3,r4\n mulhw r6,r3,r4\n mulhwu r7,r3,r4\n mulli r8,r3,-3", {"r3": 0xFFFFFFFE, "r4": 3},
     {"r5": M - 5, "r6": M, "r7": 2, "r8": 6}),
    ("div", "divw r5,r3,r4\n divwu r6,r3,r4", {"r3": 0xFFFFFFF9, "r4": 2}, {"r5": M - 2, "r6": 0x7FFFFFFC}),
    # The 601's POWER instructions (FutureBASIC^3 divides with divs).
    ("power_601", "divs r5,r3,r4\n mfspr r6,0\n mul r7,r3,r4\n mfspr r8,0\n abs r9,r3\n nabs r11,r4\n doz r12,r3,r4\n dozi r13,r4,5\n"
     " li r17,4\n li r16,7\n mtspr 0,r16\n li r14,1\n div r15,r14,r17\n mfspr r16,0",
     {"r3": 0xFFFFFFF9, "r4": 2},
     {"r5": M - 2, "r6": M, "r7": M, "r8": 0xFFFFFFF2, "r9": 7, "r11": 0xFFFFFFFE, "r12": 9, "r13": 3,
      "r15": 0x40000001, "r16": 3}),
    ("logic", "and r5,r3,r4\n or r6,r3,r4\n xor r7,r3,r4\n nand r8,r3,r4\n nor r9,r3,r4\n andc r10,r3,r4\n eqv r11,r3,r4\n orc r12,r3,r4",
     {"r3": 0xF0F0F0F0, "r4": 0xFF00FF00},
     {"r5": 0xF000F000, "r6": 0xFFF0FFF0, "r7": 0x0FF00FF0, "r8": 0x0FFF0FFF, "r9": 0x000F000F, "r10": 0x00F000F0,
      "r11": 0xF00FF00F, "r12": 0xF0FFF0FF}),
    ("immediates", "ori r5,r3,0x1234\n oris r6,r3,0x1234\n xori r7,r3,0xFFFF\n andi. r8,r3,0x00F0\n andis. r9,r3,0x0F00",
     {"r3": 0xF0F0F0F0}, {"r5": 0xF0F0F2F4, "r6": 0xF2F4F0F0, "r7": 0xF0F00F0F, "r8": 0xF0, "r9": 0, "cr": 0x20000000}),
    ("extend_count", "extsb r5,r3\n extsh r6,r3\n cntlzw r7,r4\n cntlzw r8,r9", {"r3": 0x1234F080, "r4": 0x00010000, "r9": 0},
     {"r5": 0xFFFFFF80, "r6": 0xFFFFF080, "r7": 15, "r8": 32}),
    ("rotates", "rlwinm r5,r3,8,24,31\n rlwinm r6,r3,0,16,23\n rlwimi r7,r3,16,0,15\n rlwnm r8,r3,r4,0,31",
     {"r3": 0x12345678, "r4": 4, "r7": 0xAAAAAAAA}, {"r5": 0x12, "r6": 0x5600, "r7": 0x5678AAAA, "r8": 0x23456781}),
    ("shifts", "slw r5,r3,r4\n srw r6,r3,r4\n sraw r7,r3,r4\n srawi r8,r3,4\n slw r9,r3,r10",
     {"r3": 0x80000010, "r4": 4, "r10": 32}, {"r5": 0x00000100, "r6": 0x08000001, "r7": 0xF8000001, "r8": 0xF8000001,
      "r9": 0, "xer": 0}),
    ("srawi_carry", "srawi r5,r3,1", {"r3": 0xFFFFFFFF}, {"r5": 0xFFFFFFFF, "xer": 0x20000000}),
    ("compare_cr", "cmpwi cr0,r3,-1\n cmplwi cr1,r3,5\n cmpw cr2,r3,r4\n cmplw cr3,r4,r3",
     {"r3": 0xFFFFFFFF, "r4": 1}, {"cr": 0x24880000}),
    ("branch_ctr_loop", "li r5,0\n mtctr r3\n1: addi r5,r5,2\n bdnz 1b", {"r3": 5}, {"r5": 10, "ctr": 0}),
    ("branch_cond", "cmpwi r3,0\n beq 1f\n li r5,1\n b 2f\n1: li r5,2\n2: nop", {"r3": 0}, {"r5": 2}),
    ("bl_and_link", "mflr r9\n bl 1f\n li r5,7\n b 2f\n1: mflr r6\n li r5,3\n blr\n2: mtlr r9", {}, {"r5": 7}),
    ("cr_logic", "crset 0\n crclr 1\n cror 2,0,1\n crand 3,0,1\n crxor 4,0,2\n mfcr r5", {"cr": 0}, {"r5": 0xA0000000}),
    ("mtcrf_mcrf", "mtcrf 0x81,r3\n mcrf cr1,cr7\n mfcr r5", {"r3": 0x12345678, "cr": 0}, {"r5": 0x18000008}),
    ("loads_stores", "stw r3,0(r10)\n sth r3,4(r10)\n stb r3,6(r10)\n lwz r5,0(r10)\n lhz r6,4(r10)\n lha r7,4(r10)\n lbz r8,6(r10)\n lwzu r9,4(r10)",
     {"r3": 0x8765C3E1, "r10": 0x60000000},
     {"r5": 0x8765C3E1, "r6": 0xC3E1, "r7": 0xFFFFC3E1, "r8": 0xE1, "r9": 0xC3E1E100, "r10": 0x60000004}),
    ("byte_reverse", "stwbrx r3,0,r10\n lwz r5,0(r10)\n lhbrx r6,0,r10", {"r3": 0x11223344, "r10": 0x60000000},
     {"r5": 0x44332211, "r6": 0x3344}),
    ("multiple", "stmw r28,0(r10)\n li r28,0\n li r29,0\n lmw r28,0(r10)",
     {"r28": 1, "r29": 2, "r30": 3, "r31": 4, "r10": 0x60000000}, {"r28": 1, "r29": 2, "m60000008": 3, "m6000000C": 4}),
    ("lwarx_stwcx", "lwarx r5,0,r10\n addi r5,r5,1\n stwcx. r5,0,r10\n lwz r6,0(r10)\n stwcx. r5,0,r10",
     {"r10": 0x60000000, "m60000000": 41}, {"r6": 42, "cr": 0}),
    ("dcbz", "li r3,-1\n stw r3,0(r10)\n stw r3,28(r10)\n stw r3,32(r10)\n dcbz 0,r10\n lwz r5,0(r10)\n lwz r6,28(r10)\n lwz r7,32(r10)",
     {"r10": 0x60000000}, {"r5": 0, "r6": 0, "r7": M}),
    ("fp_basic", "stw r3,0(r10)\n stw r4,4(r10)\n lfd f1,0(r10)\n fadd f2,f1,f1\n fmul f3,f2,f1\n fsub f4,f3,f1\n fdiv f5,f4,f2\n stfd f5,8(r10)\n lwz r5,8(r10)\n lwz r6,12(r10)",
     # 3.0: 2.0*3 = 6? f2=6.0, f3=18.0, f4=15.0, f5=2.5 -> 0x40040000 00000000
     {"r3": 0x40080000, "r4": 0, "r10": 0x60000000}, {"r5": 0x40040000, "r6": 0}),
    ("fp_convert", "stw r3,0(r10)\n stw r4,4(r10)\n lfd f1,0(r10)\n fctiwz f2,f1\n stfd f2,8(r10)\n lwz r5,12(r10)\n frsp f3,f1\n stfs f3,16(r10)\n lwz r6,16(r10)",
     # -2.75 -> fctiwz -2 ; single 0xC0300000
     {"r3": 0xC0060000, "r4": 0, "r10": 0x60000000}, {"r5": 0xFFFFFFFE, "r6": 0xC0300000}),
    ("fp_compare", "stw r3,0(r10)\n stw r4,4(r10)\n lfd f1,0(r10)\n fcmpu cr1,f1,f2\n fneg f3,f1\n fabs f4,f3\n fcmpu cr2,f4,f1\n mfcr r5",
     {"r3": 0x3FF00000, "r4": 0, "r10": 0x60000000, "cr": 0}, {"r5": 0x04200000}),
    ("fp_madd_single", "lis r3,0x4040\n stw r3,0(r10)\n lfs f1,0(r10)\n fmadds f2,f1,f1,f1\n stfs f2,4(r10)\n lwz r5,4(r10)",
     # 3*3+3 = 12 -> 0x41400000
     {"r10": 0x60000000}, {"r5": 0x41400000}),
]

def assemble(src):
    with tempfile.TemporaryDirectory() as d:
        s = os.path.join(d, "t.s"); o = os.path.join(d, "t.o"); b = os.path.join(d, "t.bin")
        open(s, "w").write(".text\n.globl _t\n_t:\n " + src + "\n blr\n")
        subprocess.run([TOOL + "as", "-mregnames", "-many", "-o", o, s], check=True)
        subprocess.run([TOOL + "objcopy", "-O", "binary", "-j", ".text", o, b], check=True)
        data = open(b, "rb").read()
    return [struct.unpack(">I", data[i:i + 4])[0] for i in range(0, len(data), 4)]

def key(k):
    return k

lines = ["// Generated by Tests/PPC/gen_tests.py: PowerPC interpreter cases.", "",
         "struct PPCCase {", "    let name: String", "    let code: [UInt32]",
         "    let start: [(String, UInt32)]", "    let expect: [(String, UInt32)]", "}", "",
         "let ppcCases: [PPCCase] = ["]
for name, asm, start, expect in CASES:
    words = assemble(asm.replace("\n", "\n"))
    w = ", ".join("0x%08X" % x for x in words)
    st = ", ".join('("%s", 0x%08X)' % (k, v & M) for k, v in start.items())
    ex = ", ".join('("%s", 0x%08X)' % (k, v & M) for k, v in expect.items())
    lines.append('    PPCCase(name: "%s", code: [%s],' % (name, w))
    lines.append("            start: [%s]," % st)
    lines.append("            expect: [%s])," % ex)
lines.append("]")
out = os.path.join(os.path.dirname(__file__), "..", "ToolboxTests", "PPCVectors.swift")
open(out, "w").write("\n".join(lines) + "\n")
print("wrote", len(CASES), "cases")
