/* dc-core Windows CE runtime: a minimal window manager (GWES) and GDI, the registry, and COM object
 * creation. Games draw through DirectDraw, so windows only carry messages. */
#include "ce_int.h"
#include <ctype.h>

#define RET(v) do { c->r[0] = (uint32_t)(v); return 0; } while (0)
#define FN(name) static uint32_t g_##name(sh4ctx *c)

/* ------------------------------------------------------------------ window classes and windows */
#define MAXCLS 32
#define MAXWND 64
static struct { char name[64]; uint32_t proc, extra; } cls[MAXCLS]; static int ncls;
static struct { int used; uint32_t proc, cls, style, parent, owner_tid; uint32_t longs[16]; uint32_t userdata; } wnd[MAXWND];
#define HWND_OF(i) (0x00020000u + (uint32_t)(i) * 4u)
static int wnd_index(uint32_t h) { uint32_t i = (h - 0x00020000u) / 4; return h >= 0x00020000u && i < MAXWND && wnd[i].used ? (int)i : -1; }
static uint32_t focus_wnd;

static void cls_name(uint32_t p, char *out, size_t n) {
    if (p < 0x10000) snprintf(out, n, "#%u", p);
    else ce_wstr(p, out, (int)n);
}
FN(RegisterClassW) {           /* WNDCLASS { style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbr, menu, name } */
    uint32_t w = ARG(0);
    if (ncls == MAXCLS) RET(0);
    cls_name(dc_rd32(w + 36), cls[ncls].name, sizeof cls[0].name);
    cls[ncls].proc = dc_rd32(w + 4); cls[ncls].extra = dc_rd32(w + 12);
    TRACE("RegisterClass %s proc %08X", cls[ncls].name, cls[ncls].proc);
    ncls++;
    RET(0xC000 + ncls);
}
FN(UnregisterClassW) { RET(1); }
FN(GetClassInfoW) {
    char n[64]; cls_name(ARG(1), n, sizeof n);
    for (int i = 0; i < ncls; i++) if (!strcasecmp(cls[i].name, n)) {
        uint32_t w = ARG(2);
        for (int k = 0; k < 10; k++) dc_wr32(w + (uint32_t)k * 4, 0);
        dc_wr32(w + 4, cls[i].proc); dc_wr32(w + 12, cls[i].extra); dc_wr32(w + 36, ARG(1));
        RET(1);
    }
    RET(0);
}
static uint32_t send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    int i = wnd_index(h);
    if (i < 0 || !wnd[i].proc) return 0;
    return ce_call(wnd[i].proc, 4, h, msg, wp, lp);
}
FN(CreateWindowExW) {          /* (exstyle, class, title, style, x, y, w, h, parent, menu, inst, param) */
    char n[64]; cls_name(ARG(1), n, sizeof n);
    int ci = -1;
    for (int i = 0; i < ncls; i++) if (!strcasecmp(cls[i].name, n)) ci = i;
    int k = -1;
    for (int i = 1; i < MAXWND; i++) if (!wnd[i].used) { k = i; break; }
    if (k < 0) RET(0);
    memset(&wnd[k], 0, sizeof wnd[k]);
    wnd[k].used = 1; wnd[k].proc = ci >= 0 ? cls[ci].proc : 0; wnd[k].style = ARG(3); wnd[k].parent = ARG(8);
    wnd[k].owner_tid = ce_cur->id;
    uint32_t h = HWND_OF(k);
    TRACE("CreateWindow class %s -> %08X", n, h);
    if (!focus_wnd) focus_wnd = h;
    /* WM_CREATE with a CREATESTRUCT */
    uint32_t cs = ce_alloc(48, 4);
    dc_wr32(cs, ARG(11)); dc_wr32(cs + 4, ARG(10)); dc_wr32(cs + 8, ARG(9)); dc_wr32(cs + 12, ARG(8));
    dc_wr32(cs + 16, ARG(7)); dc_wr32(cs + 20, ARG(6)); dc_wr32(cs + 24, ARG(5)); dc_wr32(cs + 28, ARG(4));
    dc_wr32(cs + 32, ARG(3)); dc_wr32(cs + 36, ARG(2)); dc_wr32(cs + 40, ARG(1)); dc_wr32(cs + 44, ARG(0));
    uint32_t r = send(h, 0x0001, 0, cs);
    ce_free(cs);
    if (r == 0xFFFFFFFFu) { wnd[k].used = 0; RET(0); }
    RET(h);
}
FN(DestroyWindow) { int i = wnd_index(ARG(0)); if (i < 0) RET(0); send(ARG(0), 0x0002, 0, 0); wnd[i].used = 0; RET(1); }
FN(DefWindowProcW) { RET(0); }
FN(IsWindow) { RET(wnd_index(ARG(0)) >= 0); }
FN(ShowWindow) { RET(1); }
FN(UpdateWindow) { RET(1); }
FN(SetFocus) { uint32_t o = focus_wnd; focus_wnd = ARG(0); RET(o); }
FN(GetFocus) { RET(focus_wnd); }
FN(SetForegroundWindow) { RET(1); }
FN(GetForegroundWindow) { RET(focus_wnd); }
FN(SetActiveWindow) { RET(focus_wnd); }
FN(GetWindowLongW) {
    int i = wnd_index(ARG(0)); int32_t idx = (int32_t)ARG(1);
    if (i < 0) RET(0);
    if (idx == -4) RET(wnd[i].proc);            /* GWL_WNDPROC */
    if (idx == -16) RET(wnd[i].style);
    if (idx == -21) RET(wnd[i].userdata);       /* GWL_USERDATA */
    if (idx >= 0 && idx / 4 < 16) RET(wnd[i].longs[idx / 4]);
    RET(0);
}
FN(SetWindowLongW) {
    int i = wnd_index(ARG(0)); int32_t idx = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0;
    if (i < 0) RET(0);
    if (idx == -4) { old = wnd[i].proc; wnd[i].proc = v; }
    else if (idx == -16) { old = wnd[i].style; wnd[i].style = v; }
    else if (idx == -21) { old = wnd[i].userdata; wnd[i].userdata = v; }
    else if (idx >= 0 && idx / 4 < 16) { old = wnd[i].longs[idx / 4]; wnd[i].longs[idx / 4] = v; }
    RET(old);
}
FN(GetSystemMetrics) {
    switch (ARG(0)) { case 0: RET(640); case 1: RET(480); default: RET(0); }
}
FN(GetClientRect) { uint32_t r = ARG(1); dc_wr32(r, 0); dc_wr32(r + 4, 0); dc_wr32(r + 8, 640); dc_wr32(r + 12, 480); RET(1); }
FN(GetWindowRect) { uint32_t r = ARG(1); dc_wr32(r, 0); dc_wr32(r + 4, 0); dc_wr32(r + 8, 640); dc_wr32(r + 12, 480); RET(1); }
FN(GetKeyState) { RET(0); }
FN(GetAsyncKeyState) { RET(0); }
FN(PostKeybdMessage) { RET(1); }
FN(ShowCursor) { RET(0); }
FN(SetCursor) { RET(0); }
FN(MessageBoxW) {
    char t[256], cap[64]; ce_wstr(ARG(1), t, sizeof t); ce_wstr(ARG(2), cap, sizeof cap);
    fprintf(stderr, "[ce] MessageBox \"%s\": %s\n", cap, t);
    RET(1);
}

/* ------------------------------------------------------------------ messages: one queue per thread */
static ce_thread *thread_by_id(uint32_t tid);
static int post(ce_thread *t, uint32_t h, uint32_t m, uint32_t wp, uint32_t lp) {
    if (!t || t->qn == 64) return 0;
    ce_msg *q = &t->q[(t->qh + t->qn++) % 64];
    q->hwnd = h; q->msg = m; q->wparam = wp; q->lparam = lp;
    return 1;
}
static int quit_posted;
#define MAXTIMERS 16
static struct { uint32_t hwnd, id, ms, proc; uint64_t next; int used; } timers[MAXTIMERS];

static int take(uint32_t msgp, uint32_t hwnd, int remove) {
    ce_thread *t = ce_cur;
    for (int k = 0; k < MAXTIMERS; k++) if (timers[k].used && ce_now() >= timers[k].next) {
        int i = wnd_index(timers[k].hwnd);
        if (i < 0 || wnd[i].owner_tid == t->id) {
            timers[k].next = ce_now() + (uint64_t)timers[k].ms * CE_CYCLES_PER_MS;
            post(t, timers[k].hwnd, 0x0113, timers[k].id, timers[k].proc);    /* WM_TIMER */
        }
    }
    for (int k = 0; k < t->qn; k++) {
        ce_msg *q = &t->q[(t->qh + k) % 64];
        if (hwnd && q->hwnd != hwnd) continue;
        dc_wr32(msgp, q->hwnd); dc_wr32(msgp + 4, q->msg); dc_wr32(msgp + 8, q->wparam); dc_wr32(msgp + 12, q->lparam);
        dc_wr32(msgp + 16, ce_milliseconds()); dc_wr32(msgp + 20, 0); dc_wr32(msgp + 24, 0);
        if (remove) {
            for (int j = k; j < t->qn - 1; j++) t->q[(t->qh + j) % 64] = t->q[(t->qh + j + 1) % 64];
            t->qn--;
        }
        return 1;
    }
    return 0;
}
FN(PeekMessageW) {             /* (msg, hwnd, min, max, remove) */
    if (take(ARG(0), ARG(1), ARG(4) & 1)) RET(1);
    /* a game polling its queue in its main loop: let the other threads run */
    ce_sleep(0);
    RET(0);
}
FN(GetMessageW) {
    for (;;) {
        if (take(ARG(0), ARG(1), 1)) RET(dc_rd32(ARG(0) + 4) != 0x0012);    /* WM_QUIT -> 0 */
        ce_sleep(1);
    }
}
FN(TranslateMessage) { RET(0); }
FN(DispatchMessageW) {
    uint32_t m = ARG(0), h = dc_rd32(m), msg = dc_rd32(m + 4);
    if (msg == 0x0113 && dc_rd32(m + 12)) RET(ce_call(dc_rd32(m + 12), 4, h, msg, dc_rd32(m + 8), ce_milliseconds()));
    RET(send(h, msg, dc_rd32(m + 8), dc_rd32(m + 12)));
}
FN(SendMessageW) { RET(send(ARG(0), ARG(1), ARG(2), ARG(3))); }
FN(PostMessageW) {
    int i = wnd_index(ARG(0));
    ce_thread *t = i >= 0 ? thread_by_id(wnd[i].owner_tid) : ce_cur;
    RET(post(t, ARG(0), ARG(1), ARG(2), ARG(3)));
}
FN(PostThreadMessageW) { RET(post(thread_by_id(ARG(0)), 0, ARG(1), ARG(2), ARG(3))); }
FN(PostQuitMessage) { quit_posted = 1; post(ce_cur, 0, 0x0012, ARG(0), 0); RET(0); }
FN(SetTimer) {                 /* (hwnd, id, ms, proc) */
    for (int k = 0; k < MAXTIMERS; k++) if (!timers[k].used || (timers[k].hwnd == ARG(0) && timers[k].id == ARG(1))) {
        timers[k].used = 1; timers[k].hwnd = ARG(0); timers[k].id = ARG(1) ? ARG(1) : (uint32_t)k + 1;
        timers[k].ms = ARG(2) ? ARG(2) : 1; timers[k].proc = ARG(3);
        timers[k].next = ce_now() + (uint64_t)timers[k].ms * CE_CYCLES_PER_MS;
        RET(timers[k].id);
    }
    RET(0);
}
FN(KillTimer) { for (int k = 0; k < MAXTIMERS; k++) if (timers[k].used && timers[k].hwnd == ARG(0) && timers[k].id == ARG(1)) timers[k].used = 0; RET(1); }

/* ------------------------------------------------------------------ GDI: objects only (text is drawn by the game) */
static uint32_t gdi_next = 0x00030000u;
FN(CreateCompatibleDC) { RET(gdi_next += 4); }
FN(GetDC) { RET(gdi_next += 4); }
FN(ReleaseDC) { RET(1); }
FN(DeleteDC) { RET(1); }
FN(SelectObject) { RET(0x00030000u); }
FN(DeleteObject) { RET(1); }
FN(GetStockObject) { RET(0x00030000u + ARG(0) * 4); }
FN(CreateFontIndirectW) { char f[64]; ce_wstr(ARG(0) + 28, f, sizeof f); TRACE("CreateFontIndirect %s %d", f, (int)dc_rd32(ARG(0))); RET(gdi_next += 4); }
FN(EnumFontFamiliesW) { ce_unimpl("EnumFontFamiliesW"); RET(1); }
FN(CreateBitmapFromPointer) { ce_unimpl("CreateBitmapFromPointer"); RET(gdi_next += 4); }
FN(CreateDIBSection) { ce_unimpl("CreateDIBSection"); RET(0); }
FN(GetDeviceCaps) {
    switch (ARG(1)) { case 8: RET(640); case 10: RET(480); case 12: RET(16); case 14: RET(1); case 88: case 90: RET(96); default: RET(0); }
}
FN(SetTextColor) { RET(0); }
FN(SetBkColor) { RET(0); }
FN(SetBkMode) { RET(1); }
FN(ExtTextOutW) { char s[128]; ce_wstr(ARG(5), s, sizeof s); TRACE("ExtTextOut \"%s\"", s); RET(1); }
FN(DrawTextW) { char s[128]; ce_wstr(ARG(1), s, sizeof s); TRACE("DrawText \"%s\"", s); RET(16); }
FN(GetTextExtentExPointW) { uint32_t sz = ARG(6); if (sz) { dc_wr32(sz, ARG(2) * 8); dc_wr32(sz + 4, 16); } RET(1); }
FN(GetTextMetricsW) { uint32_t t = ARG(1); for (int i = 0; i < 14; i++) dc_wr32(t + (uint32_t)i * 4, 0); dc_wr32(t, 16); dc_wr32(t + 20, 8); dc_wr32(t + 24, 16); RET(1); }
FN(BitBlt) { RET(1); }
FN(PatBlt) { RET(1); }
FN(FillRect) { RET(1); }
FN(AddFontResourceW) { RET(1); }

const ce_override ce_gwes_overrides[] = {
#define G(n) { "COREDLL.DLL", #n, g_##n }
    G(RegisterClassW), G(UnregisterClassW), G(GetClassInfoW), G(CreateWindowExW), G(DestroyWindow), G(DefWindowProcW),
    G(IsWindow), G(ShowWindow), G(UpdateWindow), G(SetFocus), G(GetFocus), G(SetForegroundWindow), G(GetForegroundWindow),
    G(SetActiveWindow), G(GetWindowLongW), G(SetWindowLongW), G(GetSystemMetrics), G(GetClientRect), G(GetWindowRect),
    G(GetKeyState), G(GetAsyncKeyState), G(PostKeybdMessage), G(ShowCursor), G(SetCursor), G(MessageBoxW),
    G(PeekMessageW), G(GetMessageW), G(TranslateMessage), G(DispatchMessageW), G(SendMessageW), G(PostMessageW),
    G(PostThreadMessageW), G(PostQuitMessage), G(SetTimer), G(KillTimer),
    G(CreateCompatibleDC), G(GetDC), G(ReleaseDC), G(DeleteDC), G(SelectObject), G(DeleteObject), G(GetStockObject),
    G(CreateFontIndirectW), G(EnumFontFamiliesW), G(CreateBitmapFromPointer), G(CreateDIBSection), G(GetDeviceCaps),
    G(SetTextColor), G(SetBkColor), G(SetBkMode), G(ExtTextOutW), G(DrawTextW), G(GetTextExtentExPointW), G(GetTextMetricsW),
    G(BitBlt), G(PatBlt), G(FillRect), G(AddFontResourceW),
#undef G
    { NULL, NULL, NULL }
};

/* ------------------------------------------------------------------ registry: a tree in host memory */
typedef struct rval { char name[64]; uint32_t type, len; uint8_t *data; struct rval *next; } rval;
typedef struct rkey { char path[256]; rval *vals; struct rkey *next; } rkey;
static rkey *keys;
static const char *root_name(uint32_t h) {
    switch (h) { case 0x80000000u: return "HKCR"; case 0x80000001u: return "HKCU"; case 0x80000002u: return "HKLM"; case 0x80000003u: return "HKU"; }
    ce_obj *o = ce_handle(h);
    return o && o->type == OB_REG ? o->name : NULL;
}
static rkey *key_find(const char *path, int create) {
    for (rkey *k = keys; k; k = k->next) if (!strcasecmp(k->path, path)) return k;
    if (!create) return NULL;
    {   /* the parents exist too */
        char parent[256]; snprintf(parent, sizeof parent, "%s", path);
        char *sl = strrchr(parent, '\\');
        if (sl) { *sl = 0; key_find(parent, 1); }
    }
    rkey *k = calloc(1, sizeof *k); snprintf(k->path, sizeof k->path, "%s", path); k->next = keys; keys = k;
    return k;
}
static int key_path(uint32_t root, uint32_t sub, char *out, size_t n) {
    const char *r = root_name(root);
    if (!r) return -1;
    char s[192]; ce_wstr(sub, s, sizeof s);
    if (s[0]) snprintf(out, n, "%s\\%s", r, s); else snprintf(out, n, "%s", r);
    return 0;
}
static rval *val_find(rkey *k, const char *name, int create) {
    for (rval *v = k->vals; v; v = v->next) if (!strcasecmp(v->name, name)) return v;
    if (!create) return NULL;
    rval *v = calloc(1, sizeof *v); snprintf(v->name, sizeof v->name, "%s", name); v->next = k->vals; k->vals = v;
    return v;
}
static uint32_t open_handle(const char *path) {
    uint32_t h = ce_handle_new(OB_REG);
    snprintf(ce_handle(h)->name, sizeof ce_handle(h)->name, "%s", path);
    return h;
}
FN(RegOpenKeyExW) {            /* (root, sub, opts, sam, &h) */
    char p[256];
    if (key_path(ARG(0), ARG(1), p, sizeof p)) RET(6);
    rkey *k = key_find(p, 0);
    TRACE("RegOpenKey %s%s", p, k ? "" : " (missing)");
    if (!k && strchr(p, '\\')) RET(2);       /* ERROR_FILE_NOT_FOUND */
    dc_wr32(ARG(4), open_handle(p));
    RET(0);
}
FN(RegCreateKeyExW) {          /* (root, sub, res, class, opts, sam, sec, &h, &disp) */
    char p[256];
    if (key_path(ARG(0), ARG(1), p, sizeof p)) RET(6);
    int existed = key_find(p, 0) != NULL;
    key_find(p, 1);
    dc_wr32(ARG(7), open_handle(p));
    if (ARG(8)) dc_wr32(ARG(8), existed ? 2 : 1);
    TRACE("RegCreateKey %s", p);
    RET(0);
}
FN(RegCloseKey) { ce_handle_close(ARG(0)); RET(0); }
FN(RegQueryValueExW) {         /* (h, name, res, &type, data, &len) */
    const char *kp = root_name(ARG(0));
    char n[64]; ce_wstr(ARG(1), n, sizeof n);
    rkey *k = kp ? key_find(kp, 0) : NULL;
    rval *v = k ? val_find(k, n, 0) : NULL;
    TRACE("RegQueryValue %s : %s%s", kp ? kp : "?", n, v ? "" : " (missing)");
    if (!v) RET(2);
    if (ARG(3)) dc_wr32(ARG(3), v->type);
    uint32_t lenp = ARG(5), cap = lenp ? dc_rd32(lenp) : 0;
    if (lenp) dc_wr32(lenp, v->len);
    if (ARG(4)) { if (cap < v->len) RET(234); ce_wr_block(ARG(4), v->data, v->len); }
    RET(0);
}
FN(RegSetValueExW) {           /* (h, name, res, type, data, len) */
    const char *kp = root_name(ARG(0));
    if (!kp) RET(6);
    char n[64]; ce_wstr(ARG(1), n, sizeof n);
    rval *v = val_find(key_find(kp, 1), n, 1);
    free(v->data); v->type = ARG(3); v->len = ARG(5); v->data = malloc(v->len + 1);
    ce_rd_block(v->data, ARG(4), v->len);
    RET(0);
}
FN(RegDeleteValueW) { RET(0); }
FN(RegDeleteKeyW) { RET(0); }
FN(RegEnumKeyExW) {            /* (h, index, name, &len, ...) */
    const char *kp = root_name(ARG(0));
    if (!kp) RET(6);
    size_t pl = strlen(kp); uint32_t idx = 0;
    for (rkey *k = keys; k; k = k->next)
        if (!strncasecmp(k->path, kp, pl) && k->path[pl] == '\\' && !strchr(k->path + pl + 1, '\\')) {
            if (idx++ == ARG(1)) {
                ce_put_wstr(ARG(2), k->path + pl + 1, (int)dc_rd32(ARG(3)));
                dc_wr32(ARG(3), (uint32_t)strlen(k->path + pl + 1));
                RET(0);
            }
        }
    RET(259);                  /* ERROR_NO_MORE_ITEMS */
}
FN(RegEnumValueW) { RET(259); }
FN(RegQueryInfoKeyW) {
    for (int i = 4; i < 12; i++) if (ARG(i)) dc_wr32(ARG(i), 0);
    RET(0);
}
static void reg_set(const char *key, const char *name, uint32_t type, const void *data, uint32_t len) {
    rval *v = val_find(key_find(key, 1), name, 1);
    free(v->data); v->type = type; v->len = len; v->data = malloc(len + 1); memcpy(v->data, data, len);
}
void ce_reg_set_dword(const char *key, const char *name, uint32_t d) { reg_set(key, name, 4, &d, 4); }
void ce_reg_set_string(const char *key, const char *name, const char *s) {
    uint8_t w[512]; uint32_t n = 0;
    for (; *s && n < 510; s++) { w[n++] = (uint8_t)*s; w[n++] = 0; }
    w[n++] = 0; w[n++] = 0;
    reg_set(key, name, 1, w, n);
}
void ce_reg_init(void) {}

/* ------------------------------------------------------------------ COM: CoCreateInstance without the
 * registry's class list: ask each DLL that exports DllGetClassObject (cached per class) */
static struct { uint8_t clsid[16]; char dll[64]; } com_cache[64]; static int ncom;
static const char *com_dlls[96]; static int ncom_dlls;
void ce_com_add_server(const char *dll) { if (ncom_dlls < 96) com_dlls[ncom_dlls++] = dll; }
static uint32_t try_class(const char *dll, uint32_t clsid, uint32_t outer, uint32_t riid, uint32_t ppv, uint32_t *hr) {
    uint32_t h = ce_load_library(dll);
    if (!h) return 0;
    uint32_t gco = ce_get_proc(h, "DllGetClassObject", 0);
    if (!gco) { ce_free_library(h); return 0; }
    static const uint8_t iid_cf[16] = { 1, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    uint32_t tmp = ce_alloc(24, 4);
    ce_wr_block(tmp, iid_cf, 16);
    uint32_t r = ce_call(gco, 3, clsid, tmp, tmp + 16);
    if (r != 0) { if (ce_trace > 1) TRACE("  %s DllGetClassObject -> %08X", dll, r); ce_free(tmp); ce_free_library(h); return 0; }
    uint32_t cf = dc_rd32(tmp + 16), vt = dc_rd32(cf);
    *hr = ce_call(dc_rd32(vt + 12), 4, cf, outer, riid, ppv);     /* IClassFactory::CreateInstance */
    ce_call(dc_rd32(vt + 8), 1, cf);                              /* Release */
    ce_free(tmp);
    return 1;
}
static uint32_t m_CoCreateInstance(sh4ctx *c) {   /* (clsid, outer, ctx, riid, ppv) */
    uint8_t id[16]; ce_rd_block(id, ARG(0), 16);
    uint32_t hr = 0x80040154u;                       /* REGDB_E_CLASSNOTREG */
    if (ARG(4)) dc_wr32(ARG(4), 0);
    char s[48]; snprintf(s, sizeof s, "%08X-%04X-%04X-%02X%02X", (uint32_t)(id[0] | id[1] << 8 | id[2] << 16 | (uint32_t)id[3] << 24),
                         id[4] | id[5] << 8, id[6] | id[7] << 8, id[8], id[9]);
    for (int i = 0; i < ncom; i++) if (!memcmp(com_cache[i].clsid, id, 16)) {
        if (!com_cache[i].dll[0]) RET(hr);
        try_class(com_cache[i].dll, ARG(0), ARG(1), ARG(3), ARG(4), &hr);
        TRACE("CoCreateInstance {%s} (%s) -> %08X", s, com_cache[i].dll, hr);
        RET(hr);
    }
    for (int d = 0; d < ncom_dlls; d++) {
        if (try_class(com_dlls[d], ARG(0), ARG(1), ARG(3), ARG(4), &hr)) {
            if (ncom < 64) { memcpy(com_cache[ncom].clsid, id, 16); snprintf(com_cache[ncom].dll, 64, "%s", com_dlls[d]); ncom++; }
            TRACE("CoCreateInstance {%s} -> %s %08X", s, com_dlls[d], hr);
            RET(hr);
        }
    }
    TRACE("CoCreateInstance {%s}: no server", s);
    if (ncom < 64) { memcpy(com_cache[ncom].clsid, id, 16); com_cache[ncom].dll[0] = 0; ncom++; }   /* don't ask them all again */
    RET(hr);
}
static uint32_t m_CoInitializeEx(sh4ctx *c) { (void)c; RET(0); }
static uint32_t m_CoUninitialize(sh4ctx *c) { (void)c; RET(0); }

const ce_override ce_misc_overrides[] = {
#define R(n) { "COREDLL.DLL", #n, g_##n }
    R(RegOpenKeyExW), R(RegCreateKeyExW), R(RegCloseKey), R(RegQueryValueExW), R(RegSetValueExW), R(RegDeleteValueW),
    R(RegDeleteKeyW), R(RegEnumKeyExW), R(RegEnumValueW), R(RegQueryInfoKeyW),
#undef R
    { "OLE32.DLL", "CoCreateInstance", m_CoCreateInstance }, { "OLE32.DLL", "CoInitializeEx", m_CoInitializeEx },
    { "OLE32.DLL", "CoUninitialize", m_CoUninitialize },
    { NULL, NULL, NULL }
};

static ce_thread *thread_by_id(uint32_t tid) {
    extern ce_thread *ce_thread_of(uint32_t h);
    return ce_thread_of(tid);
}
