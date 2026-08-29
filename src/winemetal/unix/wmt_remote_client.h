/* ml762: remote winemetal backend -- route unixcalls to a host Metal daemon.
 *
 * In remote mode winemetal creates NO local Metal objects. Every obj_handle_t
 * in the process is therefore a tagged remote handle, and the tag is what makes
 * a partial switch detectable: a local pointer reaching the wire, or a remote
 * id reaching a local Metal call, is a named error at the point of misuse
 * rather than a subtly wrong frame on another machine.
 *
 * ⚠️ The mode is decided ONCE and never changes. Flipping it mid-process would
 * leave handles from both address spaces alive simultaneously, which is exactly
 * the state the tag exists to make impossible.
 *
 * Calls not yet routed fail BY NAME. Discovering the remaining surface by
 * running is the approach that has worked throughout: the command census found
 * 15 of 38 opcodes, the API census 43 of 127 entries -- both far smaller than
 * the speculative estimate.
 */
#ifndef WMT_REMOTE_CLIENT_H
#define WMT_REMOTE_CLIENT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "../../../../remote-metal/protocol.h"

static int  wmtr_fd   = -1;
static int  wmtr_mode = -1;          /* -1 undecided, 0 local, 1 remote */
static pthread_once_t wmtr_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t wmtr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t wmtr_seq;

/* Defined below; the mode gate registers it with atexit. */
static void wmtr_report_unrouted(void);

static int wmtr_rd(void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t r = read(wmtr_fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
static int wmtr_wr(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t r = write(wmtr_fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}

/* One synchronous call, serialised. The transport measured 0.06 ms per round
 * trip and command count is nearly free once batched, so a lock here costs far
 * less than the correctness it buys while the backend is being brought up. */
static uint32_t wmtr_call(uint16_t op, const void *arg, uint32_t alen,
                          void *out, uint32_t ocap, uint32_t *olen) {
    if (out && ocap) memset(out, 0, ocap);
    if (olen) *olen = 0;
    pthread_mutex_lock(&wmtr_lock);
    struct rm_hdr h = { RM_MAGIC, RM_VERSION, op, ++wmtr_seq, 0, alen, 0 };
    uint32_t status = 0xffffffffu;
    if (wmtr_wr(&h, sizeof h)) goto out;
    if (alen && wmtr_wr(arg, alen)) goto out;
    struct rm_hdr r;
    if (wmtr_rd(&r, sizeof r)) goto out;
    if (r.magic != RM_MAGIC || r.version != RM_VERSION ||
        r.opcode != op || r.seq != h.seq) {
        /* Name the field that actually differs. Printing only op and seq once
         * reported "mismatch" with both matching, which reads as a protocol
         * bug when the real cause was a guest built before a version bump. */
        fprintf(stderr, "[wmt-remote] reply mismatch:%s%s%s%s"
                        " (magic %08x/%08x, version %u/%u, op %u/%u, seq %u/%u)\n",
                r.magic   != RM_MAGIC   ? " MAGIC"   : "",
                r.version != RM_VERSION ? " VERSION" : "",
                r.opcode  != op         ? " OPCODE"  : "",
                r.seq     != h.seq      ? " SEQ"     : "",
                r.magic, RM_MAGIC, r.version, RM_VERSION, r.opcode, op, r.seq, h.seq);
        if (r.version != RM_VERSION)
            fprintf(stderr, "[wmt-remote] the daemon speaks v%u and this build speaks v%u"
                            " -- rebuild whichever is older\n", r.version, RM_VERSION);
        goto out;
    }
    {
        uint32_t n = r.payload_len, take = (out && n) ? (n < ocap ? n : ocap) : 0;
        if (take && wmtr_rd(out, take)) goto out;
        for (uint32_t left = n - take; left; ) {
            uint8_t sink[4096];
            uint32_t c = left < sizeof sink ? left : (uint32_t)sizeof sink;
            if (wmtr_rd(sink, c)) goto out;
            left -= c;
        }
        if (olen) *olen = take;
    }
    status = r.status;
out:
    pthread_mutex_unlock(&wmtr_lock);
    return status;
}

/* Decided once. DXMT_REMOTE_METAL=<host-ip> turns it on. */
/* Decided exactly once. The gate sits on EVERY dispatch entry, so without
 * this two threads racing the first call would each open a socket: one leaks
 * and wmtr_fd changes under a call already in flight on the other.
 * wmtr_call() does not consult the mode, so there is no recursion here. */
static void wmtr_init(void) {
    const char *host = getenv("DXMT_REMOTE_METAL");
    const char *tok  = getenv("RMETAL_TOKEN");
    if (!host || !*host) { wmtr_mode = 0; return; }
    if (!tok || !*tok) {
        fprintf(stderr, "[wmt-remote] DXMT_REMOTE_METAL set but RMETAL_TOKEN missing -- staying local\n");
        wmtr_mode = 0; return;
    }
    wmtr_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RM_PORT) };
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 ||
        connect(wmtr_fd, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "[wmt-remote] cannot reach %s:%d -- staying local\n", host, RM_PORT);
        close(wmtr_fd); wmtr_fd = -1; wmtr_mode = 0; return;
    }
    int one = 1;
    setsockopt(wmtr_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(wmtr_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    struct timeval tv = { .tv_sec = 30 };
    setsockopt(wmtr_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(wmtr_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    wmtr_mode = 1;   /* set before the auth call so the socket is usable */
    if (wmtr_call(RM_OP_PING, tok, (uint32_t)strlen(tok), 0, 0, 0) != RM_OK) {
        fprintf(stderr, "[wmt-remote] authentication rejected -- staying local\n");
        close(wmtr_fd); wmtr_fd = -1; wmtr_mode = 0; return;
    }
    fprintf(stderr, "[wmt-remote] ml762 REMOTE MODE via %s:%d -- no local Metal objects "
                    "will be created in this process\n", host, RM_PORT);
    /* The per-name lines are the primary signal and are emitted as each call
     * is first seen; this summary is only a convenience for a clean exit. A
     * jetsam kill runs no atexit handler, which is exactly why discovery does
     * not depend on it. */
    atexit(wmtr_report_unrouted);
    return;
}

static int wmtr_enabled(void) {
    pthread_once(&wmtr_once, wmtr_init);
    return wmtr_mode;
}

/* Bytes occupied by one function-constant value.
 *
 * Metal reads the value through a pointer whose length it infers from the type,
 * so the guest must send exactly that many bytes. An unknown type returns 0 and
 * the caller reports it BY NAME rather than guessing a width -- a wrong length
 * would either truncate the constant or copy adjacent memory into the shader.
 * Vector rows follow Metal's rule that a 3-component vector occupies 4. */
static uint32_t wmt_const_size(unsigned type) {
    switch (type) {
    case 3:  return 4;   case 4:  return 8;   case 5:  return 16;  case 6:  return 16;  /* float,2,3,4 */
    case 7:  return 16;  case 8:  return 32;  case 9:  return 32;                       /* float2xN */
    case 10: return 24;  case 11: return 48;  case 12: return 48;                       /* float3xN */
    case 13: return 32;  case 14: return 64;  case 15: return 64;                       /* float4xN */
    case 16: return 2;   case 17: return 4;   case 18: return 8;   case 19: return 8;   /* half,2,3,4 */
    case 20: return 8;   case 21: return 16;  case 22: return 16;
    case 23: return 12;  case 24: return 24;  case 25: return 24;
    case 26: return 16;  case 27: return 32;  case 28: return 32;
    case 29: case 33: return 4;                                                          /* int, uint */
    case 30: case 34: return 8;
    case 31: case 32: case 35: case 36: return 16;                                       /* int3/4, uint3/4 */
    case 37: case 41: return 2;                                                          /* short, ushort */
    case 38: case 42: return 4;
    case 39: case 40: case 43: case 44: return 8;
    case 45: case 49: case 53: return 1;                                                 /* char, uchar, bool */
    case 46: case 50: case 54: return 2;
    case 47: case 48: case 51: case 52: case 55: case 56: return 4;
    default: return 0;   /* None, Struct, Array, or anything new: refuse to guess */
    }
}

/* The host's default device, fetched once. Some paths need a device without
 * having one to hand -- notably the pixel-format translator, which asks whether
 * BC is supported. In remote mode that question is about the HOST gpu; asking a
 * local MTLDevice there gives the VM's answer (no BC) and remaps compressed
 * formats that the host could have sampled directly. */
static uint64_t wmtr_dev_cached;
static pthread_once_t wmtr_dev_once = PTHREAD_ONCE_INIT;

static void wmtr_dev_init(void) {
    struct rm_ret_handle devs;
    if (wmtr_call(RM_OP_COPY_ALL_DEVICES, 0, 0, &devs, sizeof devs, 0) != RM_OK) return;
    struct rm_arg_handle_u64 a = { devs.handle, 0 };
    struct rm_ret_handle d;
    if (wmtr_call(RM_OP_ARRAY_OBJECT, &a, sizeof a, &d, sizeof d, 0) == RM_OK)
        wmtr_dev_cached = d.handle;
}

static uint64_t wmtr_host_device(void) {
    if (!wmtr_enabled()) return 0;
    pthread_once(&wmtr_dev_once, wmtr_dev_init);
    return wmtr_dev_cached;
}

/* Does the HOST support BC? Cached; -1 until asked. */
static int wmtr_host_bc(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint64_t dev = wmtr_host_device();
    if (!dev) return 0;
    struct rm_arg_handle a = { dev };
    struct rm_ret_u64 r;
    cached = (wmtr_call(RM_OP_SUPPORTS_BC, &a, sizeof a, &r, sizeof r, 0) == RM_OK && r.value) ? 1 : 0;
    fprintf(stderr, "[wmt-remote] host BC support = %d (formats will NOT be remapped)\n", cached);
    return cached;
}

/* ---- buffer shadow registry ----------------------------------------------
 *
 * MTLBuffer.contents hands the guest a raw pointer it then writes through with
 * no further calls. That pointer cannot be a host address, so CPU-visible
 * buffers get guest shadow memory and the host copy is refreshed from it.
 *
 * The flush is deliberately pessimistic: everything live is uploaded before a
 * commit. updateContents announces SOME writes, but nothing guarantees it
 * announces all of them, and a missed write shows up as a wrong-looking frame
 * rather than an error. Narrowing to dirty ranges is an optimisation to make
 * once frames are correct, not before. */
struct wmtr_buf {
    uint64_t handle;      /* tagged remote handle -- the key   */
    void    *shadow;      /* guest memory the app writes into  */
    uint64_t length;
    uint32_t options;
    int      cpu_visible;
    int      owned;       /* did WE allocate the shadow?       */
};
#define WMTR_BUFS_MAX 4096
static struct wmtr_buf wmtr_bufs[WMTR_BUFS_MAX];
static unsigned wmtr_bufs_n;
static pthread_mutex_t wmtr_bufs_lock = PTHREAD_MUTEX_INITIALIZER;

static void wmtr_buf_add(uint64_t handle, void *shadow, uint64_t len, uint32_t opts,
                         int cpu, int owned) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    if (wmtr_bufs_n < WMTR_BUFS_MAX) {
        wmtr_bufs[wmtr_bufs_n++] = (struct wmtr_buf){ handle, shadow, len, opts, cpu, owned };
    } else {
        /* Never let a cap read as success: a buffer missing from the registry
         * is never uploaded, so its contents silently never reach the host. */
        static int warned;
        if (!warned) { warned = 1;
            fprintf(stderr, "[wmt-remote] buffer registry full at %d -- further buffers "
                            "will NOT be uploaded\n", WMTR_BUFS_MAX); }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
}

/* Drop a buffer when its handle is released. Leaving it registered means the
 * flush keeps uploading through a pointer whose owner has gone, and keeps a
 * stale handle alive on the wire. Only memory WE allocated is freed. */
static void wmtr_buf_remove(uint64_t handle) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    for (unsigned i = 0; i < wmtr_bufs_n; i++) {
        if (wmtr_bufs[i].handle != handle) continue;
        if (wmtr_bufs[i].owned) free(wmtr_bufs[i].shadow);
        wmtr_bufs[i] = wmtr_bufs[--wmtr_bufs_n];
        break;
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
}

static struct wmtr_buf *wmtr_buf_find(uint64_t handle) {
    for (unsigned i = 0; i < wmtr_bufs_n; i++)
        if (wmtr_bufs[i].handle == handle) return &wmtr_bufs[i];
    return NULL;
}

/* Upload one range. Chunked: a single message is capped, and AAA buffers are
 * far larger than that cap. */
static int wmtr_buf_upload(uint64_t handle, const void *src, uint64_t off, uint64_t len) {
    const uint8_t *p = src;
    while (len) {
        uint64_t n = len > (RM_CHUNK_BYTES - sizeof(struct rm_buffer_range))
                   ? (RM_CHUNK_BYTES - sizeof(struct rm_buffer_range)) : len;
        uint8_t *msg = malloc(sizeof(struct rm_buffer_range) + n);
        if (!msg) return -1;
        struct rm_buffer_range *r = (void *)msg;
        r->handle = handle; r->offset = off; r->length = n;
        memcpy(msg + sizeof *r, p, n);
        uint32_t st = wmtr_call(RM_OP_BUFFER_UPLOAD, msg, (uint32_t)(sizeof *r + n), 0, 0, 0);
        free(msg);
        if (st != RM_OK) return -1;
        p += n; off += n; len -= n;
    }
    return 0;
}

/* Push every CPU-visible shadow to the host. Called before submission. */
static void wmtr_flush_buffers(void) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    unsigned n = wmtr_bufs_n, failed = 0;
    for (unsigned i = 0; i < n; i++) {
        struct wmtr_buf *b = &wmtr_bufs[i];
        if (!b->cpu_visible || !b->shadow || !b->length) continue;
        if (wmtr_buf_upload(b->handle, b->shadow, 0, b->length) != 0) {
            failed++;
            /* Name the buffer, not just the count: a vertex buffer that never
             * reaches the host draws nothing while the clear still lands, which
             * looks like a working frame with missing geometry. */
            static unsigned told;
            if (told++ < 8)
                fprintf(stderr, "[wmt-remote] upload FAILED for buffer 0x%llx "
                                "(%llu bytes, options 0x%x)\n",
                        (unsigned long long)b->handle, (unsigned long long)b->length,
                        b->options);
        }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    if (failed) {
        static unsigned reported;
        if (reported++ < 4)
            fprintf(stderr, "[wmt-remote] %u buffer upload(s) failed during flush\n", failed);
    }
}

/* Drawable -> texture pairing.
 *
 * NEXT_DRAWABLE returns both handles in one reply, so the texture is already
 * known by the time the guest asks for it. Recording the pair here turns
 * MetalDrawable_texture into a lookup instead of a round trip -- it is called
 * once per frame, and the answer cannot change for a given drawable. */
#define WMTR_DRAWABLES 8
static struct { uint64_t drawable, texture; } wmtr_pairs[WMTR_DRAWABLES];
static unsigned wmtr_pair_next;

static void wmtr_pair_record(uint64_t d, uint64_t t) {
    wmtr_pairs[wmtr_pair_next % WMTR_DRAWABLES].drawable = d;
    wmtr_pairs[wmtr_pair_next % WMTR_DRAWABLES].texture  = t;
    wmtr_pair_next++;
}

static uint64_t wmtr_pair_texture(uint64_t d) {
    for (unsigned i = 0; i < WMTR_DRAWABLES; i++)
        if (wmtr_pairs[i].drawable == d) return wmtr_pairs[i].texture;
    return 0;
}

/* Texture dimensions. width and height are asked separately, once per frame
 * each, but arrive together and cannot change for a given texture -- so one
 * round trip serves both. */
static struct { uint64_t tex, w, h; } wmtr_dims[8];
static unsigned wmtr_dims_next;

static uint64_t wmtr_tex_dim(uint64_t tex, char which) {
    for (unsigned i = 0; i < 8; i++)
        if (wmtr_dims[i].tex == tex)
            return which == 'w' ? wmtr_dims[i].w : wmtr_dims[i].h;
    struct rm_arg_handle a = { tex };
    struct rm_ret_handle_u64 r;
    if (wmtr_call(RM_OP_TEXTURE_DIMS, &a, sizeof a, &r, sizeof r, 0) != RM_OK) return 0;
    unsigned slot = wmtr_dims_next++ % 8;
    wmtr_dims[slot].tex = tex; wmtr_dims[slot].w = r.handle; wmtr_dims[slot].h = r.value;
    return which == 'w' ? r.handle : r.value;
}

/* A routed call that has no remote implementation yet must say so with its own
 * name. "remote call failed" one machine from the GPU is close to undebuggable.
 *
 * Names are reported ONCE each and counted, so a run answers "which calls does
 * this title actually need" in one pass. That is how every surface here was
 * sized: the command census found 15 of 38 opcodes and the API census 43 of
 * 127 entries -- both far below the speculative estimate. */
#define WMTR_SEEN_MAX 128
static const char *wmtr_seen[WMTR_SEEN_MAX];
static unsigned wmtr_seen_n;
static unsigned long wmtr_unimpl_hits;
static unsigned wmtr_seen_dropped;
static pthread_mutex_t wmtr_seen_lock = PTHREAD_MUTEX_INITIALIZER;

static inline NTSTATUS wmtr_unimplemented(const char *fn) {
    pthread_mutex_lock(&wmtr_seen_lock);
    wmtr_unimpl_hits++;
    unsigned i;
    for (i = 0; i < wmtr_seen_n; i++)
        if (strcmp(wmtr_seen[i], fn) == 0) break;
    if (i == wmtr_seen_n) {
        if (wmtr_seen_n < WMTR_SEEN_MAX) {
            wmtr_seen[wmtr_seen_n++] = fn;
            fprintf(stderr, "[wmt-remote] unrouted: %s\n", fn);
        } else {
            /* Never let a cap read as coverage. */
            wmtr_seen_dropped++;
        }
    }
    pthread_mutex_unlock(&wmtr_seen_lock);
    return STATUS_NOT_IMPLEMENTED;
}

/* Call from a partially routed handler for the paths it does not cover. */
#define WMTR_UNIMPLEMENTED(fn) ((void)wmtr_unimplemented(fn))

static void wmtr_report_unrouted(void) {
    pthread_mutex_lock(&wmtr_seen_lock);
    fprintf(stderr, "[wmt-remote] unrouted summary: %lu calls across %u distinct entries",
            wmtr_unimpl_hits, wmtr_seen_n);
    if (wmtr_seen_dropped)
        fprintf(stderr, " (+%u distinct names DROPPED, cap %d)", wmtr_seen_dropped, WMTR_SEEN_MAX);
    fprintf(stderr, "\n");
    for (unsigned i = 0; i < wmtr_seen_n; i++)
        fprintf(stderr, "[wmt-remote]   %s\n", wmtr_seen[i]);
    pthread_mutex_unlock(&wmtr_seen_lock);
}

#endif
