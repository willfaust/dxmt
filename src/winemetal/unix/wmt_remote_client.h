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
#include <sys/mman.h>
#include <mach/vm_statistics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>
#include "../../../../research/remote-metal/protocol.h"

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
/* ml819 RPC census (guarded by wmtr_lock) */
static int wmtr_batch_on;   /* ml821 */
static unsigned long wmtr_ub_msgs, wmtr_ub_ranges, wmtr_rel_msgs, wmtr_rel_handles;
static unsigned long wmtr_op_n[256], wmtr_call_n;
static double wmtr_op_ms[256], wmtr_call_ms;
static uint32_t wmtr_last_upload_status;
static uint32_t wmtr_call(uint16_t op, const void *arg, uint32_t alen,
                          void *out, uint32_t ocap, uint32_t *olen) {
    if (out && ocap) memset(out, 0, ocap);
    if (olen) *olen = 0;
    pthread_mutex_lock(&wmtr_lock);
    /* Every call is serialised, so ONE slow reply stalls the whole process.
     * A silent stall is indistinguishable from a hang, so time the call and
     * name the opcode when it is pathological. */
    struct timeval t_begin; gettimeofday(&t_begin, 0);
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
    {
        struct timeval t_end; gettimeofday(&t_end, 0);
        double dt = (t_end.tv_sec - t_begin.tv_sec) * 1000.0
                  + (t_end.tv_usec - t_begin.tv_usec) / 1000.0;
        if (dt > 250.0) {
            static unsigned told;
            if (told++ < 16)
                fprintf(stderr, "[wmt-remote] SLOW call: opcode %u took %.0f ms -- every other "
                                "winemetal call waited on it\n", op, dt);
        }
    }
    /* ml819: per-opcode census. Every call is serialised and waits for its
     * reply, so the frame rate ceiling is (calls per frame) x (round trip).
     * Nothing measured that until now; 2-4 fps in-game had no attribution. */
    {
        struct timeval t_now; gettimeofday(&t_now, 0);
        double ms = (t_now.tv_sec - t_begin.tv_sec) * 1000.0
                  + (t_now.tv_usec - t_begin.tv_usec) / 1000.0;
        wmtr_op_n[op & 255]++; wmtr_op_ms[op & 255] += ms;
        wmtr_call_n++; wmtr_call_ms += ms;
    }
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
    /* ml821: coalescing is OPT-IN. With it off this build behaves exactly as
     * ml820 did, which is what makes a matched baseline measurement possible. */
    { const char *b = getenv("DXMT_REMOTE_BATCH"); wmtr_batch_on = b && *b == '1'; }
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
    {
        /* ml1022: DISTINGUISH A REJECTED TOKEN FROM A TIMEOUT, AND SAY SO LOUDLY.
         *
         * This printed "authentication rejected -- staying local" for ANY
         * non-RM_OK ping, including a 30-second transport timeout, and then
         * silently rendered locally for the whole session. That cost a run: the
         * guest quietly used the local paravirtual device (which reports NO
         * Metal family at all, so Apple's driver later asserted) while the stale
         * host log still showed the PREVIOUS session's frames -- and the two
         * together read exactly like a successful remote run.
         *
         * The daemon serves ONE connection at a time, so a previous stuck
         * session lets a second TCP connect succeed in the backlog and then
         * never answer. That is a transport timeout, not a bad token, and the
         * two need different responses from whoever reads the log.
         *
         * Falling back is still the behaviour (a hard failure here would need a
         * failed-backend state threaded through device creation, which this
         * header cannot do), but it is now IMPOSSIBLE to miss. */
        int rc = (int)wmtr_call(RM_OP_PING, tok, (uint32_t)strlen(tok), 0, 0, 0);
        if (rc != RM_OK) {
            /* The daemon rejects a bad token by CLOSING WITHOUT REPLYING
             * (authenticate() returns 0 and never calls reply), so from here a
             * wrong token and a daemon too busy to answer look identical. Say
             * that, rather than asserting "authentication rejected" as the old
             * message did -- it sent me looking for a token mismatch when the
             * real cause was a 30-second timeout against an occupied daemon. */
            const char *why = "no reply -- either the token is wrong OR the daemon is busy/stuck "
                              "(it serves ONE connection at a time)";
            fprintf(stderr,
                "[wmt-remote] ml1022 ================ REMOTE METAL NOT AVAILABLE ================\n"
                "[wmt-remote] ml1022 %s:%d -- %s (ping rc=%d)\n"
                "[wmt-remote] ml1022 FALLING BACK TO LOCAL METAL. Remote was explicitly requested,\n"
                "[wmt-remote] ml1022 so this run is NOT a valid remote-mode test: the local device\n"
                "[wmt-remote] ml1022 reports no Metal family and behaves differently. Any host log\n"
                "[wmt-remote] ml1022 you read alongside it belongs to a PREVIOUS session.\n"
                "[wmt-remote] ml1022 ============================================================\n",
                host, RM_PORT, why, rc);
            close(wmtr_fd); wmtr_fd = -1; wmtr_mode = 0; return;
        }
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
struct wmtr_heap {
    uint64_t handle, size;
    unsigned refs, buffers;
    uint8_t *guest, *baseline;
    int guest_owned;
    struct wmtr_heap *next;
};
struct wmtr_buf {
    uint64_t handle;      /* tagged remote handle -- the key   */
    void    *shadow;      /* guest memory the app writes into  */
    uint64_t length;
    uint32_t options;
    int      cpu_visible;
    int      owned;       /* did WE allocate the shadow?       */
    uint64_t *page_sum;   /* last-uploaded checksum per 64K page */
    uint32_t  pages;
    uint32_t  refs;       /* ml820: mirrors the guest's retain/release count */
    uint32_t  rb_pending; /* ml821: a GPU readback for this buffer is outstanding */
    struct wmtr_heap *heap;
    uint64_t heap_offset;
};

/* Upload only what CHANGED.
 *
 * The flush was pessimistic by design: every live CPU-visible buffer, in full,
 * before every commit. That is correct and was the right thing to get a frame
 * on screen, but it sends the same unchanged megabytes every frame. Hashing a
 * 64K page costs a linear read at memory bandwidth; sending it costs a network
 * round trip, so comparing first is far cheaper than uploading blindly.
 *
 * Page-granular rather than whole-buffer: a ring allocator touches a small
 * moving window of a large buffer, so whole-buffer comparison would resend
 * everything for a few changed bytes. */
#define WMTR_PAGE 65536u

static uint64_t wmtr_page_sum(const uint8_t *p, size_t n) {
    /* Every byte is covered -- a missed change shows as a stale frame, not an
     * error, so this must not sample. But it runs over every live buffer every
     * frame, so the PER-BYTE COST IS THE FRAME BUDGET: a byte-at-a-time FNV is
     * a serial multiply chain at a couple of GB/s, which cost ~190ms a frame
     * here. Four independent lanes over 64-bit words keeps the same coverage
     * while letting the CPU issue the multiplies in parallel. */
    uint64_t a = 0x9e3779b97f4a7c15ull, b = 0xc2b2ae3d27d4eb4full;
    uint64_t c = 0x165667b19e3779f9ull, d = 0x27d4eb2f165667c5ull;
    size_t i = 0;
    while (i + 32 <= n) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i,      8); memcpy(&w1, p + i + 8,  8);
        memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        a = (a ^ w0) * 0x100000001b3ull;
        b = (b ^ w1) * 0x100000001b3ull;
        c = (c ^ w2) * 0x100000001b3ull;
        d = (d ^ w3) * 0x100000001b3ull;
        i += 32;
    }
    uint64_t tail = 0;
    for (; i < n; i++) tail = (tail ^ p[i]) * 0x100000001b3ull;
    uint64_t hsum = a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ tail ^ (uint64_t)n;
    return hsum ? hsum : 1;   /* 0 means "never hashed" */
}

/* Bytes considered vs bytes actually sent, so the saving is measured. */
static unsigned long long wmtr_flush_seen, wmtr_flush_sent;
static unsigned long wmtr_flush_n;
static unsigned long wmtr_batches_packed, wmtr_batches_dropped;   /* ml817 */
static double wmtr_flush_ms, wmtr_flush_rpc_ms; static unsigned long wmtr_flush_rpcs;   /* ml819 */
/* Chunked, stably addressed, and LOCKED.
 *
 * The previous table was a fixed 4096 entries: a large title exceeded it, every
 * later buffer went unregistered, and 75,525 content updates had nowhere to go
 * -- geometry with no data. It also handed out raw entry pointers from an
 * UNLOCKED lookup, so an entry could be rewritten while a caller held it.
 *
 * Chunks rather than a growing array: entries must not move. A realloc would
 * invalidate every pointer already handed out, which is the same class of bug
 * as the one this replaces, only harder to see. Chunks are never freed while
 * the process runs, so an entry's address is stable for its lifetime. */
#define WMTR_BUF_CHUNK 1024

struct wmtr_buf_chunk {
    struct wmtr_buf entries[WMTR_BUF_CHUNK];
    unsigned used;
    struct wmtr_buf_chunk *next;
};
static struct wmtr_buf_chunk *wmtr_chunks;
static unsigned long wmtr_bufs_live;
static pthread_mutex_t wmtr_bufs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wmtr_heap *wmtr_heaps;

/* All heap records and their guest pointers are protected by wmtr_bufs_lock. */
static void wmtr_heap_dispose_locked(struct wmtr_heap *heap) {
    if (heap->refs || heap->buffers) return;
    struct wmtr_heap **link = &wmtr_heaps;
    while (*link && *link != heap) link = &(*link)->next;
    if (*link) *link = heap->next;
    if (heap->baseline) munmap(heap->baseline, heap->size);
    if (heap->guest_owned) munmap(heap->guest, heap->size);
    free(heap);
}
static int wmtr_heap_register(uint64_t handle, uint64_t size) {
    if (!size || size > SIZE_MAX) return -1;
    struct wmtr_heap *heap = calloc(1, sizeof(*heap));
    if (!heap) return -1;
    heap->baseline = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (heap->baseline == MAP_FAILED) { free(heap); return -1; }
    heap->handle = handle; heap->size = size; heap->refs = 1;
    pthread_mutex_lock(&wmtr_bufs_lock);
    heap->next = wmtr_heaps; wmtr_heaps = heap;
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return 0;
}
static void wmtr_heap_reference(uint64_t handle, int retain) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_heap *heap = wmtr_heaps;
    while (heap && heap->handle != handle) heap = heap->next;
    if (heap) {
        if (retain) heap->refs++;
        else if (heap->refs) { heap->refs--; wmtr_heap_dispose_locked(heap); }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
}
static void *wmtr_heap_native_pointer(uint64_t handle, uint64_t offset, uint64_t length) {
    void *result = NULL;
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_heap *heap = wmtr_heaps;
    while (heap && heap->handle != handle) heap = heap->next;
    if (heap && heap->refs && length && offset <= heap->size && length <= heap->size - offset) {
        if (!heap->guest) {
            void *memory = mmap(NULL, (size_t)heap->size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (memory != MAP_FAILED) { heap->guest = memory; heap->guest_owned = 1; }
        }
        if (heap->guest) result = heap->guest + (size_t)offset;
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return result;
}

/* Caller must hold wmtr_bufs_lock. */
static struct wmtr_buf *wmtr_buf_find_locked(uint64_t handle) {
    for (struct wmtr_buf_chunk *c = wmtr_chunks; c; c = c->next)
        for (unsigned i = 0; i < c->used; i++)
            if (c->entries[i].handle == handle) return &c->entries[i];
    return NULL;
}

static void wmtr_buf_add(uint64_t handle, void *shadow, uint64_t len, uint32_t opts,
                         int cpu, int owned) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *slot = wmtr_buf_find_locked(handle);
    if (!slot) {
        /* Reuse a retired entry before growing. */
        for (struct wmtr_buf_chunk *c = wmtr_chunks; c && !slot; c = c->next)
            for (unsigned i = 0; i < c->used; i++)
                if (!c->entries[i].handle) { slot = &c->entries[i]; break; }
    }
    if (!slot) {
        struct wmtr_buf_chunk *c = wmtr_chunks;
        if (!c || c->used == WMTR_BUF_CHUNK) {
            struct wmtr_buf_chunk *n = calloc(1, sizeof *n);
            if (!n) {
                static int warned;
                if (!warned) { warned = 1;
                    fprintf(stderr, "[wmt-remote] out of memory growing the buffer registry -- "
                                    "further buffers will NOT be uploaded\n"); }
                pthread_mutex_unlock(&wmtr_bufs_lock);
                return;
            }
            n->next = wmtr_chunks; wmtr_chunks = n; c = n;
        }
        slot = &c->entries[c->used++];
    }
    *slot = (struct wmtr_buf){ handle, shadow, len, opts, cpu, owned, NULL, 0, 1 };
    wmtr_bufs_live++;
    pthread_mutex_unlock(&wmtr_bufs_lock);
}

/* Retire a buffer. Its shadow may be memory the CALLER owns and is about to
 * free, so this must happen before that free -- otherwise the flush hashes a
 * dead allocation, which faulted mid-frame on an unmapped address. */
static void wmtr_buf_retain(uint64_t handle) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *b = wmtr_buf_find_locked(handle);
    if (b) b->refs++;
    pthread_mutex_unlock(&wmtr_bufs_lock);
}

static void wmtr_buf_remove(uint64_t handle) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *b = wmtr_buf_find_locked(handle);
    /* ml820: ANY release used to retire the entry. DXMT copies owning
     * references freely (the encoder keeps one per bound buffer), so the
     * first of several releases dropped the shadow while the host object -- and
     * the app's writes through the mapped pointer -- lived on. Mirror the count
     * and retire only when the guest's last reference goes. */
    if (b && b->refs > 1) { b->refs--; b = NULL; }
    if (b) {
        struct wmtr_heap *heap = b->heap;
        /* ml803: owned==1 came from calloc, owned==2 from mmap. The mmap
         * fallback exists because a 32MB calloc failed with ENOMEM on the
         * research VM while the process held only ~3GB -- libmalloc's zone
         * could not place it, though the address space had room. Freeing an
         * mmap'd shadow with free() would corrupt the heap. */
        if (b->owned == 1) free(b->shadow);
        else if (b->owned == 2 && b->shadow) munmap(b->shadow, b->length);
        free(b->page_sum);
        b->handle = 0; b->shadow = NULL; b->page_sum = NULL;
        b->length = 0; b->cpu_visible = 0; b->owned = 0; b->pages = 0;
        b->heap = NULL; b->heap_offset = 0;
        if (heap) { heap->buffers--; wmtr_heap_dispose_locked(heap); }
        if (wmtr_bufs_live) wmtr_bufs_live--;
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
}

static int wmtr_buf_lookup(uint64_t handle, struct wmtr_buf *out);

/* ---- ml820: host-object lifetime mirroring ------------------------------
 *
 * Metal returns command buffers and encoders AUTORELEASED. The host interns
 * every returned object with one owning reference and nothing ever dropped
 * it: DXMT ends encoders without releasing (borrowed objects) and the guest's
 * NSAutoreleasePool drains only local objects. One session retired 57,604
 * live handles at disconnect, and every intern searched that table linearly.
 *
 * Mirror the pool: handles the host hands back for autoreleased objects are
 * pushed onto a per-thread list; the guest's pool release drains everything
 * pushed since that pool was created. A retained copy (attached_cmdbuf) keeps
 * the host object alive exactly as it would natively. */
static __thread uint64_t *wmtr_pool_items;
static __thread unsigned  wmtr_pool_n, wmtr_pool_cap;
static __thread unsigned  wmtr_pool_marks[32];
static __thread unsigned  wmtr_pool_depth;
static unsigned long wmtr_pool_released;

static void wmtr_pool_begin(void) {
    if (wmtr_pool_depth < 32) wmtr_pool_marks[wmtr_pool_depth] = wmtr_pool_n;
    wmtr_pool_depth++;
}
static void wmtr_pool_push(uint64_t handle) {
    if (!handle) return;
    if (wmtr_pool_n == wmtr_pool_cap) {
        unsigned ncap = wmtr_pool_cap ? wmtr_pool_cap * 2 : 256;
        uint64_t *ni = realloc(wmtr_pool_items, ncap * sizeof *ni);
        if (!ni) return;                 /* leak this one rather than crash */
        wmtr_pool_items = ni; wmtr_pool_cap = ncap;
    }
    wmtr_pool_items[wmtr_pool_n++] = handle;
}
static void wmtr_pool_drain(void) {
    if (!wmtr_pool_depth) return;
    wmtr_pool_depth--;
    unsigned from = wmtr_pool_depth < 32 ? wmtr_pool_marks[wmtr_pool_depth] : 0;
    if (from > wmtr_pool_n) from = 0;
    if (wmtr_batch_on) {
        /* ml821: one round trip per pool instead of one per object. The local
         * registry retirement still runs per handle -- only the RPC coalesces. */
        uint8_t msg[sizeof(struct rm_release_multi) + RM_MULTI_MAX_HANDLES * sizeof(uint64_t)];
        struct rm_release_multi *m = (void *)msg;
        uint64_t *hs = (uint64_t *)(msg + sizeof *m);
        unsigned k = 0;
        for (unsigned i = from; i < wmtr_pool_n; i++) {
            wmtr_buf_remove(wmtr_pool_items[i]);
            hs[k++] = wmtr_pool_items[i];
            wmtr_pool_released++;
            if (k == RM_MULTI_MAX_HANDLES) {
                m->count = k; m->reserved = 0;
                wmtr_call(RM_OP_RELEASE_MULTI, msg, (uint32_t)(sizeof *m + k * sizeof *hs), 0, 0, 0);
                wmtr_rel_msgs++; wmtr_rel_handles += k; k = 0;
            }
        }
        if (k) {
            m->count = k; m->reserved = 0;
            wmtr_call(RM_OP_RELEASE_MULTI, msg, (uint32_t)(sizeof *m + k * sizeof *hs), 0, 0, 0);
            wmtr_rel_msgs++; wmtr_rel_handles += k;
        }
    } else
    for (unsigned i = from; i < wmtr_pool_n; i++) {
        struct rm_arg_handle a = { wmtr_pool_items[i] };
        wmtr_buf_remove(a.handle);
        wmtr_call(RM_OP_RELEASE, &a, sizeof a, 0, 0, 0);
        wmtr_pool_released++;
    }
    wmtr_pool_n = from;
}

/* ---- ml820: GPU -> guest readback ------------------------------------------
 *
 * Nothing ever copied GPU-written bytes back into a guest shadow. Occlusion
 * query results are read by DXMT from the visibility buffer's guest memory
 * after completion; remotely that memory was never touched, so every query
 * answered zero -- "occluded" -- and the geometry behind it was culled. The
 * same applies to staging buffers filled by blit copies.
 *
 * Record, per command buffer, the CPU-visible buffers the GPU will write
 * (visibility result buffer, blit destinations). When the guest observes
 * completion -- an explicit wait, or a status poll answering Completed --
 * download them into their shadows and reset the upload baseline so the
 * bytes are not immediately pushed back as CPU edits. */
#define WMTR_RB_SLOTS 64
#define WMTR_RB_BUFS  32
struct wmtr_rb { uint64_t cmdbuf; unsigned n; uint64_t bufs[WMTR_RB_BUFS]; };
static struct wmtr_rb wmtr_rb_tab[WMTR_RB_SLOTS];
static struct { uint64_t enc, cmdbuf; } wmtr_enc_map[256];
static unsigned wmtr_enc_pos;
static pthread_mutex_t wmtr_rb_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long wmtr_rb_downloads, wmtr_rb_bytes, wmtr_rb_failed, wmtr_rb_dropped;

static void wmtr_enc_note(uint64_t enc, uint64_t cmdbuf) {
    if (!enc) return;
    pthread_mutex_lock(&wmtr_rb_lock);
    wmtr_enc_map[wmtr_enc_pos & 255].enc = enc;
    wmtr_enc_map[wmtr_enc_pos & 255].cmdbuf = cmdbuf;
    wmtr_enc_pos++;
    pthread_mutex_unlock(&wmtr_rb_lock);
}
static uint64_t wmtr_enc_cmdbuf(uint64_t enc) {
    uint64_t r = 0;
    pthread_mutex_lock(&wmtr_rb_lock);
    for (unsigned i = 0; i < 256; i++)
        if (wmtr_enc_map[i].enc == enc) { r = wmtr_enc_map[i].cmdbuf; break; }
    pthread_mutex_unlock(&wmtr_rb_lock);
    return r;
}
/* Only CPU-visible registered buffers matter; anything else is skipped. */
static void wmtr_rb_add(uint64_t cmdbuf, uint64_t buf) {
    if (!cmdbuf || !buf) return;
    struct wmtr_buf b;
    if (!wmtr_buf_lookup(buf, &b) || !b.cpu_visible || !b.shadow || b.heap) return;
    pthread_mutex_lock(&wmtr_rb_lock);
    struct wmtr_rb *e = NULL, *free_e = NULL;
    for (unsigned i = 0; i < WMTR_RB_SLOTS; i++) {
        if (wmtr_rb_tab[i].cmdbuf == cmdbuf) { e = &wmtr_rb_tab[i]; break; }
        if (!wmtr_rb_tab[i].cmdbuf && !free_e) free_e = &wmtr_rb_tab[i];
    }
    if (!e && free_e) { e = free_e; e->cmdbuf = cmdbuf; e->n = 0; }
    if (e) {
        unsigned i;
        for (i = 0; i < e->n; i++) if (e->bufs[i] == buf) break;
        if (i == e->n) {
            if (e->n < WMTR_RB_BUFS) {
                e->bufs[e->n++] = buf;
                pthread_mutex_lock(&wmtr_bufs_lock);   /* ml821 */
                struct wmtr_buf *rb = wmtr_buf_find_locked(buf);
                if (rb) rb->rb_pending++;
                pthread_mutex_unlock(&wmtr_bufs_lock);
            } else wmtr_rb_dropped++;
        }
    } else wmtr_rb_dropped++;
    pthread_mutex_unlock(&wmtr_rb_lock);
}
static int wmtr_buf_download(uint64_t handle) {
    struct wmtr_buf b;
    if (!wmtr_buf_lookup(handle, &b) || !b.shadow || !b.length) return -1;
    uint64_t off = 0;
    while (off < b.length) {
        uint64_t n = b.length - off; if (n > RM_CHUNK_BYTES) n = RM_CHUNK_BYTES;
        struct rm_buffer_range r = { handle, off, n };
        uint32_t got = 0;
        uint32_t st = wmtr_call(RM_OP_BUFFER_READ, &r, sizeof r, (uint8_t *)b.shadow + off,
                                (uint32_t)n, &got);
        if (st != RM_OK || got != n) {
            wmtr_rb_failed++;
            static unsigned told;
            if (told++ < 8)
                fprintf(stderr, "[wmt-remote] ml820 readback FAILED buffer 0x%llx off=%llu len=%llu "
                                "status=%u got=%u\n", (unsigned long long)handle,
                        (unsigned long long)off, (unsigned long long)n, st, got);
            return -1;
        }
        off += n;
    }
    /* The bytes now in the shadow are the host's; rebase the upload checksums
     * so the next flush does not push them straight back. */
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *e = wmtr_buf_find_locked(handle);
    if (e && e->shadow == b.shadow) {
        if (!e->page_sum) {
            e->pages = (uint32_t)((e->length + WMTR_PAGE - 1) / WMTR_PAGE);
            e->page_sum = calloc(e->pages, sizeof *e->page_sum);
        }
        if (e->page_sum)
            for (uint32_t p = 0; p < e->pages; p++) {
                size_t o = (size_t)p * WMTR_PAGE;
                size_t l = e->length - o < WMTR_PAGE ? (size_t)(e->length - o) : WMTR_PAGE;
                e->page_sum[p] = wmtr_page_sum((const uint8_t *)e->shadow + o, l);
            }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    wmtr_rb_downloads++; wmtr_rb_bytes += b.length;
    return 0;
}
static void wmtr_rb_drain(uint64_t cmdbuf) {
    if (!cmdbuf) return;
    struct wmtr_rb local; local.n = 0;
    pthread_mutex_lock(&wmtr_rb_lock);
    for (unsigned i = 0; i < WMTR_RB_SLOTS; i++)
        if (wmtr_rb_tab[i].cmdbuf == cmdbuf) { local = wmtr_rb_tab[i]; wmtr_rb_tab[i].cmdbuf = 0; wmtr_rb_tab[i].n = 0; break; }
    pthread_mutex_unlock(&wmtr_rb_lock);
    for (unsigned i = 0; i < local.n; i++) {
        wmtr_buf_download(local.bufs[i]);
        pthread_mutex_lock(&wmtr_bufs_lock);           /* ml821 */
        struct wmtr_buf *rb = wmtr_buf_find_locked(local.bufs[i]);
        if (rb && rb->rb_pending) rb->rb_pending--;
        pthread_mutex_unlock(&wmtr_bufs_lock);
    }
    static unsigned told;
    if (local.n && told++ < 8)
        fprintf(stderr, "[wmt-remote] ml820 readback: %u buffer(s) for cmdbuf 0x%llx "
                        "(total %lu downloads, %lu MB, %lu failed, %lu dropped)\n", local.n,
                (unsigned long long)cmdbuf, wmtr_rb_downloads, wmtr_rb_bytes >> 20,
                wmtr_rb_failed, wmtr_rb_dropped);
}

/* Copy out what the caller needs instead of leaking a pointer into the table. */
static int wmtr_buf_lookup(uint64_t handle, struct wmtr_buf *out) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *b = wmtr_buf_find_locked(handle);
    if (b) *out = *b;
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return b != NULL;
}

/* Upload one range. Chunked: a single message is capped, and AAA buffers are
 * far larger than that cap. */
/* ml820: one reusable per-thread staging area instead of a malloc per chunk.
 * The 8MB temporary failed under the same range pressure the shadows hit,
 * and that failure was reported as status=0 -- indistinguishable from a
 * host acknowledgement. Allocated once (with the tagged fallback), never freed. */
static uint8_t *wmtr_upload_scratch(void) {
    static __thread uint8_t *scratch;
    if (scratch) return scratch;
    scratch = malloc(RM_CHUNK_BYTES);
    if (!scratch) {
        void *m = mmap(NULL, RM_CHUNK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                       VM_MAKE_TAG(VM_MEMORY_MALLOC), 0);
        if (m != MAP_FAILED) scratch = m;
    }
    if (!scratch) {
        static unsigned told;
        if (told++ < 4)
            fprintf(stderr, "[wmt-remote] ml820 upload scratch allocation FAILED (errno %d) -- "
                            "this thread cannot upload\n", errno);
    }
    return scratch;
}

static int wmtr_buf_upload(uint64_t handle, const void *src, uint64_t off, uint64_t len) {
    const uint8_t *p = src;
    uint8_t *msg = wmtr_upload_scratch();
    if (!msg) { wmtr_last_upload_status = 0xfffffffeu; return -1; }   /* local, not host */
    while (len) {
        uint64_t n = len > (RM_CHUNK_BYTES - sizeof(struct rm_buffer_range))
                   ? (RM_CHUNK_BYTES - sizeof(struct rm_buffer_range)) : len;
        struct rm_buffer_range *r = (void *)msg;
        r->handle = handle; r->offset = off; r->length = n;
        memcpy(msg + sizeof *r, p, n);
        uint32_t st = wmtr_call(RM_OP_BUFFER_UPLOAD, msg, (uint32_t)(sizeof *r + n), 0, 0, 0);
        if (st != RM_OK) { wmtr_last_upload_status = st; return -1; }
        p += n; off += n; len -= n;
    }
    return 0;
}

static int wmtr_heap_attach(uint64_t handle, uint64_t heap_handle, uint64_t offset, void *shadow, uint64_t length) {
    int result = -1;
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_heap *heap = wmtr_heaps;
    while (heap && heap->handle != heap_handle) heap = heap->next;
    struct wmtr_buf *buffer = wmtr_buf_find_locked(handle);
    if (heap && heap->refs && buffer && shadow && offset <= heap->size && length <= heap->size - offset &&
        (uintptr_t)shadow >= offset) {
        uint8_t *base = (uint8_t *)shadow - (size_t)offset;
        if (!heap->guest || heap->guest == base) {
            heap->guest = base; heap->buffers++;
            buffer->heap = heap; buffer->heap_offset = offset;
            result = 0;
        }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return result;
}

/* Send only bytes changed by the CPU, so a stale alias cannot overwrite GPU data. */
static int wmtr_heap_flush_locked(void) {
    uint8_t *scratch = malloc(RM_CHUNK_BYTES);
    if (!scratch) return -1;
    int failed = 0;
    for (struct wmtr_buf_chunk *chunk = wmtr_chunks; chunk && !failed; chunk = chunk->next)
    for (unsigned ci = 0; ci < chunk->used && !failed; ci++) {
        struct wmtr_buf *b = &chunk->entries[ci];
        if (!b->handle || !b->heap) continue;
        uint8_t *guest = b->shadow, *baseline = b->heap->baseline + b->heap_offset;
        uint64_t i = 0;
        while (i < b->length) {
            uint8_t value = __atomic_load_n(guest + i, __ATOMIC_RELAXED);
            if (value == baseline[i]) { i++; continue; }
            uint64_t start = i, count = 0;
            do {
                scratch[count++] = value; i++;
                if (i == b->length || count == RM_CHUNK_BYTES) break;
                value = __atomic_load_n(guest + i, __ATOMIC_RELAXED);
            } while (value != baseline[i]);
            if (wmtr_buf_upload(b->handle, scratch, start, count)) { failed = 1; break; }
            memcpy(baseline + start, scratch, (size_t)count);
        }
    }
    free(scratch);
    return failed ? -1 : 0;
}
static int wmtr_heap_flush(void) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    int result = wmtr_heap_flush_locked();
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return result;
}

/* Preserve CPU writes made after submission while applying the completed GPU data. */
static int wmtr_heap_sync(void) {
    pthread_mutex_lock(&wmtr_bufs_lock);
    if (!wmtr_heaps) { pthread_mutex_unlock(&wmtr_bufs_lock); return 0; }
    uint8_t *scratch = malloc(RM_CHUNK_BYTES);
    if (!scratch) { pthread_mutex_unlock(&wmtr_bufs_lock); return -1; }
    int failed = 0;
    for (struct wmtr_buf_chunk *chunk = wmtr_chunks; chunk && !failed; chunk = chunk->next)
    for (unsigned ci = 0; ci < chunk->used && !failed; ci++) {
        struct wmtr_buf *b = &chunk->entries[ci];
        if (!b->handle || !b->heap) continue;
        uint8_t *guest = b->shadow, *baseline = b->heap->baseline + b->heap_offset;
        for (uint64_t off = 0; off < b->length;) {
            uint32_t n = (uint32_t)((b->length - off > RM_CHUNK_BYTES) ? RM_CHUNK_BYTES : b->length - off);
            struct rm_buffer_range range = {b->handle, off, n}; uint32_t got = 0;
            if (wmtr_call(RM_OP_BUFFER_READ, &range, sizeof(range), scratch, n, &got) != RM_OK || got != n) {
                failed = 1; break;
            }
            for (uint32_t i = 0; i < n; i++) {
                uint8_t expected = baseline[off + i];
                __atomic_compare_exchange_n(guest + off + i, &expected, scratch[i], 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
                baseline[off + i] = scratch[i];
            }
            off += n;
        }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    free(scratch);
    return failed ? -1 : 0;
}
static int wmtr_heap_update(uint64_t handle, uint64_t offset, const void *data, uint64_t length) {
    int result = -1;
    pthread_mutex_lock(&wmtr_bufs_lock);
    struct wmtr_buf *b = wmtr_buf_find_locked(handle);
    if (b && b->heap && data && offset <= b->length && length <= b->length - offset && length <= SIZE_MAX) {
        uint8_t *scratch = malloc(RM_CHUNK_BYTES);
        if (scratch) {
            uint8_t *guest = (uint8_t *)b->shadow + offset;
            memmove(guest, data, (size_t)length);
            result = 0;
            for (uint64_t off = 0; off < length;) {
                size_t n = (size_t)((length - off > RM_CHUNK_BYTES) ? RM_CHUNK_BYTES : length - off);
                for (size_t i = 0; i < n; i++) scratch[i] = __atomic_load_n(guest + off + i, __ATOMIC_RELAXED);
                if (wmtr_buf_upload(handle, scratch, offset + off, n)) { result = -1; break; }
                memcpy(b->heap->baseline + b->heap_offset + offset + off, scratch, n);
                off += n;
            }
            free(scratch);
        }
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    return result;
}

/* ---- ml821: coalesce many ranges into one round trip --------------------
 *
 * The flush sent one message per changed run. In gameplay that was 104 upload
 * calls per submission carrying 222 KB -- a mean payload of 1,179 bytes against
 * an 8 MB cap -- and at ~6 submissions per displayed frame the call count, not
 * the bytes, was the largest single cost in the frame.
 *
 * Bytes, ranges and validation are unchanged; only the framing differs. Failure
 * is still per-message: if the one message fails, every run inside it is
 * invalidated exactly as the single-range path did, so nothing is ever left
 * marked delivered when it was not.
 *
 * Used only inside wmtr_flush_buffers, which holds wmtr_bufs_lock for its whole
 * walk, so the state below is single-threaded and the wmtr_buf pointers it
 * remembers cannot retire underneath it. */
struct wmtr_ub {
    uint8_t *buf;
    uint32_t data_len, data_cap, count;
    struct rm_buffer_range r[RM_MULTI_MAX_RANGES];
    struct { struct wmtr_buf *b; uint32_t p0, p1; } pend[RM_MULTI_MAX_RANGES];
};

/* ml822: the batch MUST NOT share wmtr_upload_scratch().
 *
 * It did, and that silently corrupted every coalesced message that had a
 * single-range upload run inside the same walk -- the oversized-run fallback
 * below, and the whole-buffer path taken when a page_sum allocation failed.
 * Both write their own header and payload at the start of that same per-thread
 * buffer, overwriting bytes the batch had already accumulated. The descriptors
 * and count live in this struct, not in the buffer, so the message stayed
 * perfectly well formed: it passed every validator on both sides, the host
 * reported success, and the wrong bytes landed in the wrong buffers. Zero
 * errors, black screen. A separate allocation is the whole fix. */
static uint8_t *wmtr_batch_scratch(void) {
    /* One buffer, reused. Every user holds wmtr_bufs_lock for the whole walk. */
    static uint8_t *b;
    if (b) return b;
    b = malloc(RM_CHUNK_BYTES);
    if (!b) {
        void *m = mmap(NULL, RM_CHUNK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                       VM_MAKE_TAG(VM_MEMORY_MALLOC), 0);
        if (m != MAP_FAILED) b = m;
    }
    if (!b) fprintf(stderr, "[wmt-remote] ml822 batch buffer allocation FAILED -- "
                            "coalescing disabled for this process\n");
    return b;
}

static void wmtr_ub_init(struct wmtr_ub *u) {
    u->buf = wmtr_batch_scratch();
    u->count = u->data_len = 0;
    u->data_cap = u->buf ? (uint32_t)(RM_CHUNK_BYTES - sizeof(struct rm_buffer_multi)
                                      - RM_MULTI_MAX_RANGES * sizeof(struct rm_buffer_range))
                         : 0;
}

/* Returns the number of runs that FAILED (0 on success). */
static unsigned wmtr_ub_send(struct wmtr_ub *u) {
    if (!u->count) return 0;
    struct rm_buffer_multi *m = (void *)u->buf;
    m->count = u->count; m->data_bytes = u->data_len;
    uint8_t *tail = u->buf + sizeof *m + u->data_len;
    memcpy(tail, u->r, u->count * sizeof u->r[0]);
    uint32_t total = (uint32_t)(sizeof *m + u->data_len + u->count * sizeof u->r[0]);
    uint32_t st = wmtr_call(RM_OP_BUFFER_UPLOAD_MULTI, u->buf, total, 0, 0, 0);
    unsigned failed = 0;
    if (st != RM_OK) {
        wmtr_last_upload_status = st;
        for (uint32_t i = 0; i < u->count; i++) {
            struct wmtr_buf *b = u->pend[i].b;
            if (b && b->page_sum)
                for (uint32_t p = u->pend[i].p0; p < u->pend[i].p1 && p < b->pages; p++)
                    b->page_sum[p] = 0;
            failed++;
        }
        static unsigned told;
        if (told++ < 8)
            fprintf(stderr, "[wmt-remote] ml821 coalesced upload FAILED status=%u "
                            "(%u ranges, %u bytes) -- all invalidated for retry\n",
                    st, u->count, u->data_len);
    } else {
        wmtr_ub_msgs++; wmtr_ub_ranges += u->count;
        wmtr_flush_sent += u->data_len;
    }
    u->count = u->data_len = 0;
    return failed;
}

static unsigned wmtr_ub_add(struct wmtr_ub *u, struct wmtr_buf *b, uint32_t p0, uint32_t p1,
                            uint64_t off, uint64_t len) {
    unsigned failed = 0;
    if (!u->buf || len > u->data_cap) {
        /* Too large for one coalesced message: the chunked single-range path
         * already handles this correctly, so use it rather than special-casing. */
        if (wmtr_buf_upload(b->handle, (const uint8_t *)b->shadow + off, off, len) != 0) {
            if (b->page_sum)
                for (uint32_t p = p0; p < p1 && p < b->pages; p++) b->page_sum[p] = 0;
            return 1;
        }
        wmtr_flush_sent += len;
        return 0;
    }
    if (u->count == RM_MULTI_MAX_RANGES || u->data_len + len > u->data_cap)
        failed = wmtr_ub_send(u);
    memcpy(u->buf + sizeof(struct rm_buffer_multi) + u->data_len,
           (const uint8_t *)b->shadow + off, (size_t)len);
    u->r[u->count].handle = b->handle;
    u->r[u->count].offset = off;
    u->r[u->count].length = len;
    u->pend[u->count].b = b; u->pend[u->count].p0 = p0; u->pend[u->count].p1 = p1;
    u->count++; u->data_len += (uint32_t)len;
    return failed;
}

/* Push every CPU-visible shadow to the host. Called before submission. */
static int wmtr_flush_buffers(void) {
    /* Held for the whole walk: an entry retiring mid-flush would otherwise let
     * this hash a shadow the owner has already freed. */
    pthread_mutex_lock(&wmtr_bufs_lock);
    unsigned n = 0, failed = wmtr_heaps && wmtr_heap_flush_locked() != 0;
    struct timeval t_f0; gettimeofday(&t_f0, 0);                 /* ml819 */
    unsigned long rpc_before = wmtr_call_n; double rpc_ms_before = wmtr_call_ms;
    static struct wmtr_ub ub;            /* ml821: safe -- the walk holds the lock */
    if (wmtr_batch_on) wmtr_ub_init(&ub);
    for (struct wmtr_buf_chunk *chunk = wmtr_chunks; chunk; chunk = chunk->next)
    for (unsigned ci = 0; ci < chunk->used; ci++) {
        struct wmtr_buf *b = &chunk->entries[ci];
        if (!b->handle) continue;
        n++;
        if (!b->cpu_visible || !b->shadow || !b->length || b->heap) continue;
        wmtr_flush_seen += b->length;

        if (!b->page_sum) {
            b->pages = (uint32_t)((b->length + WMTR_PAGE - 1) / WMTR_PAGE);
            b->page_sum = calloc(b->pages, sizeof *b->page_sum);
        }
        if (b->page_sum) {
            /* Send only pages whose contents differ from what the host holds,
             * coalescing neighbours so one changed run is one message. */
            uint32_t p0 = 0;
            while (p0 < b->pages) {
                size_t off = (size_t)p0 * WMTR_PAGE;
                size_t len = b->length - off < WMTR_PAGE ? (size_t)(b->length - off) : WMTR_PAGE;
                uint64_t sum = wmtr_page_sum((const uint8_t *)b->shadow + off, len);
                if (sum == b->page_sum[p0]) { p0++; continue; }
                uint32_t p1 = p0;
                size_t run = 0;
                while (p1 < b->pages) {
                    size_t o2 = (size_t)p1 * WMTR_PAGE;
                    size_t l2 = b->length - o2 < WMTR_PAGE ? (size_t)(b->length - o2) : WMTR_PAGE;
                    uint64_t s2 = wmtr_page_sum((const uint8_t *)b->shadow + o2, l2);
                    if (p1 != p0 && s2 == b->page_sum[p1]) break;
                    b->page_sum[p1] = s2; run += l2; p1++;
                }
                if (wmtr_batch_on) {
                    failed += wmtr_ub_add(&ub, b, p0, p1, off, run);
                    p0 = p1;
                    continue;
                }
                if (wmtr_buf_upload(b->handle, (const uint8_t *)b->shadow + off,
                                    off, run) != 0) {
                    /* ml815: a FAILED upload must not leave these pages marked
                     * as delivered.
                     *
                     * The checksums above are written BEFORE the transfer is
                     * known to have succeeded. If it then fails, the next flush
                     * compares the guest bytes against a checksum claiming the
                     * host already has them, matches, and skips the range --
                     * permanently. The bytes never arrive and nothing ever
                     * retries, so the failure is silent and irreversible.
                     *
                     * wmtr_page_sum reserves 0 for "never hashed", so zeroing
                     * the run is exactly the right invalidation: the next flush
                     * recomputes, cannot match, and resends. */
                    uint32_t inv;
                    for (inv = p0; inv < p1; inv++) b->page_sum[inv] = 0;
                    failed++;
                    static unsigned told_pg;                       /* ml819 */
                    if (told_pg++ < 8)
                        fprintf(stderr, "[wmt-remote] ml819 page upload FAILED status=%u buffer 0x%llx "
                                        "off=%zu run=%zu len=%llu options=0x%x\n",
                                wmtr_last_upload_status, (unsigned long long)b->handle, off, run,
                                (unsigned long long)b->length, b->options);
                } else wmtr_flush_sent += run;
                p0 = p1;
            }
            continue;
        }

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
    if (wmtr_batch_on) failed += wmtr_ub_send(&ub);   /* ml821 */
    {   /* ml819: where a flush spends its time, accumulated between reports */
        struct timeval t_f1; gettimeofday(&t_f1, 0);
        double fms = (t_f1.tv_sec - t_f0.tv_sec) * 1000.0 + (t_f1.tv_usec - t_f0.tv_usec) / 1000.0;
        double rpcms = wmtr_call_ms - rpc_ms_before;
        wmtr_flush_ms += fms; wmtr_flush_rpc_ms += rpcms; wmtr_flush_rpcs += wmtr_call_n - rpc_before;
    }
    if ((++wmtr_flush_n % 60) == 0) {
        fprintf(stderr, "[wmt-remote] flush #%lu: %u buffers, %llu MB considered, "
                        "%llu MB sent (%.1f%%) | batches packed=%lu dropped=%lu\n", wmtr_flush_n, n,
                wmtr_flush_seen >> 20, wmtr_flush_sent >> 20,
                wmtr_flush_seen ? 100.0 * wmtr_flush_sent / wmtr_flush_seen : 0.0,
                wmtr_batches_packed, wmtr_batches_dropped);
        fprintf(stderr, "[wmt-remote] ml820 lifetime: pool-released=%lu | readback downloads=%lu %lu MB "
                        "failed=%lu dropped=%lu | registry live=%u\n", wmtr_pool_released,
                wmtr_rb_downloads, wmtr_rb_bytes >> 20, wmtr_rb_failed, wmtr_rb_dropped, wmtr_bufs_live);
        fprintf(stderr, "[wmt-remote] ml822 batching=%s (flush uploads + pool releases only) | "
                        "upload msgs=%lu carrying %lu ranges (%.1f per msg) | "
                        "release msgs=%lu carrying %lu\n",
                wmtr_batch_on ? "ON" : "off", wmtr_ub_msgs, wmtr_ub_ranges,
                wmtr_ub_msgs ? (double)wmtr_ub_ranges / wmtr_ub_msgs : 0.0,
                wmtr_rel_msgs, wmtr_rel_handles);
        /* ml819 census for the last 60 flushes: wall time inside flush, how much of
         * it was upload RPCs (the rest is hashing), and every RPC in the process
         * by opcode -- count, total ms, mean ms. Opcode numbers map to
         * protocol.h; the frame count is the flush count since flushes run once
         * per submission. */
        static struct timeval t_rep; struct timeval t_now; gettimeofday(&t_now, 0);
        double wall = t_rep.tv_sec ? (t_now.tv_sec - t_rep.tv_sec) * 1000.0
                                   + (t_now.tv_usec - t_rep.tv_usec) / 1000.0 : 0.0;
        t_rep = t_now;
        static unsigned long last_n[256]; static double last_ms[256];
        static unsigned long last_calls; static double last_calls_ms;
        unsigned long dn = wmtr_call_n - last_calls; double dms = wmtr_call_ms - last_calls_ms;
        fprintf(stderr, "[rpc-census] ml819 60 flushes in %.0f ms wall: flush=%.0f ms (upload rpc %.0f ms, "
                        "%lu upload rpcs, hash+scan %.0f ms) | ALL rpcs=%lu %.0f ms (%.1f%% of wall, "
                        "%.2f ms mean)\n", wall, wmtr_flush_ms, wmtr_flush_rpc_ms, wmtr_flush_rpcs,
                wmtr_flush_ms - wmtr_flush_rpc_ms, dn, dms, wall > 0 ? 100.0 * dms / wall : 0.0,
                dn ? dms / dn : 0.0);
        wmtr_flush_ms = wmtr_flush_rpc_ms = 0; wmtr_flush_rpcs = 0;
        /* top opcodes by time */
        unsigned i, k;
        for (k = 0; k < 10; k++) {
            unsigned best = 256; double bms = -1;
            for (i = 0; i < 256; i++) {
                double d = wmtr_op_ms[i] - last_ms[i];
                if (wmtr_op_n[i] - last_n[i] && d > bms) { bms = d; best = i; }
            }
            if (best == 256) break;
            unsigned long cn = wmtr_op_n[best] - last_n[best];
            fprintf(stderr, "[rpc-census]   op %3u: %8lu calls %8.0f ms %6.2f ms/call %6.1f calls/flush\n",
                    best, cn, bms, bms / cn, cn / 60.0);
            last_n[best] = wmtr_op_n[best]; last_ms[best] = wmtr_op_ms[best];
        }
        for (i = 0; i < 256; i++) { last_n[i] = wmtr_op_n[i]; last_ms[i] = wmtr_op_ms[i]; }
        last_calls = wmtr_call_n; last_calls_ms = wmtr_call_ms;
    }
    pthread_mutex_unlock(&wmtr_bufs_lock);
    if (failed) {
        static unsigned reported;
        if (reported++ < 4)
            fprintf(stderr, "[wmt-remote] %u buffer upload(s) failed during flush\n", failed);
    }
    return failed ? -1 : 0;
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
