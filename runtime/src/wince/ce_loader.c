/* dc-core Windows CE runtime: the module loader (EXE + DLLs at the addresses ce_layout.py chose). */
#include "ce_int.h"
#include "dccore/disc.h"
#include <ctype.h>

#define MAXM 96
static ce_module mods[MAXM];
static int nmods;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

static void upname(const char *in, char *out, size_t n) {
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t i = 0;
    for (; b[i] && i < n - 1; i++) out[i] = (char)toupper((unsigned char)b[i]);
    out[i] = 0;
    if (!strchr(out, '.') && i + 4 < n) strcat(out, ".DLL");
}

ce_module *ce_module_find(const char *name) {
    char u[64]; upname(name, u, sizeof u);
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i].name, u)) return &mods[i];
    return NULL;
}
ce_module *ce_module_at(uint32_t va) {
    for (int i = 0; i < nmods; i++) if (!mods[i].native && va - mods[i].base < mods[i].size) return &mods[i];
    return NULL;
}

/* the file as CE maps it, relocated to `base` */
static uint8_t *map_image(const uint8_t *f, uint32_t fsize, uint32_t base, uint32_t *size_out, uint32_t *entry_out) {
    if (fsize < 0x40 || f[0] != 'M' || f[1] != 'Z') return NULL;
    uint32_t o = rd32le(f + 0x3C);
    if (o + 0xF8 > fsize || memcmp(f + o, "PE\0\0", 4)) return NULL;
    uint32_t nsec = rd16le(f + o + 6), optsz = rd16le(f + o + 20), opt = o + 24;
    uint32_t entry = rd32le(f + opt + 16), ibase = rd32le(f + opt + 28), size = rd32le(f + opt + 56), hdr = rd32le(f + opt + 60);
    uint8_t *img = calloc(1, size);
    memcpy(img, f, hdr < fsize ? hdr : fsize);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = f + opt + optsz + 40 * i;
        uint32_t vsize = rd32le(s + 8), va = rd32le(s + 12), rsize = rd32le(s + 16), rptr = rd32le(s + 20);
        uint32_t n = vsize && vsize < rsize ? vsize : rsize;
        if (va + n > size) n = size - va;
        if (rptr + n > fsize) n = rptr < fsize ? fsize - rptr : 0;
        memcpy(img + va, f + rptr, n);
    }
    uint32_t delta = base - ibase, rrva = rd32le(f + opt + 96 + 5 * 8), rlen = rd32le(f + opt + 96 + 5 * 8 + 4);
    if (delta && rlen) {
        for (uint32_t p = rrva; p + 8 <= rrva + rlen;) {
            uint32_t page = rd32le(img + p), bsize = rd32le(img + p + 4);
            if (bsize < 8) break;
            for (uint32_t k = 0; k < (bsize - 8) / 2; k++) {
                uint16_t e = rd16le(img + p + 8 + 2 * k);
                if (e >> 12 == 3) { uint8_t *q = img + page + (e & 0xFFF); wr32le(q, rd32le(q) + delta); }
            }
            p += bsize;
        }
    }
    *size_out = size; *entry_out = entry ? base + entry : 0;
    return img;
}

static int read_disc_file(const char *name, uint8_t **out, uint32_t *size) {
    uint32_t lba;
    if (dc_disc_find(name, &lba, size)) return -1;
    *out = malloc(*size + 2048);
    if (dc_disc_read(lba, (*size + 2047) / 2048, *out)) { free(*out); return -1; }
    return 0;
}

int ce_init(sh4ctx *c, const ce_module_desc *d, int n, const char **bad) {
    ce_ctx = c;
    { const char *e = getenv("DC_CE_TRACE"); if (e) ce_trace = atoi(e); }
    if (!dc_uram) {
        /* the user address space is the console's RAM itself (its low 16 MB, all of it with
         * DC_RAM_32MB), so drivers' DMA buffers have physical addresses (0x0C000000 + va) the
         * hardware model can reach */
        dc_uram = calloc(1, DC_URAM_SIZE);
        if (dc_ram) { memcpy(dc_uram, dc_ram, DC_RAM_SIZE); free(dc_ram); }
        dc_ram = dc_uram;
    }
    for (int i = 0; i < n && nmods < MAXM; i++) {
        uint8_t *f; uint32_t fs;
        if (read_disc_file(d[i].name, &f, &fs)) { if (bad) *bad = d[i].name; return -1; }
        if (fs != d[i].file_size || fnv1a(f, fs) != d[i].hash) { free(f); if (bad) *bad = d[i].name; return -2; }
        ce_module *m = &mods[nmods++];
        snprintf(m->name, sizeof m->name, "%s", d[i].name);
        m->base = d[i].base; m->desc = &d[i]; m->is_exe = i == 0;
        m->native = !strcmp(m->name, "COREDLL.DLL");
        m->pristine = map_image(f, fs, d[i].base, &m->size, &m->entry);
        free(f);
        if (!m->pristine) { if (bad) *bad = d[i].name; return -2; }
    }
    uint32_t end = 0;
    for (int i = 0; i < nmods; i++) if (mods[i].base + mods[i].size > end) end = mods[i].base + mods[i].size;
    ce_heap_init((end + 0xFFFF) & ~0xFFFFu, DC_URAM_SIZE - 0x10000);
    ce_heap_reserve(CE_DRVMEM_PHYS & 0x00FFFFFFu, (CE_DRVMEM_PHYS & 0x00FFFFFFu) + CE_DRVMEM_LEN);
    ce_reg_init();
    ce_irq_init();
    ce_overrides_init();
    extern void ce_com_add_server(const char *dll);
    for (int i = 1; i < nmods; i++)              /* COM servers: CoCreateInstance asks them in turn */
        if (ce_get_proc(mods[i].base, "DllGetClassObject", 0)) ce_com_add_server(mods[i].name);
    return 0;
}

/* ---- imports */
static uint32_t resolve(const char *dll, const char *fname, uint32_t ord) {
    uint32_t a;
    uint32_t h = ce_load_library(dll);
    if (!h) { fprintf(stderr, "[ce] import from missing %s\n", dll); return 0; }
    a = ce_get_proc(h, fname, ord);
    if (!a) fprintf(stderr, "[ce] %s has no export %s (#%u)\n", dll, fname ? fname : "", ord);
    return a;
}
static void link_imports(ce_module *m) {
    uint8_t *img = dc_uram + m->base;
    uint32_t opt = rd32le(img + 0x3C) + 24;
    uint32_t irva = rd32le(img + opt + 96 + 8), ilen = rd32le(img + opt + 96 + 12);
    if (!ilen) return;
    for (uint32_t p = irva; ; p += 20) {
        uint32_t ilt = rd32le(img + p), name = rd32le(img + p + 12), iat = rd32le(img + p + 16);
        if (!name && !iat) break;
        char dll[64]; snprintf(dll, sizeof dll, "%s", (char *)img + name);
        if (!ilt) ilt = iat;
        for (uint32_t k = 0;; k++) {
            uint32_t e = rd32le(img + ilt + 4 * k);
            if (!e) break;
            uint32_t a;
            if (e & 0x80000000u) a = resolve(dll, NULL, e & 0xFFFF);
            else a = resolve(dll, (char *)img + e + 2, 0);
            wr32le(img + iat + 4 * k, a);
        }
    }
}

static void dll_main(ce_module *m, uint32_t reason) {
    if (!m->entry || m->is_exe || m->native) return;     /* COREDLL: its start-up is the kernel's, done natively */
    TRACE("DllMain %s reason %u", m->name, reason);
    ce_call(m->entry, 3, m->base, reason, 0);
}

uint32_t ce_load_library(const char *name) {
    ce_module *m = ce_module_find(name);
    if (!m) { TRACE("LoadLibrary %s: not available", name); return 0; }
    if (m->refs++ > 0) return m->base;
    TRACE("LoadLibrary %s at %08X", m->name, m->base);
    memcpy(dc_uram + m->base, m->pristine, m->size);      /* a fresh copy every time it's loaded */
    link_imports(m);
    dll_main(m, 1);
    return m->base;
}
int ce_free_library(uint32_t h) {
    for (int i = 0; i < nmods; i++) if (mods[i].base == h) {
        ce_module *m = &mods[i];
        if (m->refs > 0 && --m->refs == 0 && !m->native) { dll_main(m, 0); TRACE("FreeLibrary %s", m->name); }
        return 1;
    }
    return 0;
}

uint32_t ce_get_proc(uint32_t h, const char *fname, uint32_t ord) {
    ce_module *m = NULL;
    for (int i = 0; i < nmods; i++) if (mods[i].base == h) m = &mods[i];
    if (!m) return 0;
    uint8_t *img = m->refs ? dc_uram + m->base : m->pristine;
    uint32_t opt = rd32le(img + 0x3C) + 24;
    uint32_t erva = rd32le(img + opt + 96), elen = rd32le(img + opt + 100);
    if (!elen) return 0;
    uint32_t obase = rd32le(img + erva + 16), nfun = rd32le(img + erva + 20), nnames = rd32le(img + erva + 24);
    uint32_t afun = rd32le(img + erva + 28), anames = rd32le(img + erva + 32), aords = rd32le(img + erva + 36);
    uint32_t idx = 0xFFFFFFFFu;
    if (fname) {
        for (uint32_t i = 0; i < nnames; i++)
            if (!strcmp((char *)img + rd32le(img + anames + 4 * i), fname)) { idx = rd16le(img + aords + 2 * i); break; }
    } else idx = ord - obase;
    if (idx >= nfun) return 0;
    uint32_t f = rd32le(img + afun + 4 * idx);
    if (f >= erva && f < erva + elen) {          /* forwarder: "DLL.Name" */
        char fw[128]; snprintf(fw, sizeof fw, "%s", (char *)img + f);
        char *dot = strchr(fw, '.');
        if (!dot) return 0;
        *dot = 0;
        uint32_t h2 = ce_load_library(fw);
        return h2 ? ce_get_proc(h2, dot + 1, 0) : 0;
    }
    return f ? m->base + f : 0;
}


/* the sound CPU's program: the CE ROM loads armcom.drv into sound RAM at boot and starts the ARM7 */
int ce_load_arm_firmware(const char *name) {
    uint8_t *f; uint32_t n;
    if (read_disc_file(name, &f, &n)) return -1;
    /* a 12-byte header, then the image for sound RAM 0 (its vectors: FIQ at 0x1C is the file's 0x28,
     * and its variables at 0x200.. are the file's zeros at 0x20C..) */
    const uint32_t hdr = 12;
    dc_wr32(0xA0702C00u, 1);                                   /* ARM in reset */
    for (uint32_t i = 0; hdr + i + 4 <= n && i < 0x200000; i += 4) dc_wr32(0xA0800000u + i, rd32le(f + hdr + i));
    dc_wr32(0xA0702C00u, 0);                                   /* run */
    free(f);
    return 0;
}

static uint32_t cmdline_va;
static struct { const char *dll, *fn; } startup[8]; static int nstartup;
void ce_add_startup(const char *dll, const char *fn) { if (nstartup < 8) { startup[nstartup].dll = dll; startup[nstartup].fn = fn; nstartup++; } }
/* the main thread's first code: the EXE's imports (DllMain of each DLL it links runs here, on this
 * thread like on CE), then WinMain's caller, the EXE entry point */
static uint32_t h_process_start(sh4ctx *c) {
    ce_module *exe = &mods[0];
    for (int i = 0; i < nstartup; i++) {          /* what the system's own processes would have set up */
        uint32_t h = ce_load_library(startup[i].dll), f = h ? ce_get_proc(h, startup[i].fn, 0) : 0;
        TRACE("start-up: %s!%s", startup[i].dll, startup[i].fn);
        if (f) ce_call(f, 0); else fprintf(stderr, "[ce] start-up %s!%s not found\n", startup[i].dll, startup[i].fn);
    }
    memcpy(dc_uram + exe->base, exe->pristine, exe->size);
    exe->refs = 1;
    link_imports(exe);
    TRACE("starting %s at %08X", exe->name, exe->entry);
    c->r[4] = exe->base; c->r[5] = 0; c->r[6] = cmdline_va; c->r[7] = 1;   /* SW_SHOWNORMAL */
    dc_call(c, exe->entry);
    return 0;               /* -> ExitThread(r0) */
}
int ce_run(sh4ctx *c, const char *cmdline) {
    extern int ce_run_main(sh4ctx *c, uint32_t entry);
    for (int i = 0; i < nmods; i++) if (mods[i].native) {   /* COREDLL is always there */
        memcpy(dc_uram + mods[i].base, mods[i].pristine, mods[i].size); mods[i].refs = 1; link_imports(&mods[i]);
    }
    cmdline_va = ce_alloc(512, 4);
    for (int i = 0; cmdline && cmdline[i] && i < 255; i++) dc_wr16(cmdline_va + (uint32_t)i * 2, (uint8_t)cmdline[i]);
    c->vbr = CE_VBR; c->sr_rest = 0;
    dc_hle_register(CE_PROC_START, h_process_start);
    return ce_run_main(c, CE_PROC_START);
}
