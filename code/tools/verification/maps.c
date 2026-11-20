#include <assert.h>
#include <pmm.h>
#include <string.h>
#include <sync.h>
#include <riscv.h>
#include <stdio.h>
#include <error.h>

static const struct pmm_manager *real_manager;
static int allocation_budget;
static struct Page *budget_alloc(size_t n)
{
    if (allocation_budget == 0) return NULL;
    --allocation_budget;
    return real_manager->alloc_pages(n);
}

void lab4_map_checks(void)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    size_t initial_free = nr_free_pages();
    struct Page *root_page = alloc_page();
    struct Page *a = alloc_page(), *b = alloc_page();
    assert(root_page && a && b);
    pde_t *root = page2kva(root_page);
    memcpy(root, boot_pgdir_va, PGSIZE);
    assert(root[0] == 0 && root[1] == 0);
    uintptr_t va = 0x400000;
    assert(page_insert(root, a, va + 0x88, PTE_R | PTE_W) == 0);
    assert(page_ref(a) == 1 && get_page(root, va, NULL) == a);
    size_t free_before = nr_free_pages();
    assert(page_insert(root, a, va, PTE_R) == 0);
    assert(page_ref(a) == 1 && nr_free_pages() == free_before);
    pte_t *entry = get_pte(root, va, 0);
    assert((*entry & (PTE_V | PTE_R | PTE_W | PTE_U)) == (PTE_V | PTE_R));
    assert(page_insert(root, a, va + PGSIZE, PTE_R | PTE_W) == 0);
    assert(page_ref(a) == 2);
    assert(page_insert(root, b, va + PGSIZE, PTE_R | PTE_W) == 0);
    assert(page_ref(a) == 1 && page_ref(b) == 1);
    page_remove(root, va + PGSIZE);
    assert(page_ref(b) == 0 && get_page(root, va + PGSIZE, NULL) == NULL);
    free_before = nr_free_pages();
    page_remove(root, va + PGSIZE);
    page_remove(root, 0x80000000);
    assert(nr_free_pages() == free_before && root[2] == 0);
    cprintf("Lab4 extra: mapping aliases, same-page permissions, replacement and idempotent remove PASS\n");

    b = alloc_page();
    assert(b != NULL);
    *(volatile uintptr_t *)page2kva(a) = 0x2412396;
    *(volatile uintptr_t *)page2kva(b) = 0x2411688;
    uintptr_t old_satp = read_csr(satp);
    write_csr(satp, ((uintptr_t)8 << 60) | page2ppn(root_page));
    flush_tlb();
    assert(*(volatile uintptr_t *)va == 0x2412396);
    assert(page_insert(root, b, va, PTE_R | PTE_W) == 0);
    assert(*(volatile uintptr_t *)va == 0x2411688);
    *(volatile uintptr_t *)va = 0x2413992;
    assert(*(volatile uintptr_t *)page2kva(b) == 0x2413992);
    write_csr(satp, old_satp);
    flush_tlb();
    page_remove(root, va);
    assert(page_ref(a) == 0 && page_ref(b) == 0 && *entry == 0);
    cprintf("Lab4 extra: actual load/remap/store observes the new page after TLB invalidation PASS\n");

    struct Page *target = alloc_page();
    assert(target != NULL);
    struct pmm_manager fault_manager = *pmm_manager;
    real_manager = pmm_manager;
    fault_manager.alloc_pages = budget_alloc;
    for (int budget = 0; budget <= 1; ++budget) {
        free_before = nr_free_pages();
        allocation_budget = budget;
        pmm_manager = &fault_manager;
        int result = page_insert(root, target, 0x40000000, PTE_R | PTE_W);
        pmm_manager = real_manager;
        assert(result == -E_NO_MEM && page_ref(target) == 0);
        assert(root[1] == 0 && nr_free_pages() == free_before);
    }
    pde_t original_root = root[0];
    free_before = nr_free_pages();
    allocation_budget = 0;
    pmm_manager = &fault_manager;
    int result = page_insert(root, target, 0x200000, PTE_R);
    pmm_manager = real_manager;
    assert(result == -E_NO_MEM && root[0] == original_root && page_ref(target) == 0);
    assert(nr_free_pages() == free_before);
    free_page(target);
    cprintf("Lab4 extra: first/second table allocation failure and existing-parent preservation PASS\n");
    for (size_t i = 0; i < 511; ++i) {
        if (!(root[i] & PTE_V)) continue;
        pde_t *middle = page2kva(pde2page(root[i]));
        for (size_t j = 0; j < 512; ++j) {
            if (middle[j] & PTE_V) free_page(pde2page(middle[j]));
        }
        free_page(pde2page(root[i]));
    }
    free_page(root_page);
    assert(nr_free_pages() == initial_free);
    local_intr_restore(intr_flag);
}
