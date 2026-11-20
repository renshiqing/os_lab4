/*
 * pmm.c —— uCore Lab4 物理内存管理与页表操作实现（中文注释版）
 *
 * 说明：
 * 1. 本文件在用户提供的原始 pmm.c 基础上补充中文注释。
 * 2. 保留原有函数、变量、宏调用、控制流程和执行逻辑，不主动修改实现。
 * 3. 重点解释 DTB 物理内存识别、Page 描述符初始化、三级页表查找、
 *    页面映射/解除映射、引用计数维护、TLB 失效以及自检流程。
 * 4. 行尾注释用于说明该行代码的作用；较复杂的算法另有段落说明。
 *
 * Sv39 地址转换基本过程：
 *   虚拟地址 -> PDX1 -> 一级页表 -> PDX0 -> 二级页表
 *             -> PTX -> 三级页表 -> 物理页 + 页内偏移
 *
 * 重要约定：
 *   PGSIZE = 4 KiB；每级页表索引宽度为 9 位；每张页表有 512 项。
 *   Page 引用计数表示当前有多少映射引用该物理页面。
 */

#include <default_pmm.h> // 默认物理内存管理器的接口及定义。
#include <defs.h> // 内核基础类型、断言和底层定义。
#include <error.h> // 错误码定义，例如内存不足时返回的错误码。
#include <kmalloc.h> // 内核动态内存分配器 kmalloc 的初始化接口。
#include <memlayout.h> // 物理内存布局、内核地址空间等宏定义。
#include <mmu.h> // MMU、页大小、页表索引和 PTE 标志位定义。
#include <pmm.h> // 物理页管理器、Page 结构及页表操作接口。
#include <sbi.h> // 通过 SBI 与 OpenSBI 固件交互的接口。
#include <stdio.h> // 内核格式化输出接口，例如 cprintf。
#include <string.h> // 内存和字符串操作函数，例如 memset、strcmp。
#include <sync.h> // 中断保护及同步操作接口。
#include <vmm.h> // 虚拟内存管理相关接口。
#include <riscv.h> // RISC-V CSR 寄存器读写等体系结构接口。
#include <dtb.h> // 设备树解析接口，用于读取 DTB 中的物理内存范围。

// virtual address of physical page array；pages 保存物理页描述符数组的内核虚拟地址。
struct Page *pages; // 指向物理页描述符数组；每个物理页对应一个 struct Page。
// amount of physical memory (in pages)；npage 表示可管理物理地址范围内的总页数。
size_t npage = 0; // 初始化为 0，随后由 page_init 根据物理内存上限计算。
// The kernel image is mapped at VA=KERNBASE and PA=info.base；内核虚拟地址与物理地址之间的映射关系说明。
uint_t va_pa_offset; // va_pa_offset 保存内核虚拟地址到物理地址的偏移量。
// memory starts at 0x80000000 in RISC-V；RISC-V virt 平台的 DRAM 起始物理地址通常为 0x80000000。
const size_t nbase = DRAM_BASE / PGSIZE; // nbase 是 DRAM 起始物理地址对应的物理页号；低于该页号的地址不属于本平台 RAM。

// virtual address of boot-time page directory；启动阶段页目录的虚拟地址。
pde_t *boot_pgdir_va = NULL; // boot_pgdir_va 指向启动时使用的根页表（Sv39 一级页表）。
// physical address of boot-time page directory；启动阶段页目录的物理地址。
uintptr_t boot_pgdir_pa; // boot_pgdir_pa 用于写入 satp，启用分页地址转换。

// physical memory management；物理内存管理器接口说明。
const struct pmm_manager *pmm_manager; // pmm_manager 指向当前选用的物理页管理器实现，例如 default_pmm_manager。

static void check_alloc_page(void); // 声明物理页分配/释放自检函数。
static void check_pgdir(void); // 声明页目录和页表映射关系自检函数。
static void check_boot_pgdir(void); // 声明启动页表映射自检函数。

// init_pmm_manager - initialize a pmm_manager instance；初始化物理内存管理器：选择具体实现并调用其初始化方法。
static void init_pmm_manager(void)
{
    pmm_manager = &default_pmm_manager; // 选择默认物理内存管理器；若实验要求 best-fit，则由对应构建配置选择相应实现。
    cprintf("memory management: %s\n", pmm_manager->name); // 打印当前使用的物理内存管理器名称，便于确认实际启用的算法。
    pmm_manager->init(); // 调用管理器的 init 回调，初始化空闲链表、空闲块等内部数据结构。
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory；把一段连续物理页交给当前管理器，建立可分配的空闲内存区域。
static void init_memmap(struct Page *base, size_t n) // base 为连续物理页的起始描述符，n 为页数。
{
    pmm_manager->init_memmap(base, n); // 调用具体管理器的 init_memmap 回调，将这段页面纳入空闲内存管理。
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE；分配连续 n 个物理页面；成功返回首个页面描述符，失败返回 NULL。
// memory；申请的物理内存大小为 n * PGSIZE。
struct Page *alloc_pages(size_t n) // 分配连续物理页面的统一接口。
{
    struct Page *page = NULL; // 先将结果初始化为 NULL，避免分配失败时返回未定义值。
    bool intr_flag; // 保存进入临界区前的中断使能状态，以便结束时恢复。
    local_intr_save(intr_flag); // 暂时关闭本地中断，避免分配器内部链表操作被中断打断。
    {
        page = pmm_manager->alloc_pages(n); // 调用具体物理内存管理器分配 n 个连续页面。
    }
    local_intr_restore(intr_flag); // 恢复进入临界区前的中断状态，而不是无条件开启中断。
    return page; // 返回分配结果；调用者需要检查是否为 NULL。
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory；释放一段连续物理页面，并将其重新交给物理内存管理器。
void free_pages(struct Page *base, size_t n) // base 指向待释放区域的首个 Page 描述符，n 表示页面数量。
{
    bool intr_flag; // 记录原有中断状态，供释放操作完成后恢复。
    local_intr_save(intr_flag); // 关闭本地中断，保护释放操作中的空闲链表修改。
    {
        pmm_manager->free_pages(base, n); // 调用当前管理器的 free_pages 回调，回收连续页面。
    }
    local_intr_restore(intr_flag); // 恢复此前的中断状态。
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)；查询当前空闲物理页面数量的统一接口。
// of current free memory；返回值单位为页；若要换算字节数，应乘以 PGSIZE。
size_t nr_free_pages(void) // 统计当前空闲页面数。
{
    size_t ret; // 保存管理器返回的空闲页数量。
    bool intr_flag; // 保存中断状态，保证查询过程与管理器数据结构访问一致。
    local_intr_save(intr_flag); // 关闭本地中断，避免读取空闲链表时发生并发修改。
    {
        ret = pmm_manager->nr_free_pages(); // 调用具体管理器的 nr_free_pages 回调获取空闲页数。
    }
    local_intr_restore(intr_flag); // 恢复原来的中断状态。
    return ret; // 返回空闲页面数量。
}

//lab2 2412396；此处保留原文件中的实验标记，不影响程序执行。
static void page_init(void) { // 根据 DTB 提供的物理内存范围初始化 Page 描述符和空闲页区域。
    extern char end[]; // end 是内核镜像结束位置，由链接脚本提供；其后可安排 Page 描述符数组。
    uint64_t base = get_memory_base(), size = get_memory_size(); // 从设备树解析结果中取得 DRAM 起始物理地址 base 和内存大小 size。
    // nbase and the bootstrap mapping are fixed by this board's framework.；本实验的启动映射以 DRAM_BASE 为起点，因此需要校验 DTB 数据与平台布局一致。
    if (base != DRAM_BASE || size == 0 || size > ~(uint64_t)0 - base) { // 检查内存起点、内存大小以及 base + size 是否会发生 uint64_t 溢出。
        panic("invalid DTB memory range"); // 若设备树中的物理内存范围不合法，立即 panic，避免后续越界访问。
    }
    uint64_t mem_end = base + size; // 计算物理内存区域的结束地址（采用开区间表示，即 mem_end 不属于 RAM）。
    cprintf("physical memory map:\n  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n", // 输出检测到的物理内存范围，便于核对 DTB 解析结果。
            size, base, mem_end - 1); // 分别传入内存大小、起始地址和最后一个有效物理地址 mem_end - 1。
    uint64_t maxpa = mem_end < (KERNTOP - PHYSICAL_MEMORY_OFFSET) ? mem_end : (KERNTOP - PHYSICAL_MEMORY_OFFSET); // 将可管理物理内存上限限制在实际 RAM 末端与内核映射允许上限两者中的较小值。
    maxpa = ROUNDDOWN(maxpa, PGSIZE); // 将物理地址上限向下对齐到 PGSIZE，确保只管理完整的 4 KiB 页面。
    va_pa_offset = PHYSICAL_MEMORY_OFFSET; // 保存内核虚拟地址与物理地址之间的偏移，供 PADDR/KADDR 等转换宏使用。
    npage = PPN(maxpa); // 把最大物理地址转换为页号，得到物理地址空间的页面上界。
    if (npage <= nbase) panic("no manageable physical pages"); // 若总页数不超过 DRAM 起始页号，说明没有可管理的 RAM 页面，终止启动。
    uintptr_t desc_va = ROUNDUP((uintptr_t)end, PGSIZE); // 将内核镜像末尾地址向上对齐到页面边界，作为 Page 描述符数组的虚拟起始地址。
    uintptr_t desc_pa = PADDR(desc_va); // 把描述符数组的虚拟地址转换为对应物理地址。
    size_t desc_bytes = (npage - nbase) * sizeof(struct Page); // 计算描述符数组所需字节数：可管理页面数乘以每个 Page 结构的大小。
    // Check before touching the descriptor array, including tiny-RAM cases.；在访问描述符数组之前先做边界检查，避免小内存配置下越界。
    if (desc_pa < (KERNBASE - PHYSICAL_MEMORY_OFFSET) || desc_pa > maxpa || desc_bytes > maxpa - desc_pa) { // 检查描述符数组物理起点及长度是否完全落在可管理 RAM 范围内，同时防止减法/加法边界错误。
        panic("not enough RAM for Page descriptors"); // 描述符数组放不进可用 RAM 时停止启动，防止覆盖无效内存。
    }
    pages = (struct Page *)desc_va; // 把描述符数组的内核虚拟地址保存到全局 pages 指针。
    for (size_t i = 0; i < npage - nbase; ++i) { // 逐个初始化 DRAM 起始页之后的所有物理页描述符。
        pages[i].flags = 0; // 清空页面标志位，避免继承未初始化的状态。
        SetPageReserved(&pages[i]); // 先将页面标记为保留页；只有后续明确加入空闲管理器的区域才会成为可分配页面。
        pages[i].property = 0; // 清零连续空闲块管理所使用的 property 字段。
        set_page_ref(&pages[i], 0); // 将页面引用计数初始化为 0。
        list_init(&pages[i].page_link); // 初始化普通页面链表节点，供物理内存管理器组织空闲页或页面集合。
        list_init(&pages[i].pra_page_link); // 初始化页面置换相关链表节点，供启用页面置换机制时使用。
        pages[i].pra_vaddr = 0; // 清空页面置换算法记录的虚拟地址。
    }
    uintptr_t freemem = ROUNDUP(desc_pa + desc_bytes, PGSIZE); // 计算描述符数组末尾物理地址，并向上对齐到页面边界，得到可用于分配的内存起点。
    if (freemem < maxpa) { // 只有当描述符数组之后仍有完整可用物理内存时，才建立空闲页区域。
        init_memmap(pa2page(freemem), (maxpa - freemem) / PGSIZE); // 将 freemem 对应的物理页描述符作为起点，把剩余完整页面交给物理内存管理器。
    }
    cprintf("Page descriptors: VA=0x%016lx bytes=%lu; free start=0x%016lx pages=%lu\n", // 打印 Page 描述符数组的虚拟地址、占用字节数、空闲内存起点及当前空闲页数。
            desc_va, desc_bytes, freemem, nr_free_pages()); // 输出对应的四个参数，便于检查初始化后的内存布局。
}




static void enable_paging(void) // 将启动页目录的物理页号写入 satp，从而启用 Sv39 地址转换。
{
    write_csr(satp, 0x8000000000000000 | (boot_pgdir_pa >> RISCV_PGSHIFT)); // 设置 satp.MODE=8（Sv39），并将根页表物理地址右移 RISCV_PGSHIFT 得到 PPN。
}

// boot_map_segment - setup&enable the paging mechanism；建立一段连续的虚拟地址到物理地址映射，通常用于启动阶段映射内核区。
// parameters；以下参数描述需要建立的映射范围及其访问权限。
//  la:   linear address of this memory need to map (after x86 segment map)；la 是待映射区域的起始虚拟地址。
//  size: memory size；size 是需要映射的字节数。
//  pa:   physical address of this memory；pa 是待映射区域对应的起始物理地址。
//  perm: permission of this memory；perm 是映射权限，例如可读、可写、可执行或用户态访问权限。
static void boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size, // pgdir 指向 Sv39 根页表；la、size、pa、perm 描述映射关系。
                             uintptr_t pa, uint32_t perm) // perm 与上一行共同组成函数参数列表。
{
    assert(PGOFF(la) == PGOFF(pa)); // 要求虚拟地址和物理地址的页内偏移相同，否则无法按整页建立连续映射。
    size_t n = ROUNDUP(size + PGOFF(la), PGSIZE) / PGSIZE; // 计算覆盖该区域所需的页面数量，先把起始页内偏移计入总长度再向上取整。
    la = ROUNDDOWN(la, PGSIZE); // 将虚拟地址向下对齐到页面起始位置。
    pa = ROUNDDOWN(pa, PGSIZE); // 将物理地址向下对齐到页面起始位置。
    for (; n > 0; n--, la += PGSIZE, pa += PGSIZE) // 逐页建立映射；每轮处理一页，虚拟地址和物理地址同步增加 PGSIZE。
    {
        pte_t *ptep = get_pte(pgdir, la, 1); // 查找 la 对应的 PTE；第三个参数为 1，表示缺少中间页表时允许分配。
        assert(ptep != NULL); // 确保 get_pte 成功返回 PTE 指针；失败说明页表分配异常。
        *ptep = pte_create(pa >> PGSHIFT, PTE_V | perm); // 将物理页号和有效位、访问权限组合成叶子页表项，完成一页映射。
    }
}

// boot_alloc_page - allocate one page using pmm->alloc_pages(1)；从物理内存管理器申请一个页面，专门用于启动阶段页目录或页表分配。
// return value: the kernel virtual address of this allocated page；成功返回该物理页面对应的内核虚拟地址。
// note: this function is used to get the memory for PDT(Page Directory；PDT 是页目录表，PT 是页表；Sv39 需要多级页表结构。
// Table)&PT(Page Table)；该函数将新分配的页面转换成内核可访问的地址。
static void *boot_alloc_page(void) // 申请一个物理页面并返回其内核虚拟地址。
{
    struct Page *p = alloc_page(); // 申请一个页面描述符；alloc_page 通常是 alloc_pages(1) 的封装。
    if (p == NULL) // 检查分配是否成功。
    {
        panic("boot_alloc_page failed.\n"); // 没有可用物理页面时终止启动，避免返回无效地址。
    }
    return page2kva(p); // 把 Page 描述符转换成该页面的内核虚拟地址并返回。
}

// pmm_init - setup a pmm to manage physical memory, build PDT&PT to setup；物理内存管理初始化总入口：初始化管理器、识别内存、建立页表并执行检查。
// paging mechanism；分页机制依赖物理页分配器提供页目录和页表所需的页面。
//         - check the correctness of pmm & paging mechanism, print PDT&PT；初始化后还要通过测试函数检查物理页管理和虚拟地址映射。
void pmm_init(void) // 初始化整个物理内存管理子系统。
{
    // We need to alloc/free the physical memory (granularity is 4KB or other；物理内存分配/释放通常以 4 KiB 页面为基本粒度。
    // size).；某些管理器也可以在多个连续页面的粒度上组织空闲区域。
    // So a framework of physical memory manager (struct pmm_manager)is defined；pmm.h 定义统一的物理内存管理器接口结构体。
    // in pmm.h；不同分配算法通过相同接口向上层提供服务。
    // First we should init a physical memory manager(pmm) based on the；首先选择并初始化一个具体的物理内存管理器实现。
    // framework.；管理器初始化后才能安全地执行分配和释放操作。
    // Then pmm can alloc/free the physical memory.；初始化完成后，pmm_manager 可以统一管理物理页面。
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.；框架可支持 first-fit、best-fit、worst-fit 或 buddy 等不同策略。
    init_pmm_manager(); // 初始化所选物理页管理器的内部数据结构。

    // detect physical memory space, reserve already used memory,；识别物理内存范围，并将内核镜像、页描述符等已占用区域排除在空闲区之外。
    // then use pmm->init_memmap to create free page list；剩余完整页面通过 init_memmap 加入空闲页面管理结构。
    page_init(); // 根据 DTB 中的内存范围初始化 Page 描述符和空闲页列表。

    // use pmm->check to verify the correctness of the alloc/free function in a；调用具体管理器自带的检查程序，验证分配和释放行为。
    // pmm；检查内容通常包括分配成功、释放后空闲页数恢复等。
    check_alloc_page(); // 执行物理页面分配器的自检。
    // create boot_pgdir, an initial page directory(Page Directory Table, PDT)；建立启动阶段使用的根页表指针。
    extern char boot_page_table_sv39[]; // 链接器/启动汇编中定义的 Sv39 初始页表符号。
    boot_pgdir_va = (pte_t *)boot_page_table_sv39; // 将初始页表符号地址转换为页表指针，作为根页表虚拟地址。
    boot_pgdir_pa = PADDR(boot_pgdir_va); // 把根页表虚拟地址转换成物理地址，供 satp 使用。

    check_pgdir(); // 检查根页表、映射和页引用计数等基础结构是否正确。

    static_assert(KERNBASE % PTSIZE == 0 && KERNTOP % PTSIZE == 0); // 编译期检查内核映射起点和终点均按一个页表覆盖范围对齐。

    // now the basic virtual memory map(see memalyout.h) is established.；此时启动汇编已提供基础内核虚拟地址映射。
    // check the correctness of the basic virtual memory map.；下面通过测试验证内核映射是否能正确完成 VA 到 PA 转换。
    check_boot_pgdir(); // 检查启动阶段页表映射的正确性。

    kmalloc_init(); // 初始化内核动态内存分配器，使 kmalloc/kfree 可以使用物理页管理能力。
}

// get_pte - get pte and return the kernel virtual address of this pte for la；根据虚拟地址获取对应的 PTE 指针；必要时创建中间级页表。
//        - if the PT contians this pte didn't exist, alloc a page for PT；若所需的中间页表不存在且 create 为真，则申请物理页作为页表。
// parameter:；参数说明如下。
//  pgdir:  the kernel virtual base address of PDT；pgdir 是根页表的内核虚拟地址。
//  la:     the linear address need to map；la 是需要查询或建立映射的虚拟地址。
//  create: a logical value to decide if alloc a page for PT；create 为真时允许分配缺失的中间页表；为假时只查询、不分配。
// return vaule: the kernel virtual address of this pte；返回目标 PTE 的内核虚拟地址；失败或不创建时不存在则返回 NULL。
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create) // 沿 Sv39 三级页表路径查找 la 对应的最终页表项。
{
    pde_t *pdep1 = &pgdir[PDX1(la)]; // 利用虚拟地址的最高一级索引 PDX1，定位根页表中的一级页表项。
    if (!(*pdep1 & PTE_V)) // 检查一级页表项的 V 位；V=0 表示下一级页表尚未建立。
    {
        struct Page *page; // 声明用于承载新分配页表页面的 Page 描述符指针。
        if (!create || (page = alloc_page()) == NULL) // 如果不允许创建，或物理页分配失败，则无法继续查找。
        {
            return NULL; // 返回 NULL，表示目标 PTE 不存在且无法创建所需页表。
        }
        set_page_ref(page, 1); // 新分配的页面将作为下一级页表，引用计数设为 1。
        uintptr_t pa = page2pa(page); // 取得新分配页面的物理地址。
        memset(KADDR(pa), 0, PGSIZE); // 通过 KADDR 将物理地址转换为内核虚拟地址，并清零整个页表页，确保所有 PTE 初始无效。
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V); // 在一级页表项中写入新页表页的 PPN，并设置有效标志；此处沿用原代码的 PTE_U | PTE_V。
    }
    pde_t *pdep0 = &((pte_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)]; // 从一级页表项提取二级页表物理地址，经 KADDR 转为内核虚拟地址，再用 PDX0 定位二级页表项。
    if (!(*pdep0 & PTE_V)) // 检查二级页表项是否有效；无效说明对应的三级页表尚未建立。
    {
        struct Page *page; // 声明用于承载新分配三级页表页面的 Page 描述符指针。
        if (!create || (page = alloc_page()) == NULL) // 若 create 为假，或申请三级页表页面失败，则不能继续。
        {
            return NULL; // 返回 NULL，表示当前地址对应的三级页表不存在且无法创建。
        }
        set_page_ref(page, 1); // 把新分配的三级页表页面引用计数设为 1。
        uintptr_t pa = page2pa(page); // 获取新三级页表页面的物理地址。
        memset(KADDR(pa), 0, PGSIZE); // 将新三级页表页面清零，避免旧数据被误认为有效页表项。
        *pdep0 = pte_create(page2ppn(page), PTE_U | PTE_V); // 把三级页表物理页号写入二级页表项，并设置有效标志。
    }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)]; // 提取三级页表地址，再以 PTX(la) 作为索引，返回最终叶子 PTE 的地址。
}

// get_page - get related Page struct for linear address la using PDT pgdir；根据虚拟地址获取其映射的物理页面描述符。
struct Page *get_page(pde_t *pgdir, uintptr_t la, pte_t **ptep_store) // pgdir 是根页表，la 是虚拟地址，ptep_store 可选地接收 PTE 指针。
{
    pte_t *ptep = get_pte(pgdir, la, 0); // 调用 get_pte 且 create=0，只查找已有页表，不分配新页表。
    if (ptep_store != NULL) // 调用者若提供 ptep_store，就把查找到的 PTE 地址返回给调用者。
    {
        *ptep_store = ptep; // 保存 PTE 指针；即使其内容无效，也可以让调用者检查该表项。
    }
    if (ptep != NULL && *ptep & PTE_V) // 只有 PTE 指针存在且 V 位有效时，才能解析出其对应的物理页面。
    {
        return pte2page(*ptep); // 将 PTE 中的物理页号转换为 struct Page 指针并返回。
    }
    return NULL; // 没有有效映射时返回 NULL。
}

//LAB4 function1 2412396；Lab4 核心：删除一个虚拟地址对应的页表映射，并维护页面引用计数。
static void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep) // pgdir 是根页表，la 是待解除映射的虚拟地址，ptep 是对应 PTE 指针。
{
    if (*ptep & PTE_V) { // 只有有效 PTE 才需要执行解除映射和页面回收操作。
        struct Page *page = pte2page(*ptep); // 根据有效 PTE 找到它映射的物理页面描述符。
        assert(page_ref(page) > 0); // 断言引用计数大于 0，防止对未被引用的页面执行递减。
        *ptep = 0; // 清空 PTE，立即从页表中删除该虚拟地址的映射。
        tlb_invalidate(pgdir, la); // 使该虚拟地址对应的 TLB 项失效，避免 CPU 继续使用旧的地址转换缓存。
        if (page_ref_dec(page) == 0) free_page(page); // 引用计数减 1；当计数变为 0 时，说明没有其他映射引用该页面，可以释放物理页。
    }
}

void page_remove(pde_t *pgdir, uintptr_t la) // 对外提供按虚拟地址删除映射的接口。
{
    pte_t *ptep = get_pte(pgdir, la, 0); // 只查询已有页表，不因为删除操作而创建新的页表。
    if (ptep != NULL) page_remove_pte(pgdir, la, ptep); // 若找到 PTE，就调用内部函数完成清表、TLB 失效和引用计数维护。
}

int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm) // Lab4 核心：将一个物理页面映射到指定虚拟地址，并设置访问权限。
{
    pde_t *root_entry = &pgdir[PDX1(la)]; // 定位虚拟地址对应的根页表项，后续用于分配失败时判断是否需要回滚。
    pde_t old_root = *root_entry; // 保存原一级页表项，以区分本次调用前已有的页表路径和本次新建的路径。
    pte_t *ptep = get_pte(pgdir, la, 1); // 获取目标 PTE；create=1 表示必要时分配缺失的中间页表。
    if (ptep == NULL) { // 如果无法获得目标 PTE，说明中间页表分配失败。
        // get_pte may have created one table before its second allocation fails.；get_pte 可能先成功创建一级下级页表，再在创建更低一级页表时失败。
        // Only undo a table this invocation owns; preserve pre-existing paths.；回滚时只能释放本次调用新建且由本次调用拥有的页表，不能破坏原有页表路径。
        if (!(old_root & PTE_V) && (*root_entry & PTE_V)) { // 若原根页表项无效而现在有效，说明本次调用新建了该一级下级页表。
            struct Page *table = pde2page(*root_entry); // 根据新根页表项找到刚刚分配的页表页面。
            *root_entry = old_root; // 恢复原来的根页表项，撤销本次创建的页表路径。
            tlb_invalidate(pgdir, la); // 使相关虚拟地址的 TLB 转换失效，防止使用被撤销的页表路径。
            set_page_ref(table, 0); // 将回滚页表页面的引用计数清零，匹配其即将释放的状态。
            free_page(table); // 释放本次调用新建的页表页面，避免分配失败造成内存泄漏。
        }
        return -E_NO_MEM; // 返回内存不足错误码，调用者据此处理映射失败。
    }
    page_ref_inc(page); // 先增加新映射页面的引用计数，表示即将建立一个新的映射引用。
    if (*ptep & PTE_V) { // 若目标虚拟地址原来已有有效映射，需要处理旧映射。
        struct Page *old_page = pte2page(*ptep); // 从旧 PTE 中取得原来映射的物理页面。
        if (old_page == page) page_ref_dec(page); // 如果新旧映射指向同一个物理页，撤销刚才多加的引用，避免重复映射时引用计数虚增。
        else page_remove_pte(pgdir, la, ptep); // 如果新旧物理页不同，先删除旧映射，并递减旧页面引用计数；必要时释放旧页。
    }
    *ptep = pte_create(page2ppn(page), perm); // 写入新的叶子 PTE：包含新物理页的页号以及调用者指定的权限。
    tlb_invalidate(pgdir, la); // 刷新该虚拟地址对应的 TLB 项，使后续访问使用新映射和新权限。
    return 0; // 返回 0，表示页面映射成功。
}








// invalidate a TLB entry, but only if the page tables being；TLB（地址转换后备缓冲器）保存虚拟地址到物理地址的缓存结果。
// edited are the ones currently in use by the processor.；修改当前处理器正在使用的页表映射后，需要使旧缓存失效。
void tlb_invalidate(pde_t *pgdir, uintptr_t la) // 按虚拟地址 la 使地址转换缓存失效；pgdir 参数保留用于接口兼容/标识调用上下文。
{
    // flush_tlb();；整表刷新方案（当前未启用），会清空更大范围的 TLB 缓存。
    // The flush_tlb flush the entire TLB, is there any better way?；逐地址失效通常比无条件刷新整个 TLB 更精细。
    asm volatile("sfence.vma %0" : : "r"(la)); // 执行 RISC-V sfence.vma：rs1 指定虚拟地址，rs2 未指定时按架构规则处理 ASID 范围。
}

static void check_alloc_page(void) // 检查当前物理内存管理器的分配与释放功能。
{
    pmm_manager->check(); // 调用具体物理页管理器的 check 回调执行测试。
    cprintf("check_alloc_page() succeeded!\n"); // 输出自检成功信息；只有断言均通过才会执行到这里。
}

static void check_pgdir(void) // 测试 get_pte、page_insert、page_remove 以及引用计数维护。
{
    // assert(npage <= KMEMSIZE / PGSIZE);
    // The memory starts at 2GB in RISC-V
    // so npage is always larger than KMEMSIZE / PGSIZE
    size_t nr_free_store; // 保存测试开始前的空闲页面数量，测试结束后比较是否恢复。

    nr_free_store = nr_free_pages(); // 记录测试前的空闲页数，用于检查是否发生页面泄漏。

    assert(npage <= KERNTOP / PGSIZE); // 检查物理页总数没有超过内核允许管理的物理地址上限。
    assert(boot_pgdir_va != NULL && (uint32_t)PGOFF(boot_pgdir_va) == 0); // 检查根页表指针存在且按页面大小对齐。
    assert(get_page(boot_pgdir_va, 0x0, NULL) == NULL); // 检查虚拟地址 0x0 初始没有有效物理页映射。

    struct Page *p1, *p2; // 声明两个测试用物理页面描述符。
    p1 = alloc_page(); // 分配第一个测试页面。
    assert(page_insert(boot_pgdir_va, p1, 0x0, 0) == 0); // 把 p1 映射到虚拟地址 0x0，权限参数为 0。

    pte_t *ptep; // 声明用于保存目标 PTE 地址的指针。
    assert((ptep = get_pte(boot_pgdir_va, 0x0, 0)) != NULL); // 查询 0x0 对应的 PTE，断言页表路径已经建立。
    assert(pte2page(*ptep) == p1); // 检查 PTE 指向的物理页确实是 p1。
    assert(page_ref(p1) == 1); // 检查 p1 当前只有一个映射引用。

    ptep = (pte_t *)KADDR(PDE_ADDR(boot_pgdir_va[0])); // 从根页表第 0 项提取二级页目录的物理地址，并转换为内核虚拟地址。
    ptep = (pte_t *)KADDR(PDE_ADDR(ptep[0])) + 1; // 从二级页目录第 0 项提取三级页表地址，再定位其中索引为 1 的 PTE。
    assert(get_pte(boot_pgdir_va, PGSIZE, 0) == ptep); // 验证虚拟地址 PGSIZE（下一页）对应的 PTE 指针与手动计算结果一致。

    p2 = alloc_page(); // 分配第二个测试物理页面 p2。
    assert(page_insert(boot_pgdir_va, p2, PGSIZE, PTE_U | PTE_W) == 0); // 把 p2 映射到虚拟地址 PGSIZE，并设置用户态可访问、可写权限。
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL); // 重新查询该虚拟地址的 PTE，确认映射存在。
    assert(*ptep & PTE_U); // 检查新 PTE 中设置了 PTE_U 用户态标志。
    assert(*ptep & PTE_W); // 检查新 PTE 中设置了 PTE_W 写权限。
    assert(boot_pgdir_va[0] & PTE_U); // 检查中间级页表项也带有原代码要求的 PTE_U 标志。
    assert(page_ref(p2) == 1); // 检查 p2 的引用计数为 1。

    assert(page_insert(boot_pgdir_va, p1, PGSIZE, 0) == 0); // 把同一个物理页 p1 映射到 PGSIZE，替换原来 p2 的映射。
    assert(page_ref(p1) == 2); // p1 此时同时映射到 0x0 和 PGSIZE，因此引用计数应为 2。
    assert(page_ref(p2) == 0); // p2 的旧映射已被移除，引用计数应降为 0。
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL); // 重新获取 PGSIZE 对应的 PTE，确认新映射仍然存在。
    assert(pte2page(*ptep) == p1); // 检查新 PTE 已经指向 p1，而不是旧页面 p2。
    assert((*ptep & PTE_U) == 0); // 权限参数为 0，因此新 PTE 不应包含 PTE_U。

    page_remove(boot_pgdir_va, 0x0); // 删除虚拟地址 0x0 对 p1 的映射。
    assert(page_ref(p1) == 1); // p1 还映射在 PGSIZE，因此引用计数应剩余 1。
    assert(page_ref(p2) == 0); // p2 已无任何映射，引用计数保持为 0。

    page_remove(boot_pgdir_va, PGSIZE); // 删除 PGSIZE 对 p1 的最后一个映射。
    assert(page_ref(p1) == 0); // p1 的引用计数变为 0，内部删除函数应已释放该物理页。
    assert(page_ref(p2) == 0); // 再次确认 p2 的引用计数为 0。

    assert(page_ref(pde2page(boot_pgdir_va[0])) == 1); // 检查根页表下级页表页面的引用计数仍为 1，便于下面手动释放测试页表。

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0])); // 取得一级页目录和二级页目录的指针，准备释放测试期间动态创建的页表页面。
    free_page(pde2page(pd0[0])); // 释放三级页表页面。
    free_page(pde2page(pd1[0])); // 释放二级页目录页面。
    boot_pgdir_va[0] = 0; // 清除根页表第 0 项，撤销测试创建的页表路径。
    flush_tlb(); // 刷新整个 TLB，清除测试映射留下的地址转换缓存。

    assert(nr_free_store == nr_free_pages()); // 断言测试结束后的空闲页数量与开始时一致，验证没有物理页面泄漏。

    cprintf("check_pgdir() succeeded!\n"); // 输出页表操作测试成功信息。
}

static void check_boot_pgdir(void) // 检查启动时建立的内核地址映射及页面映射的基本功能。
{
    size_t nr_free_store; // 保存测试前空闲页数量。
    pte_t *ptep; // 保存遍历过程中查询到的 PTE 指针。
    int i; // 循环索引，用于逐页检查内核映射。

    nr_free_store = nr_free_pages(); // 记录测试前的空闲页面数量。

    for (i = ROUNDDOWN(KERNBASE, PGSIZE); i < npage * PGSIZE; i += PGSIZE) // 从内核映射对应的起始页开始，按页面大小遍历需要验证的物理页范围。
    {
        assert((ptep = get_pte(boot_pgdir_va, (uintptr_t)KADDR(i), 0)) != NULL); // 把物理页号对应的内核虚拟地址交给 get_pte，检查启动页表中是否已有映射。
        assert(PTE_ADDR(*ptep) == i); // 验证 PTE 中提取出的物理页地址与当前遍历的物理地址一致。
    }

    assert(boot_pgdir_va[0] == 0); // 检查根页表低地址区域没有遗留映射。

    struct Page *p; // 声明用于测试映射的物理页面。
    p = alloc_page(); // 分配一个测试物理页面。
    assert(page_insert(boot_pgdir_va, p, 0x100, PTE_W | PTE_R) == 0); // 把同一个物理页映射到虚拟地址 0x100，权限为可读、可写。
    assert(page_ref(p) == 1); // 首次映射后，该页面引用计数应为 1。
    assert(page_insert(boot_pgdir_va, p, 0x100 + PGSIZE, PTE_W | PTE_R) == 0); // 再把同一个物理页映射到相隔一页的虚拟地址，验证一个物理页可以有多个虚拟映射。
    assert(page_ref(p) == 2); // 建立第二个映射后，页面引用计数应为 2。

    const char *str = "ucore: Hello world!!"; // 准备一段测试字符串，用于验证两个虚拟地址是否指向同一物理页。
    strcpy((void *)0x100, str); // 通过第一个虚拟地址写入字符串。
    assert(strcmp((void *)0x100, (void *)(0x100 + PGSIZE)) == 0); // 通过第二个虚拟地址读取并比较字符串；相等说明两个虚拟地址共享同一物理页面。

    *(char *)(page2kva(p) + 0x100) = '\0'; // 通过物理页面的内核虚拟地址修改偏移 0x100 处的字节为字符串结束符。
    assert(strlen((const char *)0x100) == 0); // 再从虚拟地址 0x100 读取字符串长度，应为 0，验证虚拟映射与物理页别名关系正确。

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0])); // 重新取得一级页目录和二级页目录指针，准备释放本测试建立的页表页面。
    free_page(p); // 释放测试用物理数据页面。
    free_page(pde2page(pd0[0])); // 释放本测试创建的三级页表页面。
    free_page(pde2page(pd1[0])); // 释放本测试创建的二级页目录页面。
    boot_pgdir_va[0] = 0; // 清空根页表对应项，移除测试期间创建的低地址映射路径。
    flush_tlb(); // 刷新 TLB，确保测试映射不再被处理器缓存。

    assert(nr_free_store == nr_free_pages()); // 检查测试结束后空闲页面数量恢复到初始值，确认没有泄漏。

    cprintf("check_boot_pgdir() succeeded!\n"); // 输出启动页表映射测试成功信息。
}

// perm2str - use string 'u,r,w,-' to present the permission；把权限位转换成便于打印的字符串形式。
static const char *perm2str(int perm) // 输入 perm 是页表项权限位，返回静态字符串。
{
    static char str[4]; // 静态缓冲区保存三个字符和结尾的字符串终止符。
    str[0] = (perm & PTE_U) ? 'u' : '-'; // 根据 PTE_U 决定第一个字符：用户态权限存在显示 u，否则显示 -。
    str[1] = 'r'; // 第二个字符固定显示 r；原实现不在此处区分读权限。
    str[2] = (perm & PTE_W) ? 'w' : '-'; // 根据 PTE_W 决定第三个字符：可写显示 w，否则显示 -。
    str[3] = '\0'; // 写入字符串结束符，保证 str 是合法 C 字符串。
    return str; // 返回静态缓冲区；下一次调用会覆盖本次结果。
}

// get_pgtable_items - In [left, right] range of PDT or PT, find a continuous；在页目录或页表指定索引范围内查找连续、有效且权限相同的表项区间。
// linear addr space；返回区间由 left_store 和 right_store 描述，可用于打印映射范围。
//                  - (left_store*X_SIZE~right_store*X_SIZE) for PDT or PT；区间边界乘以对应层级的覆盖大小即可换算为虚拟地址范围。
//                  - X_SIZE=PTSIZE=4M, if PDT; X_SIZE=PGSIZE=4K, if PT；注意原注释中的 4M 与当前 Sv39 基础页表结构不完全一致；本文件的 PTSIZE 定义为 2 MiB。
// paramemters:；参数说明如下。
//  left:        no use ???；left 参数在当前函数实现中没有被使用，保留在接口中。
//  right:       the high side of table's range；right 是本次扫描允许到达的右边界（不包含该索引）。
//  start:       the low side of table's range；start 是本次扫描的起始表项索引。
//  table:       the beginning addr of table；table 指向当前页目录或页表的表项数组。
//  left_store:  the pointer of the high side of table's next range；left_store 用于返回找到的连续有效区间的起始索引。
//  right_store: the pointer of the low side of table's next range；right_store 用于返回连续有效区间结束位置（右边界不包含）。
//  return value: 0 - not a invalid item range, perm - a valid item range with；没有找到有效区间时返回 0；找到时返回该区间的权限掩码。
//  perm permission；返回的权限掩码由 PTE_USER 过滤得到。
static int get_pgtable_items(size_t left, size_t right, size_t start, // 扫描 table[start:right]，查找第一个有效项及其后续同权限连续区间。
                             uintptr_t *table, size_t *left_store, // table 为待扫描的表项数组。
                             size_t *right_store) // left_store/right_store 是可选的区间边界输出指针。
{
    if (start >= right) // 起点已经到达或超过右边界，说明没有可扫描的表项。
    {
        return 0; // 返回 0 表示没有找到有效区间。
    }
    while (start < right && !(table[start] & PTE_V)) // 跳过所有 V 位为 0 的无效表项，直到找到有效项或到达右边界。
    {
        start++; // 索引递增，继续检查下一个表项。
    }
    if (start < right) // 如果仍未越过右边界，说明找到了至少一个有效表项。
    {
        if (left_store != NULL) // 仅当调用者提供 left_store 时才写回起始索引。
        {
            *left_store = start; // 记录第一个有效表项的索引，作为连续区间的左边界。
        }
        int perm = (table[start++] & PTE_USER); // 取出当前表项的 PTE_USER 权限掩码作为本区间的权限，并将扫描位置推进一项。
        while (start < right && (table[start] & PTE_USER) == perm) // 继续向后扫描，直到遇到无效项、不同权限项或右边界。
        {
            start++; // 索引递增，检查下一个表项是否仍属于同一权限区间。
        }
        if (right_store != NULL) // 仅当调用者提供 right_store 时才写回区间结束索引。
        {
            *right_store = start; // 记录连续同权限区间的右边界（该位置本身不包含在区间内）。
        }
        return perm; // 返回本次找到的权限掩码，供上层打印或分析页表映射。
    }
    return 0; // 扫描到右边界仍未找到有效表项，返回 0。
}
