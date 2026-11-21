#ifndef __KERN_PROCESS_PROC_H__ // 防止 proc.h 被重复包含，避免重复定义类型和接口。
#define __KERN_PROCESS_PROC_H__ // 定义本头文件的保护宏。

#include <defs.h>      // 引入内核基础类型定义，例如 uintptr_t、uint32_t、bool 等。
#include <list.h>      // 引入双向循环链表结构及 list_init、list_add 等链表操作宏/函数。
#include <trap.h>      // 引入 trapframe 中断帧定义，保存线程被中断或首次运行时的寄存器现场。
#include <memlayout.h> // 引入内核内存布局常量，例如内核栈大小等。

// 进程/线程在生命周期中所处的状态；调度器通常只选择 PROC_RUNNABLE 状态的执行实体。
enum proc_state // 定义进程状态枚举类型。
{
    PROC_UNINIT = 0, // 未初始化：PCB 已分配，但还没有完成进程初始化，不能被正常调度。
    PROC_SLEEPING,   // 睡眠/阻塞：进程正在等待某个事件或资源，暂时不能获得 CPU。
    PROC_RUNNABLE,   // 就绪/运行：进程具备运行条件，可能在等待 CPU，也可能正占用 CPU。
    PROC_ZOMBIE,     // 僵尸：进程已结束运行，但其资源/退出信息仍等待父进程回收。
}; // 结束进程状态枚举定义。

// 线程上下文：保存切换线程时必须保留的被调用者保存寄存器和返回位置。
struct context // 定义一次内核线程切换所需保存的最小寄存器上下文。
{
    uintptr_t ra;  // RISC-V 返回地址寄存器 x1；switch_to 恢复后从该地址继续执行。
    uintptr_t sp;  // RISC-V 栈指针寄存器 x2；指向该线程自己的内核栈当前栈顶。
    uintptr_t s0;  // RISC-V 被调用者保存寄存器 x8，也常用作帧指针；切换时必须保存。
    uintptr_t s1;  // RISC-V 被调用者保存寄存器 x9；用于跨函数调用保留数据。
    uintptr_t s2;  // RISC-V 被调用者保存寄存器 x18；线程切换时保存和恢复。
    uintptr_t s3;  // RISC-V 被调用者保存寄存器 x19；线程切换时保存和恢复。
    uintptr_t s4;  // RISC-V 被调用者保存寄存器 x20；线程切换时保存和恢复。
    uintptr_t s5;  // RISC-V 被调用者保存寄存器 x21；线程切换时保存和恢复。
    uintptr_t s6;  // RISC-V 被调用者保存寄存器 x22；线程切换时保存和恢复。
    uintptr_t s7;  // RISC-V 被调用者保存寄存器 x23；线程切换时保存和恢复。
    uintptr_t s8;  // RISC-V 被调用者保存寄存器 x24；线程切换时保存和恢复。
    uintptr_t s9;  // RISC-V 被调用者保存寄存器 x25；线程切换时保存和恢复。
    uintptr_t s10; // RISC-V 被调用者保存寄存器 x26；线程切换时保存和恢复。
    uintptr_t s11; // RISC-V 被调用者保存寄存器 x27；线程切换时保存和恢复。
}; // 结束线程上下文结构体；与 trapframe 不同，它只保存上下文切换所需的寄存器。

#define PROC_NAME_LEN 15 // 进程名称最多保存 15 个有效字符；名称数组还会额外留出 '\0'。
#define MAX_PROCESS 4096 // 系统允许同时管理的进程/线程控制块数量上限。
#define MAX_PID (MAX_PROCESS * 2) // PID 搜索范围上限；实际分配时通常使用 1 到 MAX_PID-1，0 留给 idleproc。

extern list_entry_t proc_list; // 全局进程链表头；已登记的进程/线程通过各自的 list_link 接入该链表。

struct proc_struct // 进程控制块（PCB）：内核用它记录一个进程/内核线程的状态和运行资源。
{
    enum proc_state state;        // 当前生命周期状态；调度器根据它判断该执行实体是否可以运行。
    int pid;                      // Process ID，进程/线程标识号；idleproc 通常固定为 0。
    int runs;                     // 该进程被调度运行的次数，可用于统计或调度策略。
    uintptr_t kstack;             // 内核栈的内核虚拟地址基址；陷入内核和线程切换时使用。
    volatile bool need_resched;   // 非零表示当前执行实体应尽快让出 CPU 并触发重新调度。
    struct proc_struct *parent;   // 父进程 PCB 指针；新建线程通常将创建者 current 记录为 parent。
    struct mm_struct *mm;         // 用户虚拟地址空间管理结构；Lab4 的内核线程共享内核地址空间，通常为 NULL。
    struct context context;       // 线程切换上下文；switch_to 保存旧线程并恢复新线程时使用。
    struct trapframe *tf;         // 中断帧指针；保存通用寄存器、状态寄存器和异常返回地址等完整现场。
    uintptr_t pgdir;              // 根页表的物理地址；切换线程时将其页号写入 satp 以选择地址空间。
    uint32_t flags;               // 进程标志位；用于记录退出等状态属性，具体含义由相关宏定义。
    char name[PROC_NAME_LEN + 1]; // 进程名称缓冲区；最多 15 个有效字符，最后一个字节用于字符串结束符。
    list_entry_t list_link;       // 普通进程链表节点；将 PCB 挂入全局 proc_list。
    list_entry_t hash_link;       // PID 哈希链表节点；用于根据 PID 快速查找 PCB。
}; // 结束进程控制块定义。

// 将链表节点地址反向换算为其所属的 proc_struct 结构体地址；member 指明节点成员名。
#define le2proc(le, member) \
    to_struct((le), struct proc_struct, member) // 利用 to_struct/container_of 思想，从链表节点定位 PCB。

extern struct proc_struct *idleproc, *initproc, *current; // 声明空闲线程、初始化线程和当前正在运行的线程指针。

void proc_init(void); // 初始化进程管理子系统，建立 proc_list/PID 哈希表并创建 idleproc、initproc。
void proc_run(struct proc_struct *proc); // 将 CPU 执行权切换到指定的可运行线程，并切换其页表和上下文。
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags); // 创建内核线程；fn 为线程函数，arg 为传入参数。

char *set_proc_name(struct proc_struct *proc, const char *name); // 设置 PCB 中的进程名称，并返回名称缓冲区地址。
char *get_proc_name(struct proc_struct *proc); // 取得进程名称的副本；返回内部静态缓冲区地址。
void cpu_idle(void) __attribute__((noreturn)); // 空闲线程主循环；该函数持续运行，不应正常返回。

struct proc_struct *find_proc(int pid); // 根据 PID 在哈希表中查找进程控制块，找不到时返回 NULL。
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf); // 创建新执行实体，成功返回子线程 PID，失败返回负错误码。
int do_exit(int error_code); // 处理当前进程退出；具体实现负责释放资源、标记僵尸状态并触发调度。

#endif /* !__KERN_PROCESS_PROC_H__ */ // 结束头文件保护；后续再次包含本文件时跳过全部定义。
