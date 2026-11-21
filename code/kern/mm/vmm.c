#include <vmm.h>
#include <sync.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <pmm.h>
#include <riscv.h>
#include <kmalloc.h>

/*
  vmm design include two parts: mm_struct (mm) & vma_struct (vma)
  mm is the memory manager for the set of continuous virtual memory
  area which have the same PDT. vma is a continuous virtual memory area.
  There a linear link list for vma & a redblack link list for vma in mm.
---------------
  mm related functions:
   golbal functions
     struct mm_struct * mm_create(void)
     void mm_destroy(struct mm_struct *mm)
     int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr)
--------------
  vma related functions:
   global functions
     struct vma_struct * vma_create (uintptr_t vm_start, uintptr_t vm_end,...)
     void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
     struct vma_struct * find_vma(struct mm_struct *mm, uintptr_t addr)
   local functions
     inline void check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
---------------
   check correctness functions
     void check_vmm(void);
     void check_vma_struct(void);
     void check_pgfault(void);
*/
/*
 * VMM（Virtual Memory Management，虚拟内存管理）主要由两类数据结构组成：
 *
 * 1. mm_struct：
 *    表示一个完整的虚拟地址空间管理对象，负责统一管理多个 VMA，
 *    并保存这些 VMA 共用的页目录（页表根节点）。
 *
 * 2. vma_struct：
 *    表示一个连续的虚拟内存区域，记录该区域的起始地址、结束地址
 *    和访问权限等信息。
 *
 * 在当前代码中：
 *    mm_struct 通过 mmap_list 双向循环链表管理多个 VMA，
 *    链表按照 VMA 的 vm_start 从小到大排列；
 *    同时使用 mmap_cache 缓存最近访问的 VMA，以提高查找效率。
 *
 * 注意：原始注释提到了红黑树，但当前给出的 mm_struct
 * 实际只包含线性链表和缓存指针，并没有红黑树成员。
 *
 * mm_struct 相关的全局函数：
 *    mm_create()       创建并初始化内存管理结构
 *    mm_destroy()      释放内存管理结构及其 VMA
 *    do_pgfault()      处理缺页异常
 *
 * vma_struct 相关的全局函数：
 *    vma_create()          创建虚拟内存区域
 *    insert_vma_struct()   将 VMA 插入 MM 管理链表
 *    find_vma()            根据虚拟地址查找对应 VMA
 *
 * vma_struct 相关的内部函数：
 *    check_vma_overlap()   检查两个相邻 VMA 是否发生重叠
 *
 * 正确性检查函数：
 *    check_vmm()           检查虚拟内存管理模块
 *    check_vma_struct()    检查 VMA 创建、插入、排序和查找功能
 *    check_pgfault()       缺页异常处理检查函数的声明
 */









/*
 * 函数名称：print_vma
 *
 * 函数作用：
 *    输出指定 VMA 的内部信息，包括所属 mm_struct、虚拟地址范围、
 *    访问权限以及链表节点地址。
 *
 * 参数：
 *    name：调用者提供的名称，用于标识本次调试输出。
 *    vma：需要打印信息的虚拟内存区域指针。
 *
 * 主要用途：
 *    检查某个 VMA 是否正确创建、是否属于预期的 mm_struct，
 *    以及其虚拟地址范围和链表节点是否正确。
 */
void print_vma(char *name, struct vma_struct *vma)
{
    cprintf("-- %s print_vma --\n", name);        // 打印当前调试函数的名称，方便区分不同的输出信息

    cprintf("   mm_struct: %p\n", vma->vm_mm);    // 输出该 VMA 所属的 mm_struct 结构体地址

    cprintf("   vm_start,vm_end: %x,%x\n", vma->vm_start, vma->vm_end); // 输出 VMA 的起始和结束虚拟地址

    cprintf("   vm_flags: %x\n", vma->vm_flags);  // 输出 VMA 的访问权限标志，例如读、写和执行权限

    cprintf("   list_entry_t: %p\n", &vma->list_link); // 输出 VMA 内部链表节点的地址，用于观察链表组织情况
}


/*
 * 函数名称：print_mm
 *
 * 函数作用：
 *    输出指定 mm_struct 的内部信息，并遍历其 mmap_list 链表，
 *    逐个打印该 mm_struct 所管理的 VMA 信息。
 *
 * 参数：
 *    name：调试输出使用的名称。
 *    mm：需要打印的虚拟内存管理结构体指针。
 *
 * 执行过程：
 *    1. 输出 mmap_list 链表头地址和 map_count。
 *    2. 从链表头开始遍历 VMA。
 *    3. 利用 le2vma 将链表节点转换为 vma_struct 指针。
 *    4. 调用 print_vma 输出每个 VMA 的详细信息。
 */
void print_mm(char *name, struct mm_struct *mm)
{
    cprintf("-- %s print_mm --\n", name);        // 打印当前 MM 调试输出的名称

    cprintf("   mmap_list: %p\n", &mm->mmap_list); // 输出 VMA 双向循环链表头节点的地址

    cprintf("   map_count: %d\n", mm->map_count); // 输出当前 mm_struct 管理的 VMA 数量

    list_entry_t *list = &mm->mmap_list;         // 将 list 指针指向链表头节点，作为遍历的起点

    for (int i = 0; i < mm->map_count; i++)      // 根据 map_count 指定的 VMA 数量进行遍历
    {
        list = list_next(list);                  // 移动到当前链表节点的下一个节点

        print_vma(name, le2vma(list, list_link)); // 将链表节点转换为 vma_struct 指针，并打印其详细信息
    }
}


// ==================== 内部检查函数声明 ====================

// 声明 check_vmm，负责检查虚拟内存管理模块的整体正确性
static void check_vmm(void);

// 声明 check_vma_struct，负责检查 VMA 的创建、插入、排序和查找功能
static void check_vma_struct(void);

// 声明 check_pgfault，预留用于检查缺页异常处理机制
static void check_pgfault(void);









/*
 * 函数名称：mm_create
 *
 * 函数作用：
 *    动态分配一个 mm_struct 结构体，并对其内部成员进行初始化。
 *
 * 参数：
 *    无。
 *
 * 执行过程：
 *    1. 调用 kmalloc 分配 mm_struct 所需的内核内存。
 *    2. 检查内存分配是否成功。
 *    3. 初始化 VMA 双向循环链表。
 *    4. 将 VMA 缓存指针 mmap_cache 初始化为空。
 *    5. 将页目录指针 pgdir 初始化为空。
 *    6. 将 VMA 数量 map_count 初始化为 0。
 *    7. 将交换管理器私有数据 sm_priv 初始化为空。
 *
 * 返回值：
 *    成功：返回新创建的 mm_struct 指针。
 *    失败：返回 NULL。
 */
struct mm_struct *
mm_create(void)
{
    struct mm_struct *mm = kmalloc(sizeof(struct mm_struct)); // 动态申请一块足够容纳 mm_struct 的内核内存

    if (mm != NULL)                         // 判断内存申请是否成功，只有成功后才能初始化结构体
    {
        list_init(&(mm->mmap_list));        // 初始化 VMA 双向循环链表，将链表头的前驱和后继指针指向自身

        mm->mmap_cache = NULL;              // 初始时还没有访问过任何 VMA，因此缓存指针设置为 NULL

        mm->pgdir = NULL;                   // 初始化页目录指针，表示当前尚未关联页目录

        mm->map_count = 0;                  // 初始时没有任何 VMA，因此 VMA 数量设置为 0

        mm->sm_priv = NULL;                 // 初始时没有交换管理器私有数据，因此设置为 NULL
    }

    return mm;                              // 返回创建好的 mm_struct 指针；如果申请失败则返回 NULL
}


// ==================== 创建虚拟内存区域 VMA ====================

/*
 * 函数名称：vma_create
 *
 * 函数作用：
 *    动态分配一个 vma_struct 结构体，并设置其虚拟地址范围和访问权限。
 *
 * 参数：
 *    vm_start：VMA 的起始虚拟地址。
 *    vm_end：VMA 的结束虚拟地址，不包含该地址本身。
 *    vm_flags：VMA 的访问权限标志。
 *
 * 执行过程：
 *    1. 使用 kmalloc 为 VMA 分配内存。
 *    2. 判断内存分配是否成功。
 *    3. 设置 VMA 的起始地址。
 *    4. 设置 VMA 的结束地址。
 *    5. 设置 VMA 的访问权限。
 *
 * 返回值：
 *    成功：返回新创建的 vma_struct 指针。
 *    失败：返回 NULL。
 *
 * 注意：
 *    本函数只负责分配和设置基本成员，不负责将 VMA 插入 mm_struct。
 *    vm_mm 成员会在 insert_vma_struct() 中设置。
 */
struct vma_struct *
vma_create(uintptr_t vm_start, uintptr_t vm_end, uint32_t vm_flags)
{
    struct vma_struct *vma = kmalloc(sizeof(struct vma_struct)); // 为新 VMA 动态申请内核内存

    if (vma != NULL)                         // 判断内存申请是否成功
    {
        vma->vm_start = vm_start;            // 设置当前 VMA 的起始虚拟地址

        vma->vm_end = vm_end;                // 设置当前 VMA 的结束虚拟地址，结束地址不属于该区域

        vma->vm_flags = vm_flags;            // 设置当前 VMA 的访问权限，例如 VM_READ、VM_WRITE、VM_EXEC
    }

    return vma;                              // 返回新创建的 VMA 指针；申请失败则返回 NULL
}








/*
 * 函数名称：find_vma
 *
 * 函数作用：
 *    在指定 mm_struct 管理的所有 VMA 中，查找包含给定虚拟地址 addr 的 VMA。
 *
 * 参数：
 *    mm：需要查询的虚拟内存管理结构体。
 *    addr：需要判断归属关系的虚拟地址。
 *
 * 执行过程：
 *    1. 首先检查 mm 是否有效。
 *    2. 优先检查 mmap_cache 缓存的 VMA 是否包含 addr。
 *    3. 如果缓存未命中，则从 mmap_list 链表头开始依次遍历。
 *    4. 判断 addr 是否满足 vm_start <= addr < vm_end。
 *    5. 找到后更新 mmap_cache，方便下一次快速查找。
 *
 * 返回值：
 *    找到：返回包含 addr 的 vma_struct 指针。
 *    未找到：返回 NULL。
 *
 * 设计特点：
 *    使用 mmap_cache 优化连续或重复访问同一 VMA 的情况；
 *    缓存未命中时，采用线性链表进行查找。
 */
struct vma_struct *
find_vma(struct mm_struct *mm, uintptr_t addr)
{
    struct vma_struct *vma = NULL;             // 初始化 VMA 指针，默认表示尚未找到目标区域

    if (mm != NULL)                            // 检查 mm_struct 是否有效，防止访问空指针
    {
        vma = mm->mmap_cache;                  // 首先获取最近一次访问的 VMA，尝试通过缓存加快查找

        if (!(vma != NULL && vma->vm_start <= addr && vma->vm_end > addr)) // 判断缓存是否有效，以及 addr 是否属于缓存中的 VMA
        {
            bool found = 0;                    // 设置查找标志，0 表示当前尚未找到目标 VMA

            list_entry_t *list = &(mm->mmap_list), *le = list; // list 指向链表头，le 用于记录当前遍历位置

            while ((le = list_next(le)) != list) // 沿双向循环链表向后遍历，直到重新回到链表头
            {
                vma = le2vma(le, list_link);   // 将当前链表节点地址转换为对应的 vma_struct 结构体地址

                if (vma->vm_start <= addr && addr < vma->vm_end) // 判断目标地址是否处于当前 VMA 的左闭右开区间内
                {
                    found = 1;                 // 找到包含目标地址的 VMA，将查找标志设置为 1

                    break;                     // 已找到目标区域，不需要继续遍历链表
                }
            }

            if (!found)                        // 如果遍历完整个链表仍然没有找到目标 VMA
            {
                vma = NULL;                    // 将 VMA 指针设置为 NULL，表示该地址不属于任何 VMA
            }
        }

        if (vma != NULL)                       // 判断是否成功找到了目标 VMA
        {
            mm->mmap_cache = vma;              // 更新最近访问的 VMA 缓存，为后续查找提供加速
        }
    }

    return vma;                                // 返回查找结果，找到时返回 VMA 指针，否则返回 NULL
}








/*
 * 函数名称：check_vma_overlap
 *
 * 函数作用：
 *    检查两个按照虚拟地址顺序排列的 VMA 是否满足合法的地址范围关系，
 *    确保前一个 VMA 和后一个 VMA 不发生重叠。
 *
 * 参数：
 *    prev：前一个 VMA，即虚拟地址较低的区域。
 *    next：后一个 VMA，即虚拟地址较高的区域。
 *
 * 检查条件：
 *    1. prev->vm_start < prev->vm_end：
 *       前一个 VMA 的起始地址必须小于结束地址。
 *
 *    2. prev->vm_end <= next->vm_start：
 *       前一个 VMA 的结束地址不能大于后一个 VMA 的起始地址。
 *       该条件用于保证两个 VMA 不重叠。
 *
 *    3. next->vm_start < next->vm_end：
 *       后一个 VMA 的起始地址必须小于结束地址。
 *
 * 注意：
 *    assert 只负责检查条件是否成立，并不负责自动修复重叠区域。
 *    条件不成立时，内核会触发断言失败。
 */
static inline void
check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
{
    assert(prev->vm_start < prev->vm_end);     // 检查前一个 VMA 的地址范围是否合法，起始地址必须小于结束地址

    assert(prev->vm_end <= next->vm_start);    // 检查两个 VMA 是否重叠，前一个区域必须在后一个区域之前结束

    assert(next->vm_start < next->vm_end);     // 检查后一个 VMA 的地址范围是否合法，起始地址必须小于结束地址
}








/*
 * 函数名称：insert_vma_struct
 *
 * 函数作用：
 *    将一个已经创建好的 VMA 插入指定 mm_struct 的 VMA 双向循环链表中。
 *
 * 参数：
 *    mm：需要插入 VMA 的虚拟内存管理结构体。
 *    vma：需要插入的虚拟内存区域描述符。
 *
 * 执行过程：
 *    1. 检查新 VMA 的起始地址是否小于结束地址。
 *    2. 从 mmap_list 链表头开始遍历，寻找合适的插入位置。
 *    3. 找到起始地址大于新 VMA 起始地址的第一个 VMA。
 *    4. 确定新 VMA 前后的相邻节点。
 *    5. 分别检查新 VMA 与前后 VMA 是否发生地址重叠。
 *    6. 设置新 VMA 所属的 mm_struct。
 *    7. 将新 VMA 插入链表。
 *    8. 更新 map_count。
 *
 * 结果：
 *    VMA 链表继续按照 vm_start 从小到大排列，
 *    并且所有相邻 VMA 的地址范围互不重叠。
 */
void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
{
    assert(vma->vm_start < vma->vm_end);       // 检查新 VMA 的地址范围是否合法，起始地址必须小于结束地址

    list_entry_t *list = &(mm->mmap_list);     // 获取 mm_struct 中 VMA 链表的头节点地址

    list_entry_t *le_prev = list, *le_next;     // le_prev 保存插入位置的前一个节点，le_next 保存后一个节点

    list_entry_t *le = list;                   // 初始化遍历指针 le，使其从链表头节点开始

    while ((le = list_next(le)) != list)       // 遍历整个 VMA 链表，直到再次回到链表头
    {
        struct vma_struct *mmap_prev = le2vma(le, list_link); // 将当前链表节点转换为对应的 VMA 结构体指针

        if (mmap_prev->vm_start > vma->vm_start) // 判断当前 VMA 的起始地址是否大于新 VMA 的起始地址
        {
            break;                              // 找到第一个起始地址大于新 VMA 的节点，确定插入位置
        }

        le_prev = le;                           // 当前节点仍位于新 VMA 之前，更新前驱节点指针
    }

    le_next = list_next(le_prev);               // 获取 le_prev 的下一个节点，作为新 VMA 插入位置的后继节点

    /* check overlap */                          // 检查新 VMA 与插入位置前后两个相邻 VMA 是否发生重叠

    if (le_prev != list)                        // 如果前驱节点不是链表头，说明新 VMA 前面存在一个 VMA
    {
        check_vma_overlap(le2vma(le_prev, list_link), vma); // 检查前一个 VMA 与新 VMA 是否重叠
    }

    if (le_next != list)                        // 如果后继节点不是链表头，说明新 VMA 后面存在一个 VMA
    {
        check_vma_overlap(vma, le2vma(le_next, list_link)); // 检查新 VMA 与后一个 VMA 是否重叠
    }

    vma->vm_mm = mm;                            // 设置新 VMA 所属的 mm_struct，建立 VMA 与 MM 之间的关联关系

    list_add_after(le_prev, &(vma->list_link)); // 将新 VMA 的链表节点插入 le_prev 之后，保持链表的地址升序

    mm->map_count++;                            // 更新当前 mm_struct 管理的 VMA 数量，增加 1
}










/*
 * 函数名称：mm_destroy
 *
 * 函数作用：
 *    销毁指定的 mm_struct，释放它所管理的所有 VMA 描述符，
 *    最后释放 mm_struct 自身占用的内核内存。
 *
 * 参数：
 *    mm：需要销毁的虚拟内存管理结构体指针。
 *
 * 执行过程：
 *    1. 获取 mm_struct 中 VMA 链表的头节点。
 *    2. 从链表头开始，逐个取出 VMA 节点。
 *    3. 使用 list_del 将当前 VMA 从链表中删除。
 *    4. 使用 le2vma 将链表节点转换为 VMA 结构体指针。
 *    5. 使用 kfree 释放当前 VMA 描述符占用的内存。
 *    6. 所有 VMA 释放完毕后，使用 kfree 释放 mm_struct。
 *
 * 注意：
 *    本函数没有释放 mm->pgdir 指向的页目录，也没有释放
 *    该页目录中映射的物理页面。
 *    最后一行 mm = NULL 仅修改局部指针变量，不会改变调用者
 *    保存的原始指针。
 */
void mm_destroy(struct mm_struct *mm)
{

    list_entry_t *list = &(mm->mmap_list), *le; // list 指向 VMA 链表头，le 用于记录当前待释放的链表节点

    while ((le = list_next(list)) != list)      // 不断获取链表头的下一个节点，直到链表为空
    {
        list_del(le);                           // 将当前 VMA 对应的链表节点从双向循环链表中删除

        kfree(le2vma(le, list_link));           // 将链表节点转换为 vma_struct 指针，并释放该 VMA 占用的内存
    }

    kfree(mm);                                  // 释放 mm_struct 结构体自身占用的内核内存

    mm = NULL;                                  // 将当前函数的局部指针设置为 NULL，避免在本函数中继续使用已释放的指针
}









/*
 * 函数名称：vmm_init
 *
 * 函数作用：
 *    初始化虚拟内存管理模块。
 *
 * 执行过程：
 *    当前实现主要通过调用 check_vmm() 对 VMA 管理机制进行正确性检查。
 *
 * 注意：
 *    本函数本身没有直接创建页目录或分配物理页面，
 *    主要用于执行虚拟内存管理模块的初始化检查。
 */
void vmm_init(void)
{
    check_vmm();                   // 调用整体检查函数，验证 VMA 管理相关操作是否正确
}


// ==================== VMM 整体正确性检查 ====================

/*
 * 函数名称：check_vmm
 *
 * 函数作用：
 *    对虚拟内存管理模块执行正确性测试。
 *
 * 执行过程：
 *    1. 调用 check_vma_struct()，检查 VMA 创建、插入、排序和查找功能。
 *    2. 原本可以调用 check_pgfault() 检查缺页异常处理机制，
 *       但当前代码将该调用注释掉了。
 *
 * 结果：
 *    如果 VMA 相关断言全部通过，则输出检查成功信息。
 */
static void
check_vmm(void)
{
    check_vma_struct();                         // 检查 VMA 的创建、插入、排序及查找功能是否正确

    // check_pgfault();                          // 原本用于检查缺页异常处理机制，目前被注释，不会执行

    cprintf("check_vmm() succeeded.\n");        // 输出 VMM 检查成功信息，表示当前已执行的检查通过
}








/*
 * 函数名称：check_vma_struct
 *
 * 函数作用：
 *    测试 mm_struct 和 vma_struct 的基本管理功能，
 *    验证 VMA 创建、链表插入、地址排序、区域查找和销毁操作是否正确。
 *
 * 执行过程：
 *    第一阶段：创建一个 mm_struct。
 *
 *    第二阶段：创建并插入 100 个 VMA。
 *       前 10 个 VMA 按照起始地址从大到小的顺序插入；
 *       后 90 个 VMA 按照起始地址从小到大的顺序插入。
 *
 *    第三阶段：遍历 mmap_list，检查最终链表是否按照起始地址升序排列。
 *
 *    第四阶段：使用 find_vma() 测试地址查找功能，
 *       检查 VMA 内部地址、结束边界以及区域外地址的查找结果。
 *
 *    第五阶段：检查第一个 VMA 之前的地址是否都无法找到对应区域。
 *
 *    第六阶段：调用 mm_destroy() 释放测试创建的内存管理结构及 VMA。
 *
 * 测试结果：
 *    所有 assert 断言均通过时，输出 check_vma_struct() succeeded。
 */
static void
check_vma_struct(void)
{
    struct mm_struct *mm = mm_create();          // 创建一个新的虚拟内存管理结构体，用于管理本次测试的 VMA

    assert(mm != NULL);                          // 检查 mm_struct 是否创建成功，失败则立即触发断言

    int step1 = 10, step2 = step1 * 10;           // 设置测试规模：step1 为 10，step2 为 100，最终创建 100 个 VMA

    int i;                                       // 声明循环变量 i，用于控制 VMA 的创建和测试过程

    for (i = step1; i >= 1; i--)                 // 从 10 递减到 1，按照起始地址从大到小的顺序创建前 10 个 VMA
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0); // 创建一个起始地址为 i*5、结束地址为 i*5+2、权限标志为 0 的 VMA

        assert(vma != NULL);                     // 检查 VMA 是否创建成功

        insert_vma_struct(mm, vma);              // 将新创建的 VMA 插入 mm 的链表中，插入函数会自动维护地址升序
    }

    for (i = step1 + 1; i <= step2; i++)          // 从 11 递增到 100，按照起始地址从小到大的顺序创建剩余 90 个 VMA
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0); // 创建起始地址为 i*5、结束地址为 i*5+2 的 VMA

        assert(vma != NULL);                     // 检查当前 VMA 是否成功分配

        insert_vma_struct(mm, vma);              // 将当前 VMA 插入链表，确保链表始终按照起始地址升序排列
    }

    list_entry_t *le = list_next(&(mm->mmap_list)); // 获取 VMA 链表头的下一个节点，即第一个实际 VMA 节点

    for (i = 1; i <= step2; i++)                  // 遍历预期存在的 100 个 VMA
    {
        assert(le != &(mm->mmap_list));           // 检查当前节点不能是链表头，否则说明实际 VMA 数量不足

        struct vma_struct *mmap = le2vma(le, list_link); // 将当前链表节点转换为对应的 vma_struct 结构体指针

        assert(mmap->vm_start == i * 5 && mmap->vm_end == i * 5 + 2); // 检查当前 VMA 的起始地址和结束地址是否符合预期，验证链表排序和内容正确

        le = list_next(le);                       // 移动到下一个链表节点，继续检查后续 VMA
    }

    for (i = 5; i <= 5 * step2; i += 5)          // 从 5 开始，每次增加 5，依次测试 100 个 VMA
    {
        struct vma_struct *vma1 = find_vma(mm, i); // 查找 VMA 的起始地址，预期能够找到对应 VMA

        assert(vma1 != NULL);                    // 检查起始地址是否能够正确匹配到 VMA

        struct vma_struct *vma2 = find_vma(mm, i + 1); // 查找 VMA 内部的另一个地址，预期仍然属于同一个 VMA

        assert(vma2 != NULL);                    // 检查 VMA 内部地址能否正确找到对应区域

        struct vma_struct *vma3 = find_vma(mm, i + 2); // 查找 VMA 的结束地址，该地址不属于当前 VMA

        assert(vma3 == NULL);                     // 检查结束边界是否被正确排除

        struct vma_struct *vma4 = find_vma(mm, i + 3); // 查找当前 VMA 结束之后的第一个地址

        assert(vma4 == NULL);                     // 检查该地址不属于任何 VMA

        struct vma_struct *vma5 = find_vma(mm, i + 4); // 查找当前 VMA 结束之后的第二个地址

        assert(vma5 == NULL);                     // 检查该地址同样不属于任何 VMA

        assert(vma1->vm_start == i && vma1->vm_end == i + 2); // 检查第一次查找得到的 VMA 地址范围是否正确

        assert(vma2->vm_start == i && vma2->vm_end == i + 2); // 检查第二次查找得到的 VMA 是否与第一次相同
    }

    for (i = 4; i >= 0; i--)                     // 从地址 4 递减到地址 0，检查第一个 VMA 之前的地址
    {
        struct vma_struct *vma_below_5 = find_vma(mm, i); // 查找地址 i 对应的 VMA，判断其是否存在

        if (vma_below_5 != NULL)                 // 如果意外找到了对应 VMA
        {
            cprintf("vma_below_5: i %x, start %x, end %x\n", i, vma_below_5->vm_start, vma_below_5->vm_end); // 输出异常查找结果，方便定位错误
        }

        assert(vma_below_5 == NULL);             // 断言地址 0~4 均不属于任何 VMA，因为第一个 VMA 从地址 5 开始
    }

    mm_destroy(mm);                              // 释放本次测试创建的全部 VMA 描述符及 mm_struct 结构体

    cprintf("check_vma_struct() succeeded!\n");  // 输出测试成功信息，表示所有 VMA 管理相关断言均已通过
}


