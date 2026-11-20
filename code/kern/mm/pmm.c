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
