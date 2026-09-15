#define _POSIX_C_SOURCE 200809L
#include "npcc.h"
#include "riser.h"

#include <errno.h>
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
            "       npcc serve [host] [port]\n");
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
    if (!strcmp(argv[1], "serve")) {
        const char *host = argc > 2 ? argv[2] : "127.0.0.1";
        int port = argc > 3 ? atoi(argv[3]) : 8080;
        return cmd_serve(host, port);
    }
    usage();
    return 2;
}
