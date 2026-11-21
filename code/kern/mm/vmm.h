
#ifndef __KERN_MM_VMM_H__             // 检查宏是否已经定义，防止头文件被重复包含
#define __KERN_MM_VMM_H__             // 定义头文件保护宏

#include <defs.h>                     // 引入基础类型定义，例如 uintptr_t、uint32_t、bool 等
#include <list.h>                     // 引入双向循环链表的数据结构及相关操作函数
#include <memlayout.h>                // 引入内存布局定义，包括页目录项等内存管理相关定义
#include <sync.h>                     // 引入同步机制相关定义，例如自旋锁等

// ==================== 前置声明 ====================

// 前置声明 mm_struct，告诉编译器存在这个结构体类型。
// 此处只声明类型，不需要提前知道结构体内部成员。
// 这样可以在 vma_struct 中使用 struct mm_struct * 类型的指针。
struct mm_struct;


// ==================== 虚拟内存区域 VMA ====================

// vma_struct 表示一个虚拟内存区域（Virtual Memory Area）。
// 一个 VMA 描述一段连续的虚拟地址空间，采用左闭右开的区间表示法：
// [vm_start, vm_end)
//
// 地址 addr 属于该 VMA 的条件为：
// vm_start <= addr < vm_end
//
// 例如：vm_start = 0x1000，vm_end = 0x5000，
// 则该 VMA 管理的地址范围为 0x1000~0x4FFF。
struct vma_struct {

    struct mm_struct *vm_mm;           // 指向管理当前 VMA 的 mm_struct，表明该 VMA 属于哪个虚拟内存管理结构体

    uintptr_t vm_start;                // 当前虚拟内存区域的起始虚拟地址，属于该区域

    uintptr_t vm_end;                  // 当前虚拟内存区域的结束虚拟地址，但不包含该地址本身

    uint32_t vm_flags;                 // 当前虚拟内存区域的访问权限标志，用于表示是否允许读、写、执行

    list_entry_t list_link;            // 双向链表节点，用于将当前 VMA 插入所属 mm_struct 的 mmap_list 链表中
};


// ==================== VMA 指针转换宏 ====================

// le2vma 用于通过链表节点地址，反向获得包含该节点的 vma_struct 结构体地址。
// le：指向某个 VMA 内部 list_link 成员的链表节点指针。
// member：指定 vma_struct 中作为链表节点的成员名称，此处为 list_link。
// to_struct 会根据成员偏移量计算出整个 vma_struct 的起始地址。
#define le2vma(le, member) \
    to_struct((le), struct vma_struct, member)


// ==================== 虚拟内存区域访问权限 ====================

// VM_READ 表示该虚拟内存区域允许进行读操作。
#define VM_READ                 0x00000001

// VM_WRITE 表示该虚拟内存区域允许进行写操作。
#define VM_WRITE                0x00000002

// VM_EXEC 表示该虚拟内存区域允许执行指令。
#define VM_EXEC                 0x00000004


// ==================== 虚拟内存管理结构体 MM ====================

// mm_struct 用于管理一组属于同一个虚拟地址空间的 VMA。
// 这些 VMA 共用同一个页目录（或页表根节点）。
// 一个进程通常拥有一个独立的 mm_struct，
// 内核线程也可能没有独立的用户虚拟地址空间。
struct mm_struct {

    list_entry_t mmap_list;            // VMA 双向循环链表的头节点，链表按照 vm_start 从小到大排列

    struct vma_struct *mmap_cache;     // 最近访问过的 VMA 缓存指针，用于加快后续 find_vma() 查找速度

    pde_t *pgdir;                      // 当前虚拟地址空间所使用的页目录（Sv39 中为根页表）指针，用于完成虚拟地址到物理地址的映射

    int map_count;                     // 当前 mm_struct 管理的 VMA 数量，每插入一个 VMA 加一

    void *sm_priv;                     // 交换管理器（Swap Manager）的私有数据指针，用于保存交换管理相关的信息
};


// ==================== VMA 管理函数 ====================

// 根据给定的虚拟地址 addr，在 mm 管理的 VMA 中查找包含该地址的区域。
// 如果找到，则返回对应的 vma_struct 指针；如果不存在，则返回 NULL。
struct vma_struct *find_vma(struct mm_struct *mm, uintptr_t addr);

// 根据给定的起始地址、结束地址和访问权限，动态分配并初始化一个 VMA。
// 成功时返回新建的 VMA 指针，内存分配失败时返回 NULL。
struct vma_struct *vma_create(uintptr_t vm_start, uintptr_t vm_end, uint32_t vm_flags);

// 将指定的 VMA 插入 mm 的 VMA 链表中，并按照虚拟地址起始位置保持升序排列。
// 插入时还会检查新区域是否与相邻区域发生重叠。
void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma);


// ==================== MM 管理函数 ====================

// 动态分配一个 mm_struct，并初始化其 VMA 链表、缓存指针、页目录指针等成员。
// 成功时返回 mm_struct 指针，失败时返回 NULL。
struct mm_struct *mm_create(void);

// 释放 mm_struct 及其管理的 VMA 描述符。
// 注意：本文件中的实现主要释放管理结构，不负责释放页目录及其映射的物理页面。
void mm_destroy(struct mm_struct *mm);


// ==================== 虚拟内存管理初始化 ====================

// 初始化虚拟内存管理模块，并调用相关检查函数验证 VMA 管理机制是否正确。
void vmm_init(void);


// ==================== 缺页异常处理 ====================

// 处理缺页异常（Page Fault）。
// mm：发生缺页异常的进程的虚拟内存管理结构体。
// error_code：缺页异常的错误码，用于判断异常类型及访问权限问题。
// addr：发生缺页异常的虚拟地址。
// 返回值通常用于表示缺页处理成功或失败。
int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr);


// ==================== 全局变量 ====================

// pgfault_num 用于记录缺页异常发生的次数。
// volatile 表示该变量的值可能在程序控制流之外发生变化，
// 编译器不应随意将其访问优化为只使用寄存器中的旧值。
extern volatile unsigned int pgfault_num;

// check_mm_struct 是用于虚拟内存管理测试的全局 mm_struct 指针。
extern struct mm_struct *check_mm_struct;

#endif /* !__KERN_MM_VMM_H__ */      // 结束头文件保护
