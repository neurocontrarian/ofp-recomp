/*
 * ofp_net.c - multiplayer today. GameSpy closed in 2014; the game still asks
 * master.gamespy.com for the Internet server list and reports its own games
 * there. The community runs a replacement speaking the same protocol
 * (master.ofpisnotdead.com, PowerServer): the name is answered with it.
 * env OFP_MASTER (Options.txt MASTER_SERVER) picks another one.
 *
 * Internet servers in the game's own list: the community master's servers
 * answer the LAN search the game broadcasts (to port 2302) exactly as a LAN
 * server does, with their name, mission, players and so the ping. So each
 * such broadcast (wsock32's broadcast hook) also goes to every server the master lists (fetched at
 * start, every 5 minutes after; a list found on 2026-11-06 if the master is
 * out of reach), and they appear in the multiplayer screen like LAN games.
 * env OFP_INTERNET_LIST=0 (Options.txt SERVEURS_INTERNET=0) turns it off.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "win32hle.h"
#include "ofp.h"

#define DEFAULT_MASTER "master.ofpisnotdead.com"

static recomp_func_t o_gethostbyname;
static uint32_t g_master_va;                         /* the replacement name, in guest memory */
/* the options, read once (ofp_net_register) */
static const char *g_master = DEFAULT_MASTER;
static int g_list_on = 1, g_all_versions;
static uint32_t g_refresh_ms = 2000;

static void n_gethostbyname(void) {                  /* (name) */
    const char *name = ASTR(0);
    int gs = name && !strcasecmp(name, "master.gamespy.com");
    if (gs) {
        if (!g_master_va) g_master_va = hle_guest_strdup(g_master);
        MEM32(g_esp + 4) = g_master_va;               /* the argument, as the game passed it */
    }
    o_gethostbyname();
    uint32_t he = g_eax;
    char ip[32] = "not found";
    if (he) {
        uint32_t list = MEM32(he + 12), a = list ? MEM32(list) : 0;
        if (a) snprintf(ip, sizeof ip, "%u.%u.%u.%u", MEM8(a), MEM8(a + 1), MEM8(a + 2), MEM8(a + 3));
    }
    perf_note("network: name %s%s%s%s -> %s", gs ? "master.gamespy.com" : name ? name : "", gs ? " (GameSpy, closed: asked " : "",
              gs ? g_master : "", gs ? ")" : "", ip);
}

/* ---- the community master's list (GameSpy protocol 1 on TCP 28900) -------- */
#define MAX_SRV 512
typedef struct { uint32_t ip; uint16_t port; } srv_t;      /* network order; the game port */
static srv_t g_srv[MAX_SRV];
static volatile int g_nsrv;
static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;

/* servers seen on master.ofpisnotdead.com on 2026-11-06, for when it is down */
static const char *const g_known[] = {
    "65.78.59.74:2302", "212.110.214.66:2305", "212.110.214.66:2102", "212.110.214.66:2205", "212.110.214.66:2302",
    "95.31.5.208:2302", "87.228.56.11:2383", "87.228.56.11:2335", "87.228.56.11:2323", "87.228.56.11:2341",
    "87.228.56.11:2326", "87.228.56.11:2344", "87.228.56.11:2329", "87.228.56.11:2314", "87.228.56.11:2332",
    "87.228.56.11:2302", "73.140.36.188:2305", "73.140.36.188:2302", "185.128.105.82:2626", "185.128.105.82:2324",
    "185.128.105.82:2425", "143.177.195.34:2383", "143.177.195.34:2402", "143.177.195.34:2342", "89.163.148.52:4500",
    "89.163.148.52:5000", "95.31.244.162:2302", "89.163.140.196:4500", "89.163.140.196:5000", "94.19.180.157:2302",
    "45.138.48.120:2308", 0 };

/* the challenge answer: GameSpy's "validate" for a 6-character challenge and
 * a game key, enctype 3 (the key XORed in, as PowerServer checks it) */
static char b64(int r) { return r < 26 ? (char)('A' + r) : r < 52 ? (char)('a' + r - 26) : r < 62 ? (char)('0' + r - 52) : r == 62 ? '+' : '/'; }
static void gs_validate(const char *sec, const char *key, char out[9]) {
    uint8_t t[256], k[6];
    size_t kl = strlen(key);
    for (int i = 0; i < 256; i++) t[i] = (uint8_t)i;
    for (int i = 0, a = 0; i < 256; i++) {
        a = (a + t[i] + (uint8_t)key[i % kl]) & 255;
        uint8_t x = t[a]; t[a] = t[i], t[i] = x;
    }
    for (int i = 0, a = 0, b = 0; i < 6; i++) {
        uint8_t c = (uint8_t)sec[i];
        a = (a + c + 1) & 255;
        uint8_t x = t[a];
        b = (b + x) & 255;
        uint8_t y = t[b];
        t[b] = x, t[a] = y;
        k[i] = (uint8_t)(c ^ t[(x + y) & 255]) ^ (uint8_t)key[i % kl];
    }
    for (int j = 0, o = 0; j < 6; j += 3) {
        out[o++] = b64(k[j] >> 2), out[o++] = b64((k[j] & 3) << 4 | k[j + 1] >> 4);
        out[o++] = b64((k[j + 1] & 15) << 2 | k[j + 2] >> 6), out[o++] = b64(k[j + 2] & 63);
    }
    out[8] = 0;
}
static int fetch_master(void) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_master, "28900", &hints, &ai) || !ai) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0), got = -1;
    struct timeval tv = { 8, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
        char hello[256] = "";
        ssize_t n = recv(fd, hello, sizeof hello - 1, 0);
        const char *sec = n > 0 ? strstr(hello, "\\secure\\") : NULL;
        if (sec && strlen(sec + 8) >= 6) {
            char v[9], req[256];
            gs_validate(sec + 8, "Y3k7x1", v);
            snprintf(req, sizeof req, "\\gamename\\opflashr\\enctype\\3\\validate\\%s\\final\\\\list\\cmp\\gamename\\opflashr\\final\\", v);
            send(fd, req, strlen(req), MSG_NOSIGNAL);
            static uint8_t buf[12 + MAX_SRV * 6 + 4096];
            size_t len = 0;
            while (len < sizeof buf && (n = recv(fd, buf + len, sizeof buf - len, 0)) > 0) len += (size_t)n;
            /* FF FF FF FF, IPv4 bytes, IPv6 bytes (big-endian), then 6 bytes a server: IP, query port */
            if (len >= 12 && buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF) {
                uint32_t n4 = (uint32_t)buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7];
                int k = 0;
                pthread_mutex_lock(&g_srv_mu);
                for (uint32_t o = 12; o + 6 <= 12 + n4 && o + 6 <= len && k < MAX_SRV; o += 6, k++) {
                    memcpy(&g_srv[k].ip, buf + o, 4);
                    g_srv[k].port = htons((uint16_t)(((uint16_t)buf[o + 4] << 8 | buf[o + 5]) - 1));   /* the game port: query - 1 */
                }
                g_nsrv = k;
                pthread_mutex_unlock(&g_srv_mu);
                got = k;
            }
        }
    }
    if (fd >= 0) close(fd);
    freeaddrinfo(ai);
    return got;
}
static void use_known(void) {
    int k = 0;
    pthread_mutex_lock(&g_srv_mu);
    for (; g_known[k] && k < MAX_SRV; k++) {
        char h[64];
        snprintf(h, sizeof h, "%s", g_known[k]);
        char *c = strchr(h, ':');
        *c = 0;
        inet_pton(AF_INET, h, &g_srv[k].ip);
        g_srv[k].port = htons((uint16_t)atoi(c + 1));
    }
    g_nsrv = k;
    pthread_mutex_unlock(&g_srv_mu);
}
/* Keeps the servers that run this game's version (1.99): asked with the
 * GameSpy status query on their query port (the game port + 1). The others
 * show in the game's list with a red cross, and cannot be joined. */
static void keep_compatible(void) {
    if (g_all_versions) return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    pthread_mutex_lock(&g_srv_mu);
    int n = g_nsrv;
    static srv_t cand[MAX_SRV];                       /* static: 3 KB, the thread's stack is small */
    memcpy(cand, g_srv, sizeof cand[0] * (size_t)n);
    pthread_mutex_unlock(&g_srv_mu);
    for (int i = 0; i < n; i++) {
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET, a.sin_addr.s_addr = cand[i].ip, a.sin_port = htons((uint16_t)(ntohs(cand[i].port) + 1));
        sendto(fd, "\\status\\", 8, MSG_NOSIGNAL, (struct sockaddr *)&a, sizeof a);
    }
    static uint8_t ok[MAX_SRV];
    memset(ok, 0, sizeof ok);
    struct timeval tv = { 0, 200000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[4096];
    for (int t = 0; t < 10; t++) {                    /* 2 s for the answers */
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r;
        while ((r = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl)) > 0) {
            buf[r] = 0;
            const char *v = strstr(buf, "\\gamever\\");
            if (!v || strncmp(v + 9, "1.99", 4)) continue;
            for (int i = 0; i < n; i++)
                if (cand[i].ip == from.sin_addr.s_addr && ntohs(cand[i].port) + 1 == ntohs(from.sin_port)) ok[i] = 1;
            fl = sizeof from;
        }
    }
    close(fd);
    int k = 0;
    pthread_mutex_lock(&g_srv_mu);
    for (int i = 0; i < n; i++) if (ok[i]) g_srv[k++] = cand[i];
    g_nsrv = k;
    pthread_mutex_unlock(&g_srv_mu);
    perf_note("network: %d of %d Internet servers run version 1.99 and answer", k, n);
}

static void *master_thread(void *arg) {
    (void)arg;
    for (int first = 1;; first = 0) {
        int n = fetch_master();
        if (n >= 0) perf_note("network: %d Internet servers from %s", n, g_master);
        else if (first) { use_known(); perf_note("network: %s out of reach: %d servers known from 2026-11-06", g_master, g_nsrv); }
        if (n >= 0 || first) keep_compatible();
        sleep(300);
    }
    return NULL;
}
/* The game repeats its LAN search every ~4 s, 15 packets at a time, and drops
 * a server that has not answered for a few seconds. So each search also goes
 * to every Internet server, once: the first packet of a burst (none for
 * OFP_INTERNET_REFRESH seconds after, 2 by default). */
static void on_broadcast(int fd, const void *buf, uint32_t len, unsigned port) {
    static uint32_t last_ms;
    if (port != 2302 || !g_nsrv) return;
    uint32_t now = hle_ticks_ms();
    if (last_ms && now - last_ms < g_refresh_ms) return;
    last_ms = now;
    pthread_mutex_lock(&g_srv_mu);
    for (int i = 0; i < g_nsrv; i++) {
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET, a.sin_addr.s_addr = g_srv[i].ip, a.sin_port = g_srv[i].port;
        sendto(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
    }
    static int told;
    if (!told++) perf_note("network: the LAN search also goes to %d Internet servers", g_nsrv);
    pthread_mutex_unlock(&g_srv_mu);
}

static const win32hle_shim g_net_shims[] = {
    { "wsock32.dll#52", n_gethostbyname },
    { "gethostbyname", n_gethostbyname },
    { 0, 0 }
};
void ofp_net_register(void) {
    const char *m = getenv("OFP_MASTER");
    if (m && *m) g_master = m;
    g_list_on = ofp_env_int("OFP_INTERNET_LIST", 1) != 0;
    g_all_versions = ofp_env_int("OFP_INTERNET_ALLVERSIONS", 0) != 0;
    int r = ofp_env_int("OFP_INTERNET_REFRESH", 2);
    g_refresh_ms = (uint32_t)(r > 0 ? r : 2) * 1000u;
    o_gethostbyname = ofp_find_shim(win32hle_wsock32, "gethostbyname");
    if (o_gethostbyname) win32hle_register(g_net_shims);
    if (g_list_on) {
        hle_ws_broadcast_hook = on_broadcast;
        pthread_t t;
        pthread_create(&t, NULL, master_thread, NULL);
        pthread_detach(t);
    }
}
