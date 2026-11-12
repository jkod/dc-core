/* Online play's transport (frontend/dccore_net.c) against a stand-in DCNet server on this machine:
 * the prolog, the length-framed Ethernet frames both ways through the adapter's link (dc_net), and a
 * new connection after the server drops the first one. POSIX sockets (run on Linux). */
#include "dccore_front.h"
#include "dccore/net.h"
#include <SDL.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { uint32_t addr, end; void *fn; } dc_fn_entry;
const dc_fn_entry dc_fn_table[1] = {{0, 0, 0}};
const size_t dc_fn_count = 0;

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static int lsock, lport;
static int read_n(int s, uint8_t *p, int n) { while (n > 0) { int k = (int)recv(s, p, (size_t)n, 0); if (k <= 0) return -1; p += k; n -= k; } return 0; }

/* the server: per connection, check the prolog, then answer each frame with its bytes + 1 */
static int sessions, prolog_ok, frames_seen;
static volatile int ppp_bytes, ppp_closed;
static int server(void *arg) {
    (void)arg;
    for (int conn = 0; conn < 2; conn++) {
        int s = accept(lsock, NULL, NULL);
        if (s < 0) return 0;
        sessions++;
        uint8_t pr[8];
        if (!read_n(s, pr, 8) && !memcmp(pr, "\x06\x00" "DCNET\x01", 8)) prolog_ok++;
        for (int i = 0; i < (conn == 0 ? 1 : 3); i++) {
            uint8_t h[2], f[1600];
            if (read_n(s, h, 2)) break;
            int n = h[0] | h[1] << 8;
            if (n > 1600 || read_n(s, f, n)) break;
            frames_seen++;
            for (int k = 0; k < n; k++) f[k]++;
            send(s, h, 2, 0); send(s, f, (size_t)n, 0);
        }
        close(s);                                     /* the first connection drops after one frame */
    }
    /* the modem: raw bytes, no prolog; echoed + 1 until the client hangs up */
    int s = accept(lsock, NULL, NULL);
    if (s < 0) return 0;
    sessions++;
    uint8_t b[256];
    for (;;) {
        int n = (int)recv(s, b, sizeof b, 0);
        if (n <= 0) break;
        ppp_bytes += n;
        for (int k = 0; k < n; k++) b[k]++;
        send(s, b, (size_t)n, 0);
    }
    ppp_closed = 1;
    close(s);
    return 0;
}

static int wait_frame(uint8_t *buf) {
    for (int t = 0; t < 400; t++) {
        if (dc_net_rx_waiting) { int n = dc_net.recv(buf, 1600); if (n) return n; dc_net_rx_waiting = 0; }
        SDL_Delay(10);
    }
    return 0;
}

int main(void) {
    struct sockaddr_in a; socklen_t al = sizeof a;
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(lsock, (struct sockaddr *)&a, sizeof a) || listen(lsock, 2)) { printf("no socket: skipped\n"); return 0; }
    getsockname(lsock, (struct sockaddr *)&a, &al); lport = ntohs(a.sin_port);
    SDL_Thread *st = SDL_CreateThread(server, "server", NULL);

    CHECK(dc_front_net_start("127.0.0.1", lport) == 0);
    char status[256];
    CHECK(dc_front_net_status(status, sizeof status) == 0);   /* nothing sent: not connected yet */
    SDL_Delay(50);
    CHECK(sessions == 0);

    uint8_t f[100], g[1600];
    for (int i = 0; i < 100; i++) f[i] = (uint8_t)i;
    dc_net.send(f, 100);                               /* connects */
    int n = wait_frame(g);
    CHECK(n == 100 && g[0] == 1 && g[99] == 100);
    CHECK(prolog_ok == 1);

    /* the server closed: the link connects again (after a second) and goes on */
    SDL_Delay(300);
    for (int i = 0; i < 3; i++) { f[0] = (uint8_t)(10 * i); dc_net.send(f, 60); }
    for (int i = 0; i < 3; i++) { n = wait_frame(g); CHECK(n == 60 && g[0] == (uint8_t)(10 * i + 1)); }
    CHECK(sessions == 2 && prolog_ok == 2 && frames_seen == 4);
    dc_front_net_status(status, sizeof status);
    printf("status: %s\n", status);

    dc_front_net_stop();

    /* the modem: nothing connects until the line is up; bytes as a stream */
    dc_modem_enabled = 1;
    CHECK(dc_front_net_start("127.0.0.1", lport) == 0);
    dc_net.send((const uint8_t *)"\x7E\xFF", 2);       /* before the line is up: kept, no connection */
    SDL_Delay(100);
    CHECK(sessions == 2);
    dc_net.open();
    dc_net.send((const uint8_t *)"\x03\xC0", 2);
    uint8_t got[8]; int ngot = 0;
    for (int t = 0; t < 300 && ngot < 4; t++) {
        if (dc_net_rx_waiting) { int k = dc_net.recv(got + ngot, 1); if (k) ngot += k; else dc_net_rx_waiting = 0; }
        else SDL_Delay(10);
    }
    CHECK(ngot == 4 && !memcmp(got, "\x7F\x00\x04\xC1", 4));
    CHECK(sessions == 3 && ppp_bytes == 4);
    dc_net.hangup();
    for (int t = 0; t < 100 && !ppp_closed; t++) SDL_Delay(10);
    CHECK(ppp_closed);
    dc_front_net_stop();
    SDL_WaitThread(st, NULL);
    close(lsock);
    printf(fails ? "net: %d FAILED\n" : "net: all passed\n", fails);
    return fails != 0;
}
