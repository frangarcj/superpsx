/*
 * dynarec_cache.c - Block cache, direct linking, and PSX code resolution
 *
 * Manages the block hash table, overflow chaining, direct block linking
 * (back-patching), and PSX PC → host pointer resolution.
 */
#include "dynarec.h"

/* ---- Block cache storage ---- */
BlockEntry *block_cache;
BlockEntry *block_node_pool;
int block_node_pool_idx = 0;

/* ---- Direct block linking state ---- */
PatchSite patch_sites[PATCH_SITE_MAX];
int patch_sites_count = 0;
#ifdef ENABLE_DYNAREC_STATS
uint64_t stat_dbl_patches = 0;
#endif

/* ---- Temp buffer for IO code execution ---- */
static uint32_t io_code_buffer[64];

/* ---- Block lookup (native pointer only, no stats) ---- */
uint32_t *lookup_block_native(uint32_t psx_pc)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *e = &block_cache[idx];
    while (e)
    {
        if (e->native && e->psx_pc == psx_pc)
            return e->native;
        e = e->next;
    }
    return NULL;
}

/*
 * emit_direct_link: at the end of a block epilogue, emit a J to the
 * native code of target_psx_pc.  If not compiled yet, emit a J to the
 * slow-path trampoline (code_buffer[0]) and record a patch site.
 */
void emit_direct_link(uint32_t target_psx_pc)
{
    BlockEntry *be = &block_cache[(target_psx_pc >> 2) & BLOCK_CACHE_MASK];
    if (be->native != NULL && be->psx_pc == target_psx_pc)
    {
        /* Block already exists (e.g., backward loop). Link immediately! */
        uint32_t native_addr = (uint32_t)(be->native + DYNAREC_PROLOGUE_WORDS);
        EMIT_J_ABS(native_addr);
        EMIT_NOP();
#ifdef ENABLE_DYNAREC_STATS
        stat_dbl_patches++;
#endif
        return;
    }

    /* Target not compiled yet: record patch site and J to slow-path trampoline */
    if (patch_sites_count < PATCH_SITE_MAX)
    {
        PatchSite *ps = &patch_sites[patch_sites_count++];
        ps->site_word = code_ptr;
        ps->target_psx_pc = target_psx_pc;
    }
    /* J to JIT exit trampoline (abort_trampoline_addr) */
    EMIT_J_ABS((uint32_t)abort_trampoline_addr);
    EMIT_NOP();
}

/* apply_pending_patches: back-patch all J stubs waiting for target_psx_pc. */
void apply_pending_patches(uint32_t target_psx_pc, uint32_t *native_addr)
{
    int i, j;
    for (i = 0, j = 0; i < patch_sites_count; i++)
    {
        PatchSite *ps = &patch_sites[i];
        if (ps->target_psx_pc == target_psx_pc)
        {
            uint32_t j_target = ((uint32_t)(native_addr + DYNAREC_PROLOGUE_WORDS) >> 2) & 0x03FFFFFF;
            *ps->site_word = MK_J(2, j_target);
#ifdef ENABLE_DYNAREC_STATS
            stat_dbl_patches++;
#endif
        }
        else
        {
            patch_sites[j++] = patch_sites[i];
        }
    }
    patch_sites_count = j;
}

/* ---- Get pointer to PSX code in EE memory ---- */
uint32_t *get_psx_code_ptr(uint32_t psx_pc)
{
    uint32_t phys = psx_pc & 0x1FFFFFFF;
    if (phys < PSX_RAM_SIZE)
        return (uint32_t *)(psx_ram + phys);
    if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
        return (uint32_t *)(psx_bios + (phys - 0x1FC00000));

    /* IO regions that support instruction fetch:
     *   DMA registers  (0x1F801080-0x1F8010FF)
     *   SPU registers  (0x1F801C00-0x1F801FFF)
     */
    if ((phys >= 0x1F801080 && phys < 0x1F801100) ||
        (phys >= 0x1F801C00 && phys < 0x1F802000))
    {
        int i;
        memset(io_code_buffer, 0, sizeof(io_code_buffer));
        for (i = 0; i < 64; i++)
        {
            uint32_t addr = psx_pc + i * 4;
            uint32_t a_phys = addr & 0x1FFFFFFF;
            if (!((a_phys >= 0x1F801080 && a_phys < 0x1F801100) ||
                  (a_phys >= 0x1F801C00 && a_phys < 0x1F802000)))
                break;
            io_code_buffer[i] = ReadWord(addr);
        }
        return io_code_buffer;
    }

    return NULL;
}

/* ---- Block lookup (with collision chain traversal and stats) ---- */
uint32_t *lookup_block(uint32_t psx_pc)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *e = &block_cache[idx];
    while (e)
    {
        if (e->native && e->psx_pc == psx_pc)
        {
#ifdef ENABLE_DYNAREC_STATS
            stat_cache_hits++;
#endif
            return e->native;
        }
        e = e->next;
    }
    /* Not found - check if bucket is occupied (collision) */
    if (block_cache[idx].native && block_cache[idx].psx_pc != psx_pc)
    {
#ifdef ENABLE_DYNAREC_STATS
        stat_cache_collisions++;
#endif
    }
#ifdef ENABLE_DYNAREC_STATS
    stat_cache_misses++;
#endif
    return NULL;
}

void cache_block(uint32_t psx_pc, uint32_t *native)
{
    uint32_t idx = (psx_pc >> 2) & BLOCK_CACHE_MASK;
    BlockEntry *bucket = &block_cache[idx];

    /* Case 1: bucket is empty */
    if (!bucket->native)
    {
        bucket->psx_pc = psx_pc;
        bucket->native = native;
        return;
    }

    /* Case 2: same PC - update in place (recompile) */
    BlockEntry *e = bucket;
    while (e)
    {
        if (e->psx_pc == psx_pc)
        {
            e->native = native;
            return;
        }
        e = e->next;
    }

    /* Case 3: collision - allocate from pool and prepend to chain */
    if (block_node_pool_idx < BLOCK_NODE_POOL_SIZE)
    {
        BlockEntry *node = &block_node_pool[block_node_pool_idx++];
        node->psx_pc = psx_pc;
        node->native = native;
        node->instr_count = 0;
        node->cycle_count = 0;
        node->next = bucket->next;
        bucket->next = node;
    }
    else
    {
        /* Pool exhausted: fall back to overwrite (rare) */
        DLOG("Block node pool exhausted! Overwriting bucket at idx=%u\n", (unsigned)idx);
        bucket->psx_pc = psx_pc;
        bucket->native = native;
    }
}
