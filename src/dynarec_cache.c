/*
 * dynarec_cache.c - Block cache, direct linking, and PSX code resolution
 *
 * Manages the block hash table, overflow chaining, direct block linking
 * (back-patching), and PSX PC → host pointer resolution.
 */
#include "dynarec.h"

/* ---- Block cache storage ---- */
/* ---- Page Table storage ---- */
jit_l2_t jit_l1_ram[JIT_L1_RAM_PAGES];
jit_l2_t jit_l1_bios[JIT_L1_BIOS_PAGES];

/* ---- SMC page generation counters ---- */
uint8_t jit_page_gen[JIT_L1_RAM_PAGES];

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


/*
 * emit_direct_link: at the end of a block epilogue, emit a J to the
 * native code of target_psx_pc.  If not compiled yet, emit a J to the
 * slow-path trampoline (code_buffer[0]) and record a patch site.
 */
void emit_direct_link(uint32_t target_psx_pc)
{
    BlockEntry *be = lookup_block(target_psx_pc);
    if (be && be->native != NULL)
    {
        /* SMC check: compare page generation counter.  If the page was
         * written to since block compilation, the block may be stale. */
        uint32_t phys = target_psx_pc & 0x1FFFFFFF;
        if (phys < PSX_RAM_SIZE && be->page_gen != jit_get_page_gen(phys))
        {
            be->native = NULL;
            be = NULL;
        }
    }
    if (be && be->native != NULL)
    {
        /* Block already exists and is valid. Link immediately! */
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


BlockEntry *cache_block(uint32_t psx_pc, uint32_t *native)
{
    uint32_t phys = psx_pc & 0x1FFFFFFF;
    uint32_t l1_idx, l2_idx;
    jit_l2_t *l1_table = NULL;

    if (phys < PSX_RAM_SIZE)
    {
        l1_table = jit_l1_ram;
        l1_idx = phys >> 12;
    }
    else if (phys >= 0x1FC00000 && phys < 0x1FC00000 + PSX_BIOS_SIZE)
    {
        l1_table = jit_l1_bios;
        l1_idx = (phys - 0x1FC00000) >> 12;
    }

    if (!l1_table) return NULL;

    /* Allocate L2 page if needed */
    if (l1_table[l1_idx] == NULL)
    {
        l1_table[l1_idx] = calloc(1, sizeof(BlockEntry*) * JIT_L2_ENTRIES);
        if (!l1_table[l1_idx]) return NULL;
    }

    l2_idx = (phys >> 2) & (JIT_L2_ENTRIES - 1);
    
    /* Allocate or reuse BlockEntry */
    BlockEntry *be = (*l1_table[l1_idx])[l2_idx];
    if (!be)
    {
        if (block_node_pool_idx < BLOCK_NODE_POOL_SIZE)
        {
            be = &block_node_pool[block_node_pool_idx++];
            (*l1_table[l1_idx])[l2_idx] = be;
        }
    }

    if (be)
    {
        be->psx_pc = psx_pc;
        be->native = native;
        be->next = NULL;
        be->page_gen = jit_get_page_gen(phys);
    }
    return be;
}

void Free_PageTable(void)
{
    int i;
    for (i = 0; i < JIT_L1_RAM_PAGES; i++)
    {
        if (jit_l1_ram[i])
        {
            free(jit_l1_ram[i]);
            jit_l1_ram[i] = NULL;
        }
    }
    for (i = 0; i < JIT_L1_BIOS_PAGES; i++)
    {
        if (jit_l1_bios[i])
        {
            free(jit_l1_bios[i]);
            jit_l1_bios[i] = NULL;
        }
    }
}
