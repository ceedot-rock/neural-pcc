#define _POSIX_C_SOURCE 200809L
#include "npcc.h"
#include "riser.h"
#include "tnssrc.h"
#include "workbench.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

static int read_all(const char *path, uint8_t **p, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NPCC_ERR_IO;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return NPCC_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NPCC_ERR_IO;
    }
    rewind(f);
    uint8_t *b = malloc((size_t)sz ? (size_t)sz : 1);
    if (!b) {
        fclose(f);
        return NPCC_ERR_NOMEM;
    }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NPCC_ERR_IO;
    }
    fclose(f);
    *p = b;
    *n = (size_t)sz;
    return 0;
}

static int write_all(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return NPCC_ERR_IO;
    if (n && fwrite(p, 1, n, f) != n) {
        fclose(f);
        return NPCC_ERR_IO;
    }
    fclose(f);
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmd_cd(int compress, int argc, char **argv) {
    if (argc < 1) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    uint8_t *out = NULL;
    size_t on = 0;
    if (compress)
        rc = npcc_compress(in, n, NULL, &out, &on);
    else
        rc = npcc_decompress(in, n, &out, &on);
    free(in);
    if (rc) {
        fprintf(stderr, "%s\n", npcc_strerror(rc));
        return 1;
    }
    if (argc >= 2)
        rc = write_all(argv[1], out, on);
    else if (fwrite(out, 1, on, stdout) != on)
        rc = NPCC_ERR_IO;
    free(out);
    return rc ? 1 : 0;
}

static int cmd_riser(int build, int argc, char **argv) {
    if (argc < 1) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    uint8_t *out = NULL;
    size_t on = 0;
    if (build)
        rc = aip_quik_build(in, n, &out, &on);
    else
        rc = aip_neat_destruct(in, n, &out, &on);
    free(in);
    if (rc) {
        fprintf(stderr, "aip %s rc=%d\n", build ? "quik.build" : "neat.destruct", rc);
        return 1;
    }
    if (argc >= 2)
        rc = write_all(argv[1], out, on);
    else if (fwrite(out, 1, on, stdout) != on)
        rc = NPCC_ERR_IO;
    free(out);
    return rc ? 1 : 0;
}

/* Temporary matrix tooling: force one inner candidate mode.
 *   npcc force MODE INPUT [OUTPUT]  -> raw inner frame [mode u8][payload]
 *   npcc dforce INPUT ORIG [OUTPUT]  -> decode inner frame (tnssrc_decode)
 * Exact byte counts for the per-mode matrix; default behavior unchanged. */
static int cmd_force(int argc, char **argv) {
    if (argc < 2) return 2;
    char *end = NULL;
    long m = strtol(argv[0], &end, 10);
    if (end == argv[0] || *end) {
        fprintf(stderr, "force: bad mode\n");
        return 2;
    }
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[1], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    char es[16];
    snprintf(es, sizeof es, "%ld", m);
    setenv("NPCC_FORCE_MODE", es, 1);
    uint8_t *out = NULL;
    size_t on = 0;
    rc = tnssrc_encode(in, n, &out, &on);
    free(in);
    if (rc) {
        fprintf(stderr, "force mode %ld: no candidate\n", m);
        return 1;
    }
    if (argc >= 3)
        rc = write_all(argv[2], out, on);
    else if (fwrite(out, 1, on, stdout) != on)
        rc = NPCC_ERR_IO;
    fprintf(stderr, "force mode=%ld packed=%zu\n", m, on);
    free(out);
    return rc ? 1 : 0;
}

static int cmd_dforce(int argc, char **argv) {
    if (argc < 2) return 2;
    char *end = NULL;
    unsigned long orig = strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end) {
        fprintf(stderr, "dforce: bad orig\n");
        return 2;
    }
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    uint8_t *out = NULL;
    size_t on = 0;
    rc = tnssrc_decode(in, n, (size_t)orig, &out, &on);
    free(in);
    if (rc) {
        fprintf(stderr, "dforce: decode failed\n");
        return 1;
    }
    if (argc >= 3)
        rc = write_all(argv[2], out, on);
    else if (fwrite(out, 1, on, stdout) != on)
        rc = NPCC_ERR_IO;
    free(out);
    return rc ? 1 : 0;
}

static int cmd_bench(int argc, char **argv) {
    if (argc < 1) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    const char *mode = argc >= 2 ? argv[1] : "full";
    if (!strcmp(mode, "riser")) {
        uint8_t *c = NULL, *y = NULL;
        size_t cn = 0, yn = 0;
        double t0 = now_s();
        rc = aip_quik_build(in, n, &c, &cn);
        double enc = now_s() - t0;
        if (rc) {
            fprintf(stderr, "quik.build fail\n");
            free(in);
            return 1;
        }
        t0 = now_s();
        rc = aip_neat_destruct(c, cn, &y, &yn);
        double dec = now_s() - t0;
        int ok = rc == 0 && yn == n && memcmp(y, in, n) == 0;
        printf("%s raw=%zu packed=%zu path=AIP kind=%u enc=%.3fs dec=%.3fs ok=%s\n",
               argv[0], n, cn, c ? (unsigned)c[5] : 0, enc, dec, ok ? "true" : "false");
        free(in);
        free(c);
        free(y);
        return ok ? 0 : 1;
    }
    NpccBudget b;
    if (!strcmp(mode, "classical"))
        npcc_budget_classical(&b);
    else if (!strcmp(mode, "ar"))
        npcc_budget_ar(&b);
    else
        npcc_budget_full(&b);
    uint8_t *c = NULL;
    size_t cn = 0;
    double t0 = now_s();
    rc = npcc_compress(in, n, &b, &c, &cn);
    double enc = now_s() - t0;
    if (rc) {
        fprintf(stderr, "compress: %s\n", npcc_strerror(rc));
        free(in);
        return 1;
    }
    int path = -1;
    uint64_t orig = 0;
    npcc_peek(c, cn, &path, &orig);
    uint8_t *y = NULL;
    size_t yn = 0;
    t0 = now_s();
    rc = npcc_decompress(c, cn, &y, &yn);
    double dec = now_s() - t0;
    int ok = rc == 0 && yn == n && memcmp(y, in, n) == 0;
    printf("%s raw=%zu packed=%zu path=%s enc=%.3fs dec=%.3fs ok=%s\n", argv[0], n, cn,
           npcc_pathway_name(path), enc, dec, ok ? "true" : "false");
    free(in);
    free(c);
    free(y);
    return ok ? 0 : 1;
}

static void http_send(int fd, int code, const char *ctype, const uint8_t *body, size_t n) {
    char hdr[256];
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : "Not Found";
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      code, msg, ctype, n);
    send(fd, hdr, (size_t)hn, 0);
    if (n) send(fd, body, n, 0);
}

static int cmd_serve(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        close(s);
        return 1;
    }
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) {
        close(s);
        return 1;
    }
    fprintf(stderr, "npcc listening on http://%s:%d\n", host, port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        char req[4096];
        ssize_t r = recv(c, req, sizeof req - 1, 0);
        if (r <= 0) {
            close(c);
            continue;
        }
        req[r] = 0;
        char method[8] = {0}, path[256] = {0};
        sscanf(req, "%7s %255s", method, path);
        if (!strcmp(method, "GET") && !strncmp(path, "/health", 7)) {
            http_send(c, 200, "text/plain", (const uint8_t *)"ok", 2);
            close(c);
            continue;
        }
        char *cl = NULL;
        for (char *s = req; *s; s++) {
            if ((s[0] == 'C' || s[0] == 'c') && strncasecmp(s, "Content-Length:", 15) == 0) {
                cl = s;
                break;
            }
        }
        size_t body_n = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
        char *hdr_end = strstr(req, "\r\n\r\n");
        size_t have = 0;
        uint8_t *body = NULL;
        if (hdr_end) {
            size_t prefix = (size_t)(r - (hdr_end + 4 - req));
            body = malloc(body_n ? body_n : 1);
            if (!body) {
                close(c);
                continue;
            }
            if (prefix > body_n) prefix = body_n;
            memcpy(body, hdr_end + 4, prefix);
            have = prefix;
            while (have < body_n) {
                ssize_t k = recv(c, body + have, body_n - have, 0);
                if (k <= 0) break;
                have += (size_t)k;
            }
        }
        NpccBudget b;
        npcc_budget_full(&b);
        char *q = strchr(path, '?');
        if (q && strstr(q, "budget=classical"))
            npcc_budget_classical(&b);
        else if (q && strstr(q, "budget=ar"))
            npcc_budget_ar(&b);
        if (!strcmp(method, "POST") && !strncmp(path, "/api/compress", 13)) {
            uint8_t *out = NULL;
            size_t on = 0;
            int rc = npcc_compress(body ? body : (uint8_t *)"", have, &b, &out, &on);
            if (rc)
                http_send(c, 400, "text/plain", (const uint8_t *)npcc_strerror(rc),
                          strlen(npcc_strerror(rc)));
            else
                http_send(c, 200, "application/octet-stream", out, on);
            free(out);
        } else if (!strcmp(method, "POST") && !strncmp(path, "/api/decompress", 15)) {
            uint8_t *out = NULL;
            size_t on = 0;
            int rc = npcc_decompress(body ? body : (uint8_t *)"", have, &out, &on);
            if (rc)
                http_send(c, 400, "text/plain", (const uint8_t *)npcc_strerror(rc),
                          strlen(npcc_strerror(rc)));
            else
                http_send(c, 200, "application/octet-stream", out, on);
            free(out);
        } else if (!strcmp(method, "POST") && !strncmp(path, "/api/quik", 9)) {
            uint8_t *out = NULL;
            size_t on = 0;
            int rc = aip_quik_build(body ? body : (uint8_t *)"", have, &out, &on);
            if (rc)
                http_send(c, 400, "text/plain", (const uint8_t *)"quik.build fail", 15);
            else
                http_send(c, 200, "application/octet-stream", out, on);
            free(out);
        } else if (!strcmp(method, "POST") && !strncmp(path, "/api/neat", 9)) {
            uint8_t *out = NULL;
            size_t on = 0;
            int rc = aip_neat_destruct(body ? body : (uint8_t *)"", have, &out, &on);
            if (rc)
                http_send(c, 400, "text/plain", (const uint8_t *)"neat.destruct fail", 18);
            else
                http_send(c, 200, "application/octet-stream", out, on);
            free(out);
        } else
            http_send(c, 404, "text/plain", (const uint8_t *)"not found", 9);
        free(body);
        close(c);
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: npcc c|d INPUT [OUTPUT]\n"
            "       npcc quik|neat INPUT [OUTPUT]\n"
            "       npcc bench INPUT [full|ar|classical|riser]\n"
            "       npcc wbc IN OUT        (SickNode Bay-1 encode; NPCC_WB_FULL=1 for full battery)\n"
            "       npcc wbd IN ORIG OUT   (SickNode decode)\n"
            "       npcc wbscan FILE       (SickNode structure scan report)\n"
            "       npcc serve [host] [port]\n");
}

static const char *wb_cli_name(int mode) {
    switch (mode) {
    case WB_MODE_RAW: return "raw";
    case WB_MODE_COLUMNAR: return "columnar";
    case WB_MODE_DELTA8: return "delta8";
    case WB_MODE_DELTA16: return "delta16";
    case WB_MODE_DELTA24: return "delta24";
    case WB_MODE_DELTA32: return "delta32";
    case WB_MODE_XOR16: return "xor16";
    case WB_MODE_XOR32: return "xor32";
    case WB_MODE_EXE: return "exe";
    case WB_MODE_IMG2D: return "img2d";
    case WB_MODE_BITPLANE: return "bitplane";
    case WB_MODE_SHUFFLE: return "shuffle";
    case WB_MODE_ROUTED: return "routed";
    default: return "?";
    }
}

/* SickNode workbench: Bay-1 transform conductor.
 *   npcc wbc IN OUT   -> wb_encode (Bay 1; NPCC_WB_FULL=1 runs full battery)
 *   npcc wbd IN ORIG OUT -> wb_decode
 *   npcc wbscan FILE  -> structure scan report + Bay-1 shortlist */
/* SickNode workbench: Bay-1 transform conductor.
 *   npcc wbc IN OUT   -> wb_encode_full (Bay 1; NPCC_WB_FULL=1 runs full battery)
 *   npcc wbd IN ORIG OUT -> wb_decode
 *   npcc wbscan FILE  -> machine-parseable structure scan + Bay-1 shortlist
 *   npcc wbroute IN [AUDIT] -> hot-loop routing map + audit dump
 * Every wbc run appends Tier-3 rows to MIXER_LOG.tsv (spec v3). */
static void wb_cli_join_names(char *dst, size_t dn, const wb_candidate_t *cands,
                              int nc) {
    size_t j = 0;
    for (int i = 0; i < nc && j + 1 < dn; i++) {
        if (i) dst[j++] = '>';
        const char *nm = wb_cli_name(cands[i].mode);
        for (size_t k = 0; nm[k] && j + 1 < dn; k++) dst[j++] = nm[k];
    }
    dst[j] = 0;
}

static const char *wb_cli_winner_name(int mode) {
    if (mode == WB_MODE_RAW) return "raw";
    return wb_cli_name(mode);
}

static int cmd_wbc(int argc, char **argv) {
    if (argc < 2) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    const char *e = getenv("NPCC_WB_FULL");
    int full = e && !strcmp(e, "1");
    wb_result_t res;
    double t0 = now_s();
    rc = wb_encode_full(in, n, full, &res);
    double enc = now_s() - t0;
    free(in);
    if (rc) {
        fprintf(stderr, "wbc: no candidate\n");
        return 1;
    }
    char seats[512];
    wb_cli_join_names(seats, sizeof seats, res.seats, res.nseats);
    const char *winner = wb_cli_winner_name(res.winner_mode);
    /* machine-parseable single line: seated candidates in order + winner */
    printf("file=%s raw=%zu packed=%zu winner=%s seats=%s full=%d routed=%d enc=%.3fs\n",
           argv[0], n, res.on, winner, seats, full, res.routed_built, enc);
    /* Tier-3: append-only mixer logging (no learning here) */
    const wb_scan_t *rp = &res.rep;
    if (wb_mixer_log_row(argv[0], "whole", n, rp->entropy, rp->h1,
                         rp->alpha_util, rp->e8e9_per_mb, rp->record_period,
                         rp->period_conf, rp->smooth8, rp->smooth16,
                         rp->smooth24, rp->smooth32, rp->is_tar,
                         rp->homogeneous, rp->bwt_friendly, seats, winner,
                         res.on))
        fprintf(stderr, "wbc: mixer log append failed\n");
    if (res.routed_built) {
        wb_mixer_log_row(argv[0], "routed", n, rp->entropy, rp->h1,
                         rp->alpha_util, rp->e8e9_per_mb, rp->record_period,
                         rp->period_conf, rp->smooth8, rp->smooth16,
                         rp->smooth24, rp->smooth32, rp->is_tar,
                         rp->homogeneous, rp->bwt_friendly, "-", "routed",
                         res.routed_bytes);
        for (size_t b = 0; b < res.nblocks; b++) {
            const wb_block_route_t *rb = &res.routes[b];
            char bseats[512];
            size_t j = 0;
            char bidx[32];
            snprintf(bidx, sizeof bidx, "%zu", b);
            for (int i = 0; i < rb->ncands && j + 1 < sizeof bseats; i++) {
                if (i) bseats[j++] = ',';
                const char *nm = wb_cli_name(rb->cand_modes[i]);
                for (size_t k = 0; nm[k] && j + 1 < sizeof bseats; k++)
                    bseats[j++] = nm[k];
                j += (size_t)snprintf(bseats + j, sizeof bseats - j, ":%s",
                                      rb->cand_bytes[i] == (size_t)-1
                                          ? "X"
                                          : "");
                if (rb->cand_bytes[i] != (size_t)-1)
                    j += (size_t)snprintf(bseats + j, sizeof bseats - j, "%zu",
                                         rb->cand_bytes[i]);
            }
            bseats[j] = 0;
            size_t bn = n - b * res.block_size > res.block_size
                            ? res.block_size
                            : n - b * res.block_size;
            wb_mixer_log_row(argv[0], bidx, bn, rb->entropy, rb->h1,
                             rb->alpha_util, rb->e8e9_per_mb, rp->record_period,
                             rp->period_conf, rb->smooth8, rb->smooth16,
                             rb->smooth24, rb->smooth32, rp->is_tar,
                             rp->homogeneous, rp->bwt_friendly, bseats,
                             wb_cli_winner_name(rb->mode),
                             res.routed_block_bytes[b]);
        }
    }
    rc = write_all(argv[1], res.out, res.on);
    wb_result_free(&res);
    return rc ? 1 : 0;
}

static int cmd_wbd(int argc, char **argv) {
    if (argc < 3) return 2;
    char *end = NULL;
    unsigned long orig = strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end) {
        fprintf(stderr, "wbd: bad orig\n");
        return 2;
    }
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    uint8_t *out = NULL;
    size_t on = 0;
    double t0 = now_s();
    rc = wb_decode(in, n, (size_t)orig, &out, &on);
    double dec = now_s() - t0;
    free(in);
    if (rc) {
        fprintf(stderr, "wbd: decode failed\n");
        return 1;
    }
    int ok = on == (size_t)orig;
    printf("%s packed=%zu raw=%zu dec=%.3fs ok=%s\n", argv[0], n, on, dec,
           ok ? "true" : "false");
    rc = write_all(argv[2], out, on);
    free(out);
    return (rc || !ok) ? 1 : 0;
}

static int cmd_wbscan(int argc, char **argv) {
    if (argc < 1) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    wb_scan_t rep;
    double t0 = now_s();
    rc = wb_scan(in, n, &rep);
    double dt = now_s() - t0;
    free(in);
    if (rc) {
        fprintf(stderr, "wbscan: failed\n");
        return 1;
    }
    wb_candidate_t cands[13];
    int nc = wb_bay1(&rep, cands, 0);
    char seats[512];
    wb_cli_join_names(seats, sizeof seats, cands, nc);
    /* machine-parseable single line (spec v3 Tier-2) */
    printf("file=%s size=%zu entropy=%.3f h1=%.3f alpha=%.3f e8e9_per_mb=%.1f "
           "period=%zu period_conf=%.3f "
           "smooth8=%d smooth16=%d smooth24=%d smooth32=%d "
           "is_tar=%d homogeneous=%d bwt_friendly=%d "
           "seats=%s scan=%.3fs\n",
           argv[0], rep.size, rep.entropy, rep.h1, rep.alpha_util,
           rep.e8e9_per_mb, rep.record_period, rep.period_conf, rep.smooth8,
           rep.smooth16, rep.smooth24, rep.smooth32, rep.is_tar,
           rep.homogeneous, rep.bwt_friendly, seats, dt);
    return 0;
}

/* Hot-loop routing audit: scan + route, print a one-line summary, and
 * write the routing audit dump (format in ROUTING.md) to AUDIT or stdout. */
static int cmd_wbroute(int argc, char **argv) {
    if (argc < 1) return 2;
    uint8_t *in = NULL;
    size_t n = 0;
    int rc = read_all(argv[0], &in, &n);
    if (rc) {
        fprintf(stderr, "read: %s\n", npcc_strerror(rc));
        return 1;
    }
    wb_scan_t rep;
    if (wb_scan(in, n, &rep)) {
        fprintf(stderr, "wbroute: scan failed\n");
        free(in);
        return 1;
    }
    size_t bs = wb_hot_block_size();
    wb_block_route_t *routes = NULL;
    size_t nblocks = 0;
    double t0 = now_s();
    rc = wb_route(in, n, bs, &rep, &routes, &nblocks);
    double rdt = now_s() - t0;
    if (rc) {
        fprintf(stderr, "wbroute: routing failed\n");
        free(in);
        return 1;
    }
    char *audit = wb_route_audit(argv[0], bs, routes, nblocks);
    /* route histogram for the summary line */
    char hist[256] = {0};
    {
        int counts[32] = {0};
        for (size_t b = 0; b < nblocks; b++)
            if (routes[b].mode >= 0 && routes[b].mode < 32)
                counts[routes[b].mode]++;
        size_t j = 0;
        for (int m = 0; m < 32 && j + 1 < sizeof hist; m++) {
            if (!counts[m]) continue;
            if (j) hist[j++] = ',';
            j += (size_t)snprintf(hist + j, sizeof hist - j, "%s:%d",
                                  wb_cli_name(m), counts[m]);
        }
    }
    printf("file=%s size=%zu block_size=%zu nblocks=%zu routes=%s route=%.3fs\n",
           argv[0], n, bs, nblocks, hist, rdt);
    FILE *f = stdout;
    if (argc >= 2) {
        f = fopen(argv[1], "w");
        if (!f) {
            fprintf(stderr, "wbroute: cannot write %s\n", argv[1]);
            free(audit);
            free(routes);
            free(in);
            return 1;
        }
    }
    if (audit) fputs(audit, f);
    if (f != stdout) fclose(f);
    free(audit);
    free(routes);
    free(in);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "c")) return cmd_cd(1, argc - 2, argv + 2);
    if (!strcmp(argv[1], "d")) return cmd_cd(0, argc - 2, argv + 2);
    if (!strcmp(argv[1], "quik") || !strcmp(argv[1], "build"))
        return cmd_riser(1, argc - 2, argv + 2);
    if (!strcmp(argv[1], "neat") || !strcmp(argv[1], "destruct"))
        return cmd_riser(0, argc - 2, argv + 2);
    if (!strcmp(argv[1], "bench")) return cmd_bench(argc - 2, argv + 2);
    if (!strcmp(argv[1], "force")) return cmd_force(argc - 2, argv + 2);
    if (!strcmp(argv[1], "dforce")) return cmd_dforce(argc - 2, argv + 2);
    if (!strcmp(argv[1], "wbc")) return cmd_wbc(argc - 2, argv + 2);
    if (!strcmp(argv[1], "wbd")) return cmd_wbd(argc - 2, argv + 2);
    if (!strcmp(argv[1], "wbscan")) return cmd_wbscan(argc - 2, argv + 2);
    if (!strcmp(argv[1], "wbroute")) return cmd_wbroute(argc - 2, argv + 2);
    if (!strcmp(argv[1], "serve")) {
        const char *host = argc > 2 ? argv[2] : "127.0.0.1";
        int port = argc > 3 ? atoi(argv[3]) : 8080;
        return cmd_serve(host, port);
    }
    usage();
    return 2;
}
