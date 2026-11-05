#!/usr/bin/env python3
"""dc-core SH-4 -> C static recompiler.

Translates a Dreamcast SH-4 executable (raw binary, e.g. 1ST_READ.BIN) into C functions
that run against the runtime in runtime/include/dccore/sh4.h.

    sh4recomp.py BIN --base 0x8C010000 --funcs funcs.txt --out gen/

funcs.txt: one function start address per line (hex, `fn_8c0xxxxx` names are accepted).
Every function is translated from its start up to the next start. A function returns 0 when the
guest returns (rts/rte) or the address to continue at for a jump that leaves it (tail calls,
fall-through into the next function); dc_call() runs that chain as a trampoline, so only real
calls (bsr/jsr) use the C stack. Each 16-bit slot gets a
label, so any in-function branch target is reachable; branches that leave the function
become tail calls through dc_call(). Generated code never contains game data except
literal-pool constants folded into instructions, and it is meant to be produced on the
player's machine from their own disc, not distributed.
"""
import argparse, bisect, os, re, struct, sys

def s8(v): return v - 0x100 if v & 0x80 else v
def s12(v): return v - 0x1000 if v & 0x800 else v
def h(v): return "0x%08Xu" % (v & 0xFFFFFFFF)

R = lambda n: "c->r[%d]" % n
FR = lambda n: "c->fr.f[%d]" % n
FU = lambda n: "c->fr.u[%d]" % n

DELAYED = set()  # filled by decode: mnemonics that have a delay slot


class Insn:
    __slots__ = ("addr", "op", "kind", "code", "target", "cond", "delayed", "reg", "text")

    def __init__(self, addr, op):
        self.addr = addr; self.op = op
        self.kind = "op"        # op | bra | bsr | cbr | jmp | jsr | rts | braf | bsrf | rte | bad
        self.code = ""          # C statements for plain ops
        self.target = None      # static branch target
        self.cond = None        # for cbr: "t" / "!t"
        self.delayed = False
        self.reg = None         # for register-indirect branches
        self.text = ""


class Translator:
    def __init__(self, data, base):
        self.data = data; self.base = base; self.end = base + len(data)
        self.volatile = self._volatile_literals()
        self.setjmp = set()          # the game's setjmp routine(s): calls get DC_CALL_SETJMP
        self.prefix = ""             # symbol prefix (a second executable: PREFIX_dc_fn_table, f_PREFIX_...)

    def _volatile_literals(self):
        """Literal-pool words that code can write: some routines keep variables in their own pool
        (Rush 2049's interrupt stub saves r0 with mova + mov.l and reloads it from the same slot).
        A pool slot counts as writable when an address pointing at it can be produced -- a mova
        target, or a pool constant that holds the address of another pool slot. Loads from those
        slots are read at run time instead of folded. The scan covers every halfword, so data that
        happens to decode as mova only costs a runtime read of a constant."""
        n = len(self.data) // 2
        ops = struct.unpack_from("<%dH" % n, self.data, 0)
        slots, vol = set(), set()
        for k, op in enumerate(ops):
            a = self.base + k * 2
            top = op >> 12
            if top == 0xC and (op >> 8) & 0xF == 7:          # mova @(disp,PC),r0
                vol.add(((a + 4) & ~3) + (op & 0xFF) * 4)
            elif top == 0xD:                                   # mov.l @(disp,PC),Rn
                slots.add(((a + 4) & ~3) + (op & 0xFF) * 4)
        for pa in slots:
            v = self.u32(pa)
            if v is not None:
                v = (v & 0x1FFFFFFF) | (self.base & 0xE0000000)
                if v in slots: vol.add(v)
        return vol

    def u16(self, a):
        o = a - self.base
        return struct.unpack_from("<H", self.data, o)[0] if 0 <= o <= len(self.data) - 2 else None

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from("<I", self.data, o)[0] if 0 <= o <= len(self.data) - 4 else None

    # ------------------------------------------------------------------ decode
    def decode(self, a):
        op = self.u16(a)
        i = Insn(a, op)
        if op is None:
            i.kind = "bad"; i.code = 'dc_trap(c, %s, "out of image");' % h(a); return i
        n = (op >> 8) & 0xF; m = (op >> 4) & 0xF; lo = op & 0xF; lb = op & 0xFF
        top = op >> 12
        e = lambda s, t="": self._op(i, s, t)

        if top == 0x0:
            if lo == 0x2:
                if m & 8: return e("%s = c->rbank[%d];" % (R(n), m & 7), "stc rbank")
                src = {0: "sh4_get_sr(c)", 1: "c->gbr", 2: "c->vbr", 3: "c->ssr", 4: "c->spc"}.get(m)
                if src: return e("%s = %s;" % (R(n), src), "stc")
            if lo == 0x3:
                if m == 0: i.kind = "bsrf"; i.reg = n; i.delayed = True; return i
                if m == 2: i.kind = "braf"; i.reg = n; i.delayed = True; return i
                if m == 8: return e("dc_hw_pref(%s);" % R(n), "pref")
                if m in (9, 0xA, 0xB): return e("/* cache op */", "ocb")
                if m == 0xC: return e("dc_wr32(%s, %s);" % (R(n), R(0)), "movca.l")
            if lo == 0x4: return e("dc_wr8(%s + %s, (uint8_t)%s);" % (R(n), R(0), R(m)))
            if lo == 0x5: return e("dc_wr16(%s + %s, (uint16_t)%s);" % (R(n), R(0), R(m)))
            if lo == 0x6: return e("dc_wr32(%s + %s, %s);" % (R(n), R(0), R(m)))
            if lo == 0x7: return e("c->macl = %s * %s;" % (R(n), R(m)), "mul.l")
            if lo == 0x8 and n == 0:
                t = {0: "c->t = 0;", 1: "c->t = 1;", 2: "c->mach = c->macl = 0;", 4: "c->s = 0;", 5: "c->s = 1;"}.get(m)
                if t: return e(t)
                if m == 3: return e("", "ldtlb")   # no MMU model: TLB loads (a dev-kit monitor's) do nothing
            if lo == 0x9:
                if op == 0x0009: return e("", "nop")
                if op == 0x0019: return e("c->m = c->q = c->t = 0;", "div0u")
                if m == 2: return e("%s = c->t;" % R(n), "movt")
            if lo == 0xA:
                src = {0: "c->mach", 1: "c->macl", 2: "c->pr", 3: "c->sgr", 5: "c->fpul", 6: "c->fpscr", 0xF: "c->dbr"}.get(m)
                if src: return e("%s = %s;" % (R(n), src))
            if lo == 0xB:
                if op == 0x000B: i.kind = "rts"; i.delayed = True; return i
                if op == 0x001B: return e('dc_trap(c, %s, "sleep");' % h(a))
                if op == 0x002B: i.kind = "rte"; i.delayed = True; return i
            if lo == 0xC: return e("%s = (uint32_t)(int8_t)dc_rd8(%s + %s);" % (R(n), R(m), R(0)))
            if lo == 0xD: return e("%s = (uint32_t)(int16_t)dc_rd16(%s + %s);" % (R(n), R(m), R(0)))
            if lo == 0xE: return e("%s = dc_rd32(%s + %s);" % (R(n), R(m), R(0)))
            if lo == 0xF: return e("sh4_macl(c, %d, %d);" % (n, m), "mac.l")
        elif top == 0x1:
            return e("dc_wr32(%s + %d, %s);" % (R(n), lo * 4, R(m)))
        elif top == 0x2:
            t = {
                0: "dc_wr8(%s, (uint8_t)%s);" % (R(n), R(m)),
                1: "dc_wr16(%s, (uint16_t)%s);" % (R(n), R(m)),
                2: "dc_wr32(%s, %s);" % (R(n), R(m)),
                4: "{ uint32_t v = %s; %s -= 1; dc_wr8(%s, (uint8_t)v); }" % (R(m), R(n), R(n)),
                5: "{ uint32_t v = %s; %s -= 2; dc_wr16(%s, (uint16_t)v); }" % (R(m), R(n), R(n)),
                6: "{ uint32_t v = %s; %s -= 4; dc_wr32(%s, v); }" % (R(m), R(n), R(n)),
                7: "c->q = %s >> 31; c->m = %s >> 31; c->t = c->q ^ c->m;" % (R(n), R(m)),
                8: "c->t = (%s & %s) == 0;" % (R(n), R(m)),
                9: "%s &= %s;" % (R(n), R(m)),
                0xA: "%s ^= %s;" % (R(n), R(m)),
                0xB: "%s |= %s;" % (R(n), R(m)),
                0xC: "{ uint32_t x = %s ^ %s; c->t = !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000u); }" % (R(n), R(m)),
                0xD: "%s = (%s >> 16) | (%s << 16);" % (R(n), R(n), R(m)),
                0xE: "c->macl = (uint32_t)(uint16_t)%s * (uint16_t)%s;" % (R(n), R(m)),
                0xF: "c->macl = (uint32_t)((int32_t)(int16_t)%s * (int16_t)%s);" % (R(n), R(m)),
            }.get(lo)
            if t: return e(t)
        elif top == 0x3:
            rn, rm = R(n), R(m)
            t = {
                0: "c->t = %s == %s;" % (rn, rm),
                2: "c->t = %s >= %s;" % (rn, rm),
                3: "c->t = (int32_t)%s >= (int32_t)%s;" % (rn, rm),
                4: "sh4_div1(c, %d, %d);" % (n, m),
                5: "{ uint64_t p = (uint64_t)%s * %s; c->mach = (uint32_t)(p >> 32); c->macl = (uint32_t)p; }" % (rn, rm),
                6: "c->t = %s > %s;" % (rn, rm),
                7: "c->t = (int32_t)%s > (int32_t)%s;" % (rn, rm),
                8: "%s -= %s;" % (rn, rm),
                0xA: "{ uint32_t a = %s, b = %s, r = a - b - c->t; c->t = (a < b) || (a - b < c->t); %s = r; }" % (rn, rm, rn),
                0xB: "{ int32_t a = (int32_t)%s, b = (int32_t)%s; int32_t r = (int32_t)((uint32_t)a - (uint32_t)b); c->t = ((a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }" % (rn, rm, rn),
                0xC: "%s += %s;" % (rn, rm),
                0xD: "{ int64_t p = (int64_t)(int32_t)%s * (int32_t)%s; c->mach = (uint32_t)((uint64_t)p >> 32); c->macl = (uint32_t)p; }" % (rn, rm),
                0xE: "{ uint32_t a = %s, b = %s, r = a + b + c->t; c->t = (r < a) || (r == a && c->t); %s = r; }" % (rn, rm, rn),
                0xF: "{ int32_t a = (int32_t)%s, b = (int32_t)%s; int32_t r = (int32_t)((uint32_t)a + (uint32_t)b); c->t = (~(a ^ b) & (a ^ r)) >> 31 & 1; %s = (uint32_t)r; }" % (rn, rm, rn),
            }.get(lo)
            if t: return e(t)
        elif top == 0x4:
            rn = R(n)
            if lo == 0xC: return e("{ int32_t s = (int32_t)%s; uint32_t v = %s; if (s >= 0) %s = v << (s & 31); else if ((s & 31) == 0) %s = (uint32_t)((int32_t)v >> 31); else %s = (uint32_t)((int32_t)v >> ((-s) & 31)); }" % (R(m), rn, rn, rn, rn), "shad")
            if lo == 0xD: return e("{ int32_t s = (int32_t)%s; uint32_t v = %s; if (s >= 0) %s = v << (s & 31); else if ((s & 31) == 0) %s = 0; else %s = v >> ((-s) & 31); }" % (R(m), rn, rn, rn, rn), "shld")
            if lo == 0xF: return e("sh4_macw(c, %d, %d);" % (n, m), "mac.w")
            if lb == 0x0B: i.kind = "jsr"; i.reg = n; i.delayed = True; return i
            if lb == 0x2B: i.kind = "jmp"; i.reg = n; i.delayed = True; return i
            # banked control regs
            if (lb & 0x8F) == 0x83: return e("%s -= 4; dc_wr32(%s, c->rbank[%d]);" % (rn, rn, m & 7))
            if (lb & 0x8F) == 0x87: return e("c->rbank[%d] = dc_rd32(%s); %s += 4;" % (m & 7, rn, rn))
            if (lb & 0x8F) == 0x8E: return e("c->rbank[%d] = %s;" % (m & 7, rn))
            push = lambda src: "%s -= 4; dc_wr32(%s, %s);" % (rn, rn, src)
            pop = lambda dst: "%s = dc_rd32(%s); %s += 4;" % (dst, rn, rn)
            t = {
                0x00: "c->t = %s >> 31; %s <<= 1;" % (rn, rn),
                0x01: "c->t = %s & 1; %s >>= 1;" % (rn, rn),
                0x02: push("c->mach"), 0x12: push("c->macl"), 0x22: push("c->pr"),
                0x03: push("sh4_get_sr(c)"), 0x13: push("c->gbr"), 0x23: push("c->vbr"),
                0x32: push("c->sgr"), 0x33: push("c->ssr"), 0x43: push("c->spc"),
                0x52: push("c->fpul"), 0x62: push("c->fpscr"), 0xF2: push("c->dbr"),
                0x04: "c->t = %s >> 31; %s = (%s << 1) | c->t;" % (rn, rn, rn),
                0x05: "c->t = %s & 1; %s = (%s >> 1) | (c->t << 31);" % (rn, rn, rn),
                0x06: pop("c->mach"), 0x16: pop("c->macl"), 0x26: pop("c->pr"),
                0x07: "sh4_set_sr(c, dc_rd32(%s)); %s += 4;" % (rn, rn),
                0x17: pop("c->gbr"), 0x27: pop("c->vbr"), 0x37: pop("c->ssr"), 0x47: pop("c->spc"),
                0x56: pop("c->fpul"), 0xF6: pop("c->dbr"),
                0x66: "sh4_set_fpscr(c, dc_rd32(%s)); %s += 4;" % (rn, rn),
                0x08: "%s <<= 2;" % rn, 0x09: "%s >>= 2;" % rn,
                0x18: "%s <<= 8;" % rn, 0x19: "%s >>= 8;" % rn,
                0x28: "%s <<= 16;" % rn, 0x29: "%s >>= 16;" % rn,
                0x0A: "c->mach = %s;" % rn, 0x1A: "c->macl = %s;" % rn, 0x2A: "c->pr = %s;" % rn,
                0x5A: "c->fpul = %s;" % rn, 0x6A: "sh4_set_fpscr(c, %s);" % rn, 0xFA: "c->dbr = %s;" % rn,
                0x0E: "sh4_set_sr(c, %s);" % rn, 0x1E: "c->gbr = %s;" % rn, 0x2E: "c->vbr = %s;" % rn,
                0x3E: "c->ssr = %s;" % rn, 0x4E: "c->spc = %s;" % rn,
                0x10: "%s -= 1; c->t = %s == 0;" % (rn, rn),
                0x11: "c->t = (int32_t)%s >= 0;" % rn,
                0x15: "c->t = (int32_t)%s > 0;" % rn,
                0x1B: "{ uint8_t v = dc_rd8(%s); c->t = v == 0; dc_wr8(%s, v | 0x80); }" % (rn, rn),
                0x20: "c->t = %s >> 31; %s <<= 1;" % (rn, rn),
                0x21: "c->t = %s & 1; %s = (uint32_t)((int32_t)%s >> 1);" % (rn, rn, rn),
                0x24: "{ uint32_t o = c->t; c->t = %s >> 31; %s = (%s << 1) | o; }" % (rn, rn, rn),
                0x25: "{ uint32_t o = c->t; c->t = %s & 1; %s = (%s >> 1) | (o << 31); }" % (rn, rn, rn),
            }.get(lb)
            if t: return e(t)
        elif top == 0x5:
            return e("%s = dc_rd32(%s + %d);" % (R(n), R(m), lo * 4))
        elif top == 0x6:
            rn, rm = R(n), R(m)
            t = {
                0: "%s = (uint32_t)(int8_t)dc_rd8(%s);" % (rn, rm),
                1: "%s = (uint32_t)(int16_t)dc_rd16(%s);" % (rn, rm),
                2: "%s = dc_rd32(%s);" % (rn, rm),
                3: "%s = %s;" % (rn, rm),
                4: "{ uint32_t a = %s; %s += 1; %s = (uint32_t)(int8_t)dc_rd8(a); }" % (rm, rm, rn),
                5: "{ uint32_t a = %s; %s += 2; %s = (uint32_t)(int16_t)dc_rd16(a); }" % (rm, rm, rn),
                6: "{ uint32_t a = %s; %s += 4; %s = dc_rd32(a); }" % (rm, rm, rn),
                7: "%s = ~%s;" % (rn, rm),
                8: "%s = (%s & 0xFFFF0000u) | ((%s & 0xFF) << 8) | ((%s >> 8) & 0xFF);" % (rn, rm, rm, rm),
                9: "%s = (%s >> 16) | (%s << 16);" % (rn, rm, rm),
                0xA: "{ uint32_t t0 = 0 - %s; uint32_t r = t0 - c->t; c->t = (0 < t0) || (t0 < r); %s = r; }" % (rm, rn),
                0xB: "%s = 0 - %s;" % (rn, rm),
                0xC: "%s = %s & 0xFF;" % (rn, rm),
                0xD: "%s = %s & 0xFFFF;" % (rn, rm),
                0xE: "%s = (uint32_t)(int8_t)%s;" % (rn, rm),
                0xF: "%s = (uint32_t)(int16_t)%s;" % (rn, rm),
            }.get(lo)
            # mov.x @Rm+,Rn with n == m: the loaded value wins (increment is discarded)
            if lo in (4, 5, 6) and n == m:
                sz = {4: "(uint32_t)(int8_t)dc_rd8", 5: "(uint32_t)(int16_t)dc_rd16", 6: "dc_rd32"}[lo]
                t = "%s = %s(%s);" % (rn, sz, rn)
            if t: return e(t)
        elif top == 0x7:
            return e("%s += %s;" % (R(n), h(s8(lb))))
        elif top == 0x8:
            d = lo
            if n == 0x0: return e("dc_wr8(%s + %d, (uint8_t)%s);" % (R(m), d, R(0)))
            if n == 0x1: return e("dc_wr16(%s + %d, (uint16_t)%s);" % (R(m), d * 2, R(0)))
            if n == 0x4: return e("%s = (uint32_t)(int8_t)dc_rd8(%s + %d);" % (R(0), R(m), d))
            if n == 0x5: return e("%s = (uint32_t)(int16_t)dc_rd16(%s + %d);" % (R(0), R(m), d * 2))
            if n == 0x8: return e("c->t = %s == %s;" % (R(0), h(s8(lb))))
            if n in (0x9, 0xB, 0xD, 0xF):
                i.kind = "cbr"; i.target = a + 4 + s8(lb) * 2
                i.cond = "c->t" if n in (0x9, 0xD) else "!c->t"
                i.delayed = n in (0xD, 0xF)
                return i
        elif top == 0x9:
            v = self.u16(a + 4 + lb * 2)
            if v is None: return e('dc_trap(c, %s, "pc-rel out of image");' % h(a))
            if (a + 4 + lb * 2) & ~3 in self.volatile:
                return e("%s = (uint32_t)(int16_t)dc_rd16(%s);" % (R(n), h(a + 4 + lb * 2)))
            return e("%s = %s; /* @%08X */" % (R(n), h(v - 0x10000 if v & 0x8000 else v), a + 4 + lb * 2))
        elif top == 0xA:
            i.kind = "bra"; i.target = a + 4 + s12(op & 0xFFF) * 2; i.delayed = True; return i
        elif top == 0xB:
            i.kind = "bsr"; i.target = a + 4 + s12(op & 0xFFF) * 2; i.delayed = True; return i
        elif top == 0xC:
            g = "c->gbr"
            t = {
                0x0: "dc_wr8(%s + %d, (uint8_t)%s);" % (g, lb, R(0)),
                0x1: "dc_wr16(%s + %d, (uint16_t)%s);" % (g, lb * 2, R(0)),
                0x2: "dc_wr32(%s + %d, %s);" % (g, lb * 4, R(0)),
                0x3: 'dc_trap(c, %s, "trapa");' % h(a),
                0x4: "%s = (uint32_t)(int8_t)dc_rd8(%s + %d);" % (R(0), g, lb),
                0x5: "%s = (uint32_t)(int16_t)dc_rd16(%s + %d);" % (R(0), g, lb * 2),
                0x6: "%s = dc_rd32(%s + %d);" % (R(0), g, lb * 4),
                0x7: "%s = %s;" % (R(0), h(((a + 4) & ~3) + lb * 4)),
                0x8: "c->t = (%s & %du) == 0;" % (R(0), lb),
                0x9: "%s &= %du;" % (R(0), lb),
                0xA: "%s ^= %du;" % (R(0), lb),
                0xB: "%s |= %du;" % (R(0), lb),
                0xC: "c->t = (dc_rd8(%s + c->gbr) & %du) == 0;" % (R(0), lb),
                0xD: "{ uint32_t a_ = %s + c->gbr; dc_wr8(a_, dc_rd8(a_) & %du); }" % (R(0), lb),
                0xE: "{ uint32_t a_ = %s + c->gbr; dc_wr8(a_, dc_rd8(a_) ^ %du); }" % (R(0), lb),
                0xF: "{ uint32_t a_ = %s + c->gbr; dc_wr8(a_, dc_rd8(a_) | %du); }" % (R(0), lb),
            }.get(n)
            if t: return e(t)
        elif top == 0xD:
            pa = ((a + 4) & ~3) + lb * 4
            v = self.u32(pa)
            if v is None or pa in self.volatile: return e("%s = dc_rd32(%s);" % (R(n), h(pa)))
            return e("%s = %s; /* @%08X */" % (R(n), h(v), pa))
        elif top == 0xE:
            return e("%s = %s;" % (R(n), h(s8(lb))))
        elif top == 0xF:
            return self.decode_fpu(i, n, m, lo, lb, a)
        return self._bad(i)

    def _op(self, i, code, text=""):
        i.kind = "op"; i.code = code; i.text = text; return i

    def _bad(self, i):
        i.kind = "bad"; i.code = 'dc_trap(c, %s, "illegal %04X");' % (h(i.addr), i.op); return i

    def decode_fpu(self, i, n, m, lo, lb, a):
        e = lambda s, t="": self._op(i, s, t)
        PR = "(c->fpscr & FPSCR_PR)"
        SZ = "(c->fpscr & FPSCR_SZ)"
        def dual(single, double): return "if (%s) { %s } else { %s }" % (PR, double, single)
        def arith(sym):
            return dual("%s %s= %s;" % (FR(n), sym, FR(m)),
                        "sh4_set_dr(c, %d, sh4_get_dr(c, %d) %s sh4_get_dr(c, %d));" % (n & 0xE, n & 0xE, sym, m & 0xE))
        if lo == 0x0: return e(arith("+"), "fadd")
        if lo == 0x1: return e(arith("-"), "fsub")
        if lo == 0x2: return e(arith("*"), "fmul")
        if lo == 0x3: return e(arith("/"), "fdiv")
        if lo == 0x4: return e(dual("c->t = %s == %s;" % (FR(n), FR(m)), "c->t = sh4_get_dr(c, %d) == sh4_get_dr(c, %d);" % (n & 0xE, m & 0xE)), "fcmp/eq")
        if lo == 0x5: return e(dual("c->t = %s > %s;" % (FR(n), FR(m)), "c->t = sh4_get_dr(c, %d) > sh4_get_dr(c, %d);" % (n & 0xE, m & 0xE)), "fcmp/gt")
        # fmov family: SZ=0 -> 32-bit, SZ=1 -> 64-bit pair
        xn, xm = "sh4_xd(%d)" % n, "sh4_xd(%d)" % m
        def ld(addr):
            return ("if (%s) { int d = %s; uint32_t a_ = %s; c->fr.u[d] = dc_rd32(a_); c->fr.u[d+1] = dc_rd32(a_ + 4); } "
                    "else { %s = dc_rd32(%s); }") % (SZ, xn, addr, FU(n), addr)
        def st(addr):
            return ("if (%s) { int s_ = %s; uint32_t a_ = %s; dc_wr32(a_, c->fr.u[s_]); dc_wr32(a_ + 4, c->fr.u[s_+1]); } "
                    "else { dc_wr32(%s, %s); }") % (SZ, xm, addr, addr, FU(m))
        if lo == 0x6: return e(ld("%s + %s" % (R(m), R(0))), "fmov @(r0,rm)")
        if lo == 0x7: return e(st("%s + %s" % (R(n), R(0))), "fmov ->@(r0,rn)")
        if lo == 0x8: return e(ld(R(m)), "fmov @rm")
        if lo == 0x9:
            return e(("{ uint32_t a0 = %s; " % R(m)) + ld("a0") + (" %s += (%s) ? 8 : 4; }" % (R(m), SZ)), "fmov @rm+")
        if lo == 0xA: return e(st(R(n)), "fmov ->@rn")
        if lo == 0xB:
            return e(("{ %s -= (%s) ? 8 : 4; uint32_t a0 = %s; " % (R(n), SZ, R(n))) + st("a0") + " }", "fmov ->@-rn")
        if lo == 0xC:
            return e("if (%s) { int d = %s, s_ = %s; uint32_t x0 = c->fr.u[s_], x1 = c->fr.u[s_+1]; c->fr.u[d] = x0; c->fr.u[d+1] = x1; } else { %s = %s; }"
                     % (SZ, xn, xm, FU(n), FU(m)), "fmov")
        if lo == 0xE: return e("%s = fmaf(%s, %s, %s);" % (FR(n), FR(0), FR(m), FR(n)), "fmac (fused)")
        if lo == 0xD:
            if m == 0x0: return e("%s = c->fpul;" % FU(n), "fsts")
            if m == 0x1: return e("c->fpul = %s;" % FU(n), "flds")
            if m == 0x2: return e(dual("%s = (float)(int32_t)c->fpul;" % FR(n), "sh4_set_dr(c, %d, (double)(int32_t)c->fpul);" % (n & 0xE)), "float")
            if m == 0x3: return e(dual("c->fpul = sh4_ftrc(%s);" % FR(n), "c->fpul = sh4_ftrc_d(sh4_get_dr(c, %d));" % (n & 0xE)), "ftrc")
            if m == 0x4: return e(dual("%s = -%s;" % (FR(n), FR(n)), "c->fr.u[%d] ^= 0x80000000u;" % (n & 0xE)), "fneg")
            if m == 0x5: return e(dual("%s = fabsf(%s);" % (FR(n), FR(n)), "c->fr.u[%d] &= 0x7FFFFFFFu;" % (n & 0xE)), "fabs")
            if m == 0x6: return e(dual("%s = sqrtf(%s);" % (FR(n), FR(n)), "sh4_set_dr(c, %d, sqrt(sh4_get_dr(c, %d)));" % (n & 0xE, n & 0xE)), "fsqrt")
            if m == 0x7: return e("%s = 1.0f / sqrtf(%s);" % (FR(n), FR(n)), "fsrra")
            if m == 0x8: return e("%s = 0x00000000u;" % FU(n), "fldi0")
            if m == 0x9: return e("%s = 0x3F800000u;" % FU(n), "fldi1")
            if m == 0xA: return e("{ float f_; memcpy(&f_, &c->fpul, 4); sh4_set_dr(c, %d, (double)f_); }" % (n & 0xE), "fcnvsd")
            if m == 0xB: return e("{ float f_ = (float)sh4_get_dr(c, %d); memcpy(&c->fpul, &f_, 4); }" % (n & 0xE), "fcnvds")
            if m == 0xE: return e("sh4_fipr(c, %d, %d);" % (n & 0xC, (n & 3) << 2), "fipr")
            if m == 0xF:
                if (n & 1) == 0: return e("sh4_fsca(c, %d);" % n, "fsca")
                if (n & 3) == 1: return e("sh4_ftrv(c, %d);" % (n & 0xC), "ftrv")
                if n == 0xB: return e("sh4_set_fpscr(c, c->fpscr ^ FPSCR_FR);", "frchg")
                if n == 0x3: return e("c->fpscr ^= FPSCR_SZ;", "fschg")
                if n == 0x7: return e("c->fpscr ^= FPSCR_PR;", "fpchg")
        return self._bad(i)

    # ------------------------------------------------------------------ emit
    def emit_function(self, start, end, known, out):
        """known: sorted list of function starts (for direct calls)."""
        name = "f_%s%08X" % (self.prefix, start)
        out.append("uint32_t %s(sh4ctx *c) {" % name)
        out.append("  if (c->entry != %s) { jt = c->entry; goto local_jump; }" % h(start))
        insns = {}
        a = start
        while a < end:
            insns[a] = self.decode(a); a += 2
        needs_local_jump = True   # computed jumps and mid-function entries both use the switch
        for a in range(start, end, 2):
            i = insns[a]
            out.append("L_%08X:" % a)
            out.append("  " + self.emit_insn(i, insns, start, end) )
        # falling off the end continues in the next function
        out.append("  return %s;" % h(end))
        if needs_local_jump:
            out.append("local_jump:")
            out.append("  switch (jt) {")
            for a in range(start, end, 2):
                out.append("    case %s: goto L_%08X;" % (h(a), a))
            out.append("  }")
            out.append("  return jt;")
        out.append("}")
        # declare jt at top if needed
        if needs_local_jump:
            idx = len(out) - 1
            # insert after function header
            for k in range(idx, -1, -1):
                if out[k].startswith("uint32_t %s(" % name):
                    out.insert(k + 1, "  uint32_t jt = 0; (void)jt;")
                    break

    def slot_code(self, i, insns):
        s = insns.get(i.addr + 2) or self.decode(i.addr + 2)
        if s.kind != "op":
            if s.kind == "bad" and self.u16(i.addr + 2) == 0x0000:
                return "/* 0000 in the delay slot: a no-op on the hardware */"
            if s.kind == "bad":
                return s.code
            return 'dc_trap(c, %s, "branch in delay slot");' % h(s.addr)
        return s.code

    def goto_or_call(self, t, start, end, here=None):
        if start <= t < end and (t & 1) == 0:
            if here is not None and t <= here:          # loop: let time pass / interrupts in
                return "{ DC_TICK(c, dc_loop_cost); goto L_%08X; }" % t
            return "goto L_%08X;" % t
        return "return %s;" % h(t)

    def emit_insn(self, i, insns, start, end):
        k = i.kind
        if k in ("op", "bad"):
            return i.code + ((" /* %s */" % i.text) if i.text else "")
        a = i.addr
        nxt = a + 4
        if k == "cbr":
            if not i.delayed:
                return "if (%s) %s" % (i.cond, self.goto_or_call(i.target, start, end, a))
            slot = self.slot_code(i, insns)
            return "{ int cc = %s; %s if (cc) %s goto L_%08X; }" % (
                i.cond, slot, self.goto_or_call(i.target, start, end, a),
                nxt) if nxt < end else "{ int cc = %s; %s if (cc) %s return %s; }" % (
                i.cond, slot, self.goto_or_call(i.target, start, end), h(nxt))
        slot = self.slot_code(i, insns)
        if k == "bra":
            return "{ %s %s }" % (slot, self.goto_or_call(i.target, start, end, a))
        if k == "bsr":
            if i.target in self.setjmp:
                return "{ c->pr = %s; %s DC_CALL_SETJMP(c, %s); %s }" % (h(nxt), slot, h(i.target), self._after_call(nxt, end))
            # a fixed target: the call site keeps what it resolves to (dc_call_site)
            return "{ static dc_site s_; c->pr = %s; %s dc_call_site(c, %s, &s_); %s }" % (h(nxt), slot, h(i.target), self._after_call(nxt, end))
        if k == "jsr":
            v = self._reg_const(insns, a, i.reg, start)
            if self.setjmp and v in self.setjmp:
                return "{ uint32_t t_ = %s; c->pr = %s; %s DC_CALL_SETJMP(c, t_); %s }" % (R(i.reg), h(nxt), slot, self._after_call(nxt, end))
            if v is not None:                           # register just loaded with a constant: checked, then cached
                return "{ static dc_site s_; uint32_t t_ = %s; c->pr = %s; %s if (((t_ & 0x1FFFFFFFu) | 0x80000000u) == %s) dc_call_site(c, %s, &s_); else dc_call(c, t_); %s }" % (
                    R(i.reg), h(nxt), slot, h(v), h(v), self._after_call(nxt, end))
            return "{ uint32_t t_ = %s; c->pr = %s; %s dc_call(c, t_); %s }" % (R(i.reg), h(nxt), slot, self._after_call(nxt, end))
        if k == "bsrf":
            return "{ uint32_t t_ = %s + %s; c->pr = %s; %s dc_call(c, t_); %s }" % (R(i.reg), h(nxt), h(nxt), slot, self._after_call(nxt, end))
        if k == "rts":
            return "{ %s return 0; }" % slot
        if k == "rte":
            # interrupts/exceptions are entered as calls from dc_events, so rte = restore SR + return
            return "{ sh4_set_sr(c, c->ssr); %s return 0; }" % slot
        if k == "jmp":
            return "{ jt = %s; %s goto local_jump; }" % (R(i.reg), slot)
        if k == "braf":
            return "{ jt = %s + %s; %s goto local_jump; }" % (R(i.reg), h(nxt), slot)
        raise ValueError(k)

    def _reg_const(self, insns, a, reg, start):
        """Value of reg at a if one of the 8 preceding instructions loads it from the literal pool
        (and nothing in between names it as a destination)."""
        b = a - 2
        while b >= max(start, a - 16):
            op = self.u16(b)
            if op is None: return None
            if (op >> 8) & 0xF == reg:
                if op >> 12 == 0xD:
                    v = self.u32(((b + 4) & ~3) + (op & 0xFF) * 4)
                    return None if v is None else (v & 0x1FFFFFFF) | 0x80000000
                return None
            b -= 2
        return None

    def _after_call(self, nxt, end):
        return "goto L_%08X;" % nxt if nxt < end else "return %s;" % h(nxt)


def flow_end(tr, s, cap=0x4000):
    """End of the guest function that starts at s, by control flow: the first rts (or jmp) past
    every branch target seen so far, plus its delay slot."""
    a = s; far = s
    while a < s + cap:
        op = tr.u16(a)
        if op is None: return a
        top = op >> 12
        if top == 0xA:
            d = op & 0xFFF; d = d - 0x1000 if d & 0x800 else d
            far = max(far, a + 4 + d * 2)
        elif top == 0x8 and ((op >> 8) & 0xF) in (0x9, 0xB, 0xD, 0xF):
            d = op & 0xFF; d = d - 0x100 if d & 0x80 else d
            far = max(far, a + 4 + d * 2)
        elif (op == 0x000B or (op & 0xF0FF) == 0x402B) and a >= far:
            return a + 4
        a += 2
    return a


def extend_setjmp_callers(tr, ranges):
    """A function that calls the game's setjmp must stay one host function until it returns: the
    guest longjmp resumes in the host frame of that call (DC_CALL_SETJMP), so the function may not
    leave its frame by a tail jump into a split-off part (a loop head that became a function
    start, for example). Such a function is extended to its end by control flow; the starts inside
    it remain functions of their own for jumps from outside."""
    if not tr.setjmp:
        return ranges
    sites = []
    a = tr.base
    while a < tr.end - 1:
        op = tr.u16(a)
        if op >> 12 == 0xB:
            d = op & 0xFFF; d = d - 0x1000 if d & 0x800 else d
            if a + 4 + d * 2 in tr.setjmp: sites.append(a)
        elif (op & 0xF0FF) == 0x400B and tr._reg_const(None, a, (op >> 8) & 0xF, tr.base) in tr.setjmp:
            sites.append(a)
        a += 2
    out = list(ranges)
    starts = [s for s, _ in ranges]
    import bisect
    for site in sites:
        k = bisect.bisect_right(starts, site) - 1
        if k < 0: continue
        s, e = out[k]
        fe = flow_end(tr, s)
        if fe > e and fe > site:
            out[k] = (s, fe)
    return out


FUNC_ENDS = {}   # start -> end, from funcs files that give one ("start end", e.g. from PE .pdata)


def read_funcs(path, base, end):
    out = set()
    for line in open(path):
        mm = re.search(r"(?:fn_|0x)?([0-9a-fA-F]{8})(?:\s+(?:0x)?([0-9a-fA-F]{8}))?", line)
        if mm:
            v = int(mm.group(1), 16)
            if base <= v < end and v % 2 == 0:
                out.add(v)
                if mm.group(2):
                    FUNC_ENDS[v] = int(mm.group(2), 16)
    return sorted(out)


def mirror(tr, v):
    """A code address the game may hold in P0 or P2 (0C......, AC......; Propeller Arena calls its
    library through 0C addresses) as the image's own segment (8C......)."""
    if v is None: return None
    if (v >> 29) in (0, 4, 5) and tr.base <= ((v & 0x1FFFFFFF) | (tr.base & 0xE0000000)) < tr.end:
        return (v & 0x1FFFFFFF) | (tr.base & 0xE0000000)
    return v


def discover(tr, funcs, rounds=4):
    """Add call targets the seed list missed: bsr targets and `mov.l @(disp,PC),Rn ... jsr @Rn`
    constants. Scans only inside existing functions, so data is mostly avoided."""
    funcs = set(funcs); end = tr.end
    for _ in range(rounds):
        new = set()
        starts = sorted(funcs)
        for k, s in enumerate(starts):
            e = starts[k + 1] if k + 1 < len(starts) else end
            e = min(e, s + 0x4000)
            lit = {}
            a = s
            while a < e:
                op = tr.u16(a)
                if op is None: break
                top = op >> 12
                if top == 0xD:
                    v = mirror(tr, tr.u32(((a + 4) & ~3) + (op & 0xFF) * 4))
                    lit[(op >> 8) & 0xF] = (v, a)
                elif top == 0xB:
                    d = op & 0xFFF; d = d - 0x1000 if d & 0x800 else d
                    new.add(a + 4 + d * 2)
                elif top == 0xA or (top == 0x8 and ((op >> 8) & 0xF) in (0x9, 0xB, 0xD, 0xF)):
                    # a branch that leaves its function must land on a function start
                    if top == 0xA:
                        d = op & 0xFFF; d = d - 0x1000 if d & 0x800 else d
                    else:
                        d = op & 0xFF; d = d - 0x100 if d & 0x80 else d
                    tgt = a + 4 + d * 2
                    if not (s <= tgt < e):
                        new.add(tgt)
                elif (op & 0xF0FF) == 0x400B:          # jsr @Rn
                    r = (op >> 8) & 0xF
                    if r in lit and a - lit[r][1] <= 16 and lit[r][0] is not None:
                        new.add(lit[r][0])
                elif op == 0x000B:                      # rts ends the straight-line window
                    lit = {}
                a += 2
        new = {x for x in new if tr.base <= x < end and x % 2 == 0} - funcs
        if not new: break
        funcs |= new
    return sorted(funcs)


PROLOGUE = {0x4F22, 0x2FE6, 0x2FD6, 0x2FC6, 0x2FB6, 0x2FA6, 0x2F96, 0x2F86, 0x4F12, 0x4F02}


def bra_tables(tr):
    """Switch tables made of `bra case ; slot` pairs (entered with braf/jmp): every entry is a
    jump target. Runs of 3+ consecutive pairs whose slot is a nop or simple op qualify."""
    out = []
    a = tr.base
    end = tr.end
    while a < end - 4:
        run = 0
        def slot_ok(op):     # anything but another branch / rts
            return op is not None and (op >> 12) not in (0x8, 0xA, 0xB) and op not in (0x000B, 0x002B) and (op & 0xF0DF) not in (0x400B, 0x0003)
        while a + run * 4 < end - 2 and (tr.u16(a + run * 4) >> 12) == 0xA and slot_ok(tr.u16(a + run * 4 + 2)):
            run += 1
        if run >= 3:
            out.extend(a + k * 4 for k in range(run)); a += run * 4
        else:
            a += 2
    return out


def scan_functions(tr):
    """Seed function starts without a disassembler project: the entry point, plus every
    bsr / `mov.l @(pc),Rn ; jsr @Rn` target and every pointer in the image that lands on a
    typical SHC prologue (sts.l pr,@-r15 / mov.l rN,@-r15 / add #-n,r15). Targets found
    only once that do not look like a prologue are dropped (data decoded as code)."""
    from collections import Counter
    hits = Counter(); end = tr.end
    def ok(x): return tr.base <= x < end and x % 2 == 0
    def looks_like_start(x):
        op = tr.u16(x)
        if op is None: return False
        if op in PROLOGUE or ((op & 0xFF00) == 0x7F00 and (op & 0x80)): return True
        # right after the end of another function: rts ; slot [; nop padding...]
        k = x - 2
        while k >= x - 16 and tr.u16(k) == 0x0009: k -= 2
        return tr.u16(k - 2) == 0x000B or tr.u16(k) == 0x000B
    lit = {}
    a = tr.base
    while a < end - 1:
        op = tr.u16(a); top = op >> 12
        if top == 0xD:
            lit[(op >> 8) & 0xF] = (mirror(tr, tr.u32(((a + 4) & ~3) + (op & 0xFF) * 4)), a)
        elif top == 0xB:
            d = op & 0xFFF; d = d - 0x1000 if d & 0x800 else d
            hits[a + 4 + d * 2] += 1
        elif (op & 0xF0FF) == 0x400B:
            r = (op >> 8) & 0xF
            if r in lit and lit[r][0] is not None and a - lit[r][1] <= 16:
                hits[lit[r][0]] += 2          # literal + jsr is strong evidence
        a += 2
    for o in range(0, len(tr.data) - 3, 4):   # function pointer tables
        v = mirror(tr, struct.unpack_from("<I", tr.data, o)[0])
        if ok(v) and looks_like_start(v):
            hits[v] += 1
    funcs = {tr.base}
    for x, n in hits.items():
        if ok(x) and (n >= 2 or looks_like_start(x)):
            funcs.add(x)
    return sorted(funcs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin"); ap.add_argument("--base", default="0x8C010000")
    ap.add_argument("--funcs", help="function start list; omit to scan the image (--scan)")
    ap.add_argument("--extra-funcs", action="append", default=[],
                    help="more function starts, e.g. the found-functions file a run wrote (dc_found_funcs_path); "
                    "a missing file is skipped")
    ap.add_argument("--out", required=True)
    ap.add_argument("--write-funcs", help="save the final function list here")
    ap.add_argument("--chunk", type=int, default=150, help="functions per .c file")
    ap.add_argument("--max-func-bytes", type=lambda x: int(x, 0), default=0x40000,
                    help="cap a function's range (protects against huge data gaps)")
    ap.add_argument("--no-discover", action="store_true")
    ap.add_argument("--setjmp", action="append", default=[], type=lambda x: int(x, 0),
                    help="address of the game's setjmp (cooperative threads; see dccore/coro.h)")
    ap.add_argument("--prefix", default="", help="symbol prefix for a second executable in the same program "
                    "(table PREFIX_dc_fn_table / PREFIX_dc_fn_count; switch with dc_use_code)")
    args = ap.parse_args()
    data = open(args.bin, "rb").read()
    base = int(args.base, 0)
    tr = Translator(data, base)
    tr.setjmp = {(a & 0x1FFFFFFF) | 0x80000000 for a in args.setjmp}
    tr.prefix = args.prefix + "_" if args.prefix else ""
    funcs = read_funcs(args.funcs, base, base + len(data)) if args.funcs else scan_functions(tr)
    if not args.funcs:
        print("scan: %d function starts" % len(funcs))
    for ex in args.extra_funcs:
        if os.path.exists(ex):
            more = set(read_funcs(ex, base, base + len(data))) - set(funcs)
            if more:
                funcs = sorted(set(funcs) | more)
                print("%s: %d more function starts" % (os.path.basename(ex), len(more)))
    tabs = set(bra_tables(tr)) - set(funcs)
    if FUNC_ENDS:          # exact function list: switch tables can only be inside its functions
        kn = sorted(FUNC_ENDS.items()); ks = [a for a, _ in kn]
        def inside(x):
            j = bisect.bisect_right(ks, x) - 1
            return j >= 0 and kn[j][0] < x < kn[j][1]
        tabs = {x for x in tabs if inside(x)}
    if tabs:
        funcs = sorted(set(funcs) | tabs)
        print("switch tables: %d bra entries" % len(tabs))
    if not args.no_discover:
        n0 = len(funcs); funcs = discover(tr, funcs)
        print("discovered %d extra call targets" % (len(funcs) - n0))
    os.makedirs(args.out, exist_ok=True)
    if args.write_funcs:
        open(args.write_funcs, "w").write("".join("%08x\n" % f for f in funcs))
    ranges = []
    MSVC_NOOPT_BYTES = 0x1000
    # entries inside a function of known length (switch-table targets) end with it
    known = sorted(FUNC_ENDS.items())
    kstarts = [a for a, _ in known]
    for k, s in enumerate(funcs):
        e = funcs[k + 1] if k + 1 < len(funcs) else base + len(data)
        e = min(e, s + args.max_func_bytes)
        if s in FUNC_ENDS and s < FUNC_ENDS[s] < e:
            e = FUNC_ENDS[s]               # known length: data after the function is not code
        elif known:
            j = bisect.bisect_right(kstarts, s) - 1
            if j >= 0 and known[j][0] < s < known[j][1] < e:
                e = known[j][1]
        ranges.append((s, e))
    ranges = extend_setjmp_callers(tr, ranges)
    # one file per 16 KB of address space: adding a function only changes its own file,
    # and files whose text is unchanged are not rewritten (incremental builds stay fast)
    files = []
    groups = {}
    for s_, e_ in ranges:
        groups.setdefault(s_ >> 14, []).append((s_, e_))
    keep = set()
    for g, rs in sorted(groups.items()):
        out = ['#include "dccore/sh4.h"', ""]
        for s_, e_ in rs:
            # MSVC's optimizer takes many minutes (or hangs) on very large generated functions; build
            # those without optimisation there (GCC/Clang at -O1 are fine)
            big = e_ - s_ >= MSVC_NOOPT_BYTES
            if big: out += ["#if defined(_MSC_VER) && !defined(__clang__)", '#pragma optimize("", off)', "#endif"]
            tr.emit_function(s_, e_, funcs, out)
            if big: out += ["#if defined(_MSC_VER) && !defined(__clang__)", '#pragma optimize("", on)', "#endif"]
            out.append("")
        fn = os.path.join(args.out, "recomp_%08X.c" % (g << 14))
        text = "\n".join(out)
        if not (os.path.exists(fn) and open(fn).read() == text):
            open(fn, "w").write(text)
        files.append(fn); keep.add(os.path.basename(fn))
    for f in os.listdir(args.out):
        if f.startswith("recomp_") and f.endswith(".c") and f != "recomp_table.c" and f not in keep:
            os.remove(os.path.join(args.out, f))
            o = os.path.join(args.out, f[:-2] + ".o")
            if os.path.exists(o): os.remove(o)
    # dispatch table
    tab = ['#include "dccore/sh4.h"', "#include <stddef.h>", ""]
    pf = tr.prefix
    for s, _ in ranges: tab.append("uint32_t f_%s%08X(sh4ctx *c);" % (pf, s))
    tab.append("")
    tab.append("typedef struct { uint32_t addr, end; sh4fn fn; } dc_fn_entry;")
    tab.append("const dc_fn_entry %sdc_fn_table[] = {" % pf)
    for s_, e_ in ranges: tab.append("  { %s, %s, f_%s%08X }," % (h(s_), h(e_), pf, s_))
    tab.append("};")
    tab.append("const size_t %sdc_fn_count = %d;" % (pf, len(ranges)))
    tp = os.path.join(args.out, "recomp_table.c"); text = "\n".join(tab) + "\n"
    if not (os.path.exists(tp) and open(tp).read() == text):
        open(tp, "w").write(text)
    print("%d functions, %d files -> %s" % (len(ranges), len(files) + 1, args.out))


if __name__ == "__main__":
    main()
