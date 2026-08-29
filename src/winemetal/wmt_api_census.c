/* ml761: top-level winemetal API census -- counters and report.
 *
 * Classifies each observed call, because the question this answers is not
 * "how busy is the API" but "which calls make and take handles". Remote replay
 * cannot use the guest's pointer-cast handles, so every producer and consumer
 * has to be redirected together; a half-switched state mixes address spaces
 * and fails as a wrong-looking frame rather than an error.
 */
#include "wmt_api_names.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_ulong g_calls[WMT_API_COUNT];
static atomic_ulong g_total;
static int g_on = -1;

/* Bounded first-call sequence: the ORDER of the control plane is what has to
 * be reimplemented, and a frequency table alone does not show it. */
#define FIRSTN 96
#define FIRST_EMPTY 0xffffu
/* Atomic slots with a sentinel. Claiming the index atomically is not enough:
 * the reporter could read a claimed slot before its value was written and
 * print whatever was there. Each slot is now atomic and starts EMPTY, so a
 * half-published entry is visibly missing rather than silently wrong. */
static atomic_ushort g_first[FIRSTN];
static atomic_uint g_first_len;
static atomic_int g_first_init;

/* Classification. A call can be more than one thing; the label names its
 * primary role for the purpose of redirecting the control plane. */
/* Producers are named EXPLICITLY. Two heuristic attempts were both wrong:
 * `strstr(n,"new")==n` matched nothing (every entry is Class_newThing), and
 * broadening to "CommandEncoder" then swept in endEncoding and encodeCommands,
 * which consume an encoder rather than produce one. A census exists to be read,
 * and a confidently wrong label is worse than a missing one. */
static int is_producer(const char *n) {
    static const char *const P[] = {
        "MTLCopyAllDevices", "NSArray_object", "NSString_string", "NSString_alloc_init",
        "NSAutoreleasePool_alloc_init", "MTLDevice_newCommandQueue",
        "MTLCommandQueue_commandBuffer", "MTLCommandBuffer_renderCommandEncoder",
        "MTLCommandBuffer_computeCommandEncoder", "MTLCommandBuffer_blitCommandEncoder",
        "MTLDevice_newBuffer", "MTLDevice_newTexture", "MTLDevice_newLibrary",
        "MTLLibrary_newFunction", "MTLDevice_newRenderPipelineState",
        "MTLDevice_newComputePipelineState", "MTLDevice_newDepthStencilState",
        "MTLDevice_newSamplerState", "MTLDevice_newSharedEvent",
        "MTLDevice_newBinaryArchive", "MTLTexture_newTextureView",
        "MetalLayer_nextDrawable", "MetalDrawable_texture", 0
    };
    for (int i = 0; P[i]; i++) if (strcmp(n, P[i]) == 0) return 1;
    return 0;
}

static const char *classify(const char *n) {
    if (is_producer(n)) return "PRODUCER";
    if (strstr(n, "retain") || strstr(n, "release"))          return "lifetime";
    if (strstr(n, "commit") || strstr(n, "waitUntil") ||
        strstr(n, "Event") || strstr(n, "Fence"))             return "sync";
    if (strstr(n, "present") || strstr(n, "Layer"))           return "present";
    if (strstr(n, "Bytes") || strstr(n, "Contents") ||
        strstr(n, "replaceRegion") || strstr(n, "getBytes"))  return "bulk-memory";
    if (strstr(n, "supports") || strstr(n, "status") ||
        strstr(n, "count") || strstr(n, "Size") ||
        strstr(n, "name") || strstr(n, "error"))              return "query";
    return "consumer";
}

static void wmt_api_report(void) {
    if (g_on != 1) return;
    unsigned long tot = atomic_load(&g_total);
    fprintf(stderr, "\n[api-census] ml761 total calls=%lu across %d entries\n", tot, WMT_API_COUNT);
    unsigned used = 0;
    for (int i = 0; i < WMT_API_COUNT; i++) {
        unsigned long c = atomic_load(&g_calls[i]);
        if (!c) continue;
        used++;
        fprintf(stderr, "[api-census]   %-12s %-44s %lu\n",
                classify(wmt_api_names[i]), wmt_api_names[i], c);
    }
    fprintf(stderr, "[api-census] %u of %d entries used\n", used, WMT_API_COUNT);
    unsigned fl = atomic_load(&g_first_len);
    if (fl > FIRSTN) fl = FIRSTN;
    if (fl) {
        fprintf(stderr, "[api-census] first %u calls (control-plane ORDER):\n", fl);
        for (unsigned i = 0; i < fl; i++) {
            unsigned short code = atomic_load(&g_first[i]);
            if (code == FIRST_EMPTY) { fprintf(stderr, "[api-census]     %2u. <pending>\n", i); continue; }
            fprintf(stderr, "[api-census]     %2u. %s\n", i,
                    code < WMT_API_COUNT ? wmt_api_names[code] : "?");
        }
    }
}

void wmt_api_census_note(unsigned code) {
    if (__builtin_expect(g_on < 0, 0)) {
        const char *e = getenv("DXMT_API_CENSUS");
        g_on = (e && e[0] == '1') ? 1 : 0;
        if (g_on) fprintf(stderr, "[api-census] ml761 armed (%d entries)\n", WMT_API_COUNT);
    }
    if (__builtin_expect(g_on != 1, 1)) return;
    if (code < WMT_API_COUNT) atomic_fetch_add(&g_calls[code], 1);
    unsigned long t = atomic_fetch_add(&g_total, 1) + 1;
    /* Claim a slot atomically. Load-then-store races between threads and can
     * drop or duplicate entries in the very sequence the control plane has to
     * be reimplemented from. */
    if (!atomic_exchange(&g_first_init, 1))
        for (int i = 0; i < FIRSTN; i++) atomic_store(&g_first[i], FIRST_EMPTY);
    unsigned slot = atomic_fetch_add(&g_first_len, 1);
    if (slot < FIRSTN) atomic_store(&g_first[slot], (unsigned short)code);

    /* Report often enough that the last checkpoint is close to the end. atexit
     * never fires here -- iOS apps are killed, not exited -- so "the whole run"
     * can only ever mean "the latest checkpoint", and 20,000 was too coarse. */
    if (t == 1 || t == 100 || t == 1000 || (t % 5000) == 0) wmt_api_report();
}
