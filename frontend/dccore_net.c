/* Online play (dccore_front.h, dc_front_net_*): the Broadband Adapter's link (dccore/net.h) carried
 * to a DCNet server, the online service Flycast runs for Dreamcast games.
 *
 * DCNet over TCP (port 7655): every message is a 16-bit little-endian length and that many bytes.
 * The first one the client sends is the prolog "DCNET" + version 1; after it every message either
 * way is one Ethernet frame. The server gives the console an address by DHCP and routes it, so the
 * game's own network code does the rest.
 *
 * The modem (dc_modem_enabled) goes to DCNet's dial-up access instead (port 7654): the PPP bytes
 * the game sends and receives, as they are, from when the line comes up (dc_net.open) until the
 * game hangs up (dc_net.hangup), which ends the connection so the next call starts a new session.
 *
 * One thread owns the socket: it connects (the adapter: when the game first sends; the modem: when
 * the line comes up), sends what the game queued and reads what arrives, and when the connection
 * drops it connects again after 1, 2, 4 ... 30 seconds. The game's thread only touches the two
 * queues, under a mutex, and never waits for the network. */
#include "dccore_front.h"
#include "dccore/net.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define close_sock closesocket
#  define sock_err() WSAGetLastError()
#  define IN_PROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
static void set_nonblock(sock_t s, int on) { u_long v = (u_long)on; ioctlsocket(s, FIONBIO, &v); }
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define close_sock close
#  define sock_err() errno
#  define IN_PROGRESS(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK || (e) == EAGAIN)
static void set_nonblock(sock_t s, int on) { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK); }
#endif
#ifdef __SWITCH__
#  include <switch.h>
#endif

#define FRAME_MAX 1600
#define RXQ 128                       /* frames received and not yet in the adapter's ring */
#define TXQ 256                       /* frames the game sent while the connection was not up */

typedef struct { uint16_t len; uint8_t d[FRAME_MAX]; } frame_t;
static frame_t rxq[RXQ], txq[TXQ];
static int rx_head, rx_tail, tx_head, tx_tail, rx_off;
static int ppp;                       /* the modem's byte stream (else the adapter's frames) */
static SDL_mutex *mx;
static SDL_cond *wake;
static SDL_Thread *thr;
static volatile int running, wanted, connected;
static char host[128];
static int port;
static char status[256] = "offline";
static uint64_t tx_dropped, rx_dropped;

static void set_status(const char *s) {
    SDL_LockMutex(mx); snprintf(status, sizeof status, "%s", s); SDL_UnlockMutex(mx);
    fprintf(stderr, "online: %s\n", s);
}

/* ---------------------------------------------------------------- the game's side */
static void link_send(const uint8_t *f, int n) {
    if (n <= 0 || n > FRAME_MAX) return;
    SDL_LockMutex(mx);
    frame_t *last = tx_tail != tx_head ? &txq[(tx_tail - 1) % TXQ] : NULL;
    if (ppp && last && last->len + n <= FRAME_MAX) {   /* the modem's bytes: into the last chunk not yet taken */
        memcpy(last->d + last->len, f, (size_t)n); last->len = (uint16_t)(last->len + n);
    } else if (tx_tail - tx_head < TXQ) { frame_t *q = &txq[tx_tail % TXQ]; q->len = (uint16_t)n; memcpy(q->d, f, (size_t)n); tx_tail++; }
    else tx_dropped++;
    if (!ppp) wanted = 1;                         /* the first frame the adapter sends connects */
    SDL_CondSignal(wake);
    SDL_UnlockMutex(mx);
}

static int link_recv(uint8_t *buf, int max) {
    int n = 0;
    SDL_LockMutex(mx);
    if (rx_head != rx_tail) {
        frame_t *q = &rxq[rx_head % RXQ];
        if (ppp) {                                /* a stream: as much as asked, the rest stays */
            n = q->len - rx_off < max ? q->len - rx_off : max;
            memcpy(buf, q->d + rx_off, (size_t)n);
            rx_off += n;
            if (rx_off == q->len) { rx_off = 0; rx_head++; }
        } else {
            n = q->len < max ? q->len : max;
            memcpy(buf, q->d, (size_t)n);
            rx_head++;
        }
    }
    SDL_UnlockMutex(mx);
    return n;
}

static void link_open(void) {
    SDL_LockMutex(mx); wanted = 1; SDL_CondSignal(wake); SDL_UnlockMutex(mx);
}
static void link_hangup(void) {
    SDL_LockMutex(mx);
    wanted = 0;                                   /* the thread closes the connection */
    rx_head = rx_tail = tx_head = tx_tail = rx_off = 0;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(mx);
    dc_net_rx_waiting = 0;
}

/* ---------------------------------------------------------------- the socket's thread */
static sock_t connect_to(const char *h, int p, char *err, size_t errn) {
    struct addrinfo hints, *res = NULL, *ai;
    char ps[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    snprintf(ps, sizeof ps, "%d", p);
    if (getaddrinfo(h, ps, &hints, &res) || !res) { snprintf(err, errn, "cannot find %s", h); return BAD_SOCK; }
    sock_t s = BAD_SOCK;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK) continue;
        set_nonblock(s, 1);                       /* a bounded wait: 5 s */
        int ok = connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0;
        if (!ok && IN_PROGRESS(sock_err())) {
            fd_set w; struct timeval tv = {5, 0};
            FD_ZERO(&w); FD_SET(s, &w);
            if (select((int)s + 1, NULL, &w, NULL, &tv) > 0) {
                int e = 0; socklen_t l = sizeof e;
                ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&e, &l) == 0 && e == 0;
            }
        }
        if (ok) { set_nonblock(s, 0); int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one); break; }
        close_sock(s); s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) snprintf(err, errn, "cannot connect to %s:%d", h, p);
    return s;
}

static int send_all(sock_t s, const uint8_t *p, int n) {
    while (n > 0) { int k = (int)send(s, (const char *)p, n, 0); if (k <= 0) return -1; p += k; n -= k; }
    return 0;
}

/* everything queued for the server: 0 ok, -1 the connection is gone */
static int flush_tx(sock_t s) {
    uint8_t buf[2 + FRAME_MAX];
    for (;;) {
        SDL_LockMutex(mx);
        if (tx_head == tx_tail) { SDL_UnlockMutex(mx); return 0; }
        frame_t *q = &txq[tx_head % TXQ];
        int n = q->len;
        if (ppp) { memcpy(buf, q->d, (size_t)n); tx_head++; }   /* taken now: the game may not add to it */
        else { buf[0] = (uint8_t)n; buf[1] = (uint8_t)(n >> 8); memcpy(buf + 2, q->d, (size_t)n); }
        SDL_UnlockMutex(mx);
        if (send_all(s, buf, ppp ? n : n + 2)) return -1;
        if (!ppp) { SDL_LockMutex(mx); tx_head++; SDL_UnlockMutex(mx); }   /* a frame: off the queue once sent */
    }
}

static int net_thread(void *arg) {
    (void)arg;
    static uint8_t in[2 * (2 + FRAME_MAX)];
    int backoff = 0;
    char err[200], msg[256];
    while (running) {
        SDL_LockMutex(mx);
        while (running && !wanted) SDL_CondWait(wake, mx);
        SDL_UnlockMutex(mx);
        if (!running) break;
        if (backoff) {                            /* wait the whole delay, unless asked to stop */
            Uint32 until = SDL_GetTicks() + (Uint32)backoff * 1000;
            SDL_LockMutex(mx);
            for (Sint32 left; running && (left = (Sint32)(until - SDL_GetTicks())) > 0; ) SDL_CondWaitTimeout(wake, mx, (Uint32)left);
            SDL_UnlockMutex(mx);
            if (!running) break;
        }
        snprintf(msg, sizeof msg, "connecting to %s:%d", host, port);
        set_status(msg);
        sock_t s = connect_to(host, port, err, sizeof err);
        if (s == BAD_SOCK) {
            backoff = backoff ? (backoff * 2 > 30 ? 30 : backoff * 2) : 1;
            snprintf(msg, sizeof msg, "%s; trying again in %d s", err, backoff);
            set_status(msg);
            continue;
        }
        static const uint8_t prolog[8] = {6, 0, 'D', 'C', 'N', 'E', 'T', 1};
        if (!ppp && send_all(s, prolog, 8)) { close_sock(s); backoff = 1; continue; }
        connected = 1; backoff = 0;
        snprintf(msg, sizeof msg, "connected to DCNet (%s:%d)%s", host, port, ppp ? " by modem" : "");
        set_status(msg);
        int have = 0;
        while (running && wanted) {
            if (flush_tx(s)) break;
            fd_set r; struct timeval tv = {0, 2000};          /* the queue is looked at every 2 ms */
            FD_ZERO(&r); FD_SET(s, &r);
            int k = select((int)s + 1, &r, NULL, NULL, &tv);
            if (k < 0) break;
            if (k == 0) continue;
            int n = (int)recv(s, (char *)in + have, (int)(sizeof in - (size_t)have), 0);
            if (n <= 0) break;
            if (ppp) {                                         /* bytes as they come */
                SDL_LockMutex(mx);
                if (wanted && rx_tail - rx_head < RXQ) { frame_t *q = &rxq[rx_tail % RXQ]; q->len = (uint16_t)n; memcpy(q->d, in, (size_t)n); rx_tail++; }
                else if (wanted) rx_dropped++;
                SDL_UnlockMutex(mx);
                dc_net_rx_waiting = 1;
                continue;
            }
            have += n;
            int at = 0, bad = 0;
            while (have - at >= 2) {
                int len = in[at] | in[at + 1] << 8;
                if (len == 0 || len > FRAME_MAX) { bad = 1; break; }
                if (have - at - 2 < len) break;
                SDL_LockMutex(mx);
                if (rx_tail - rx_head < RXQ) { frame_t *q = &rxq[rx_tail % RXQ]; q->len = (uint16_t)len; memcpy(q->d, in + at + 2, (size_t)len); rx_tail++; }
                else rx_dropped++;
                SDL_UnlockMutex(mx);
                dc_net_rx_waiting = 1;
                at += 2 + len;
            }
            if (bad) break;
            memmove(in, in + at, (size_t)(have - at)); have -= at;
        }
        close_sock(s);
        connected = 0;
        if (running && !wanted) set_status("hung up");
        else if (running && ppp) {                    /* the modem loses its carrier; the game calls again */
            SDL_LockMutex(mx); wanted = 0; SDL_UnlockMutex(mx);
            dc_net_carrier_lost = 1;
            set_status("connection lost (the game hangs up)");
        }
        else if (running) { backoff = 1; set_status("connection lost; connecting again"); }
    }
    return 0;
}

/* ---------------------------------------------------------------- dccore_front.h */
int dc_front_net_start(const char *server, int server_port) {
    if (thr) return 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { fprintf(stderr, "online: no Winsock\n"); return -1; }
#endif
#ifdef __SWITCH__
    socketInitializeDefault();
#endif
    snprintf(host, sizeof host, "%s", server && *server ? server : "dcnet.flyca.st");
    ppp = dc_modem_enabled && !dc_bba_enabled;
    port = server_port > 0 ? server_port : ppp ? 7654 : 7655;
    mx = SDL_CreateMutex(); wake = SDL_CreateCond();
    rx_head = rx_tail = tx_head = tx_tail = rx_off = 0;
    dc_net.send = link_send; dc_net.recv = link_recv; dc_net.open = link_open; dc_net.hangup = link_hangup;
    running = 1; wanted = 0;
    snprintf(status, sizeof status, "waiting for the game to go online (%s)", host);
    thr = SDL_CreateThread(net_thread, "dcnet", NULL);
    if (!thr) { running = 0; return -1; }
    return 0;
}

void dc_front_net_stop(void) {
    if (!thr) return;
    SDL_LockMutex(mx); running = 0; SDL_CondSignal(wake); SDL_UnlockMutex(mx);
    /* a connect or a select in progress ends within its timeout */
    SDL_WaitThread(thr, NULL); thr = NULL;
    dc_net.send = NULL; dc_net.recv = NULL; dc_net.open = NULL; dc_net.hangup = NULL;
    if (tx_dropped || rx_dropped) fprintf(stderr, "online: %llu frames not sent, %llu not received (queues full)\n",
                                          (unsigned long long)tx_dropped, (unsigned long long)rx_dropped);
#ifdef __SWITCH__
    socketExit();
#endif
}

int dc_front_net_status(char *buf, int n) {
    if (!thr) { snprintf(buf, (size_t)n, "offline"); return 0; }
    SDL_LockMutex(mx); snprintf(buf, (size_t)n, "%s", status); SDL_UnlockMutex(mx);
    return connected;
}
