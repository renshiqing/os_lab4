/*
 * proc.c —— uCore Lab4 进程/内核线程管理模块（逐行中文注释版）
 *
 * 本文件依据用户提供的 proc.c 和此前上传的 Lab4 实验指导书添加注释。
 * 只增加/整理注释，不改变原有 C 语句、函数签名、宏调用和控制流程。
 *
 * 阅读提示：
 * 1. proc_struct 是进程控制块（PCB），用于统一记录线程状态和资源。
 * 2. context 保存 switch_to 所需的最小上下文；trapframe 保存完整异常返回现场。
 * 3. Lab4 创建的是共享内核地址空间的内核线程，copy_mm() 因而不复制用户地址空间。
 * 4. 新线程首次运行的核心路径为 switch_to -> forkret -> forkrets -> __trapret。
 * 5. 当前代码中的 do_exit() 仍是 panic 占位实现，注释不会将其描述为已完成的退出流程。
 */

#include <proc.h> // 引入本模块的公共声明，包括 proc_struct、proc_state、context 和进程管理函数接口。
#include <kmalloc.h> // 引入 kmalloc/kfree，用于动态分配和释放进程控制块等内核对象。
#include <string.h> // 引入 memset、memcpy 等内存操作函数，用于初始化 PCB 和复制 trapframe。
#include <sync.h> // 引入中断保护/同步接口，避免修改进程全局数据结构时被中断打断。
#include <pmm.h> // 引入物理页面分配接口，用于为每个新线程分配内核栈。
#include <error.h> // 引入错误码定义，例如内存不足、进程数达到上限等错误。
#include <sched.h> // 引入调度器接口；本文件中的 cpu_idle 会在需要重调度时调用 schedule。
#include <elf.h> // 引入 ELF 文件格式定义，供进程装载相关代码使用；当前展示片段未直接解析 ELF。
#include <vmm.h> // 引入虚拟内存管理结构和接口，例如 mm_struct 等定义。
#include <trap.h> // 引入 trapframe 等异常/中断现场结构定义。
#include <stdio.h> // 引入 cprintf 等内核控制台输出接口。
#include <stdlib.h> // 引入标准库风格的内核辅助定义；具体使用取决于完整源文件。
#include <assert.h> // 引入 assert/panic 等断言和内核错误检查接口。

/*
 * 进程/线程机制说明（对应 Lab4 实验指导书）
 *
 * 一、uCore 的进程与内核线程
 *   uCore 将进程和线程统一交由 proc_struct 管理。一个执行实体至少包含：
 *   进程控制块（PCB）、内核栈、寄存器上下文，以及必要的地址空间信息。
 *   Lab4 重点创建的是内核线程：它们都运行在 S-mode（内核态），共享内核地址空间，
 *   因而本实验的 copy_mm() 不复制用户地址空间，proc->mm 保持为 NULL。
 *
 * 二、进程状态
 *   PROC_UNINIT   ：PCB 已分配，但尚未完成初始化或登记。
 *   PROC_SLEEPING ：线程因等待资源/事件而阻塞，暂时不能被调度执行。
 *   PROC_RUNNABLE ：线程已经具备运行条件；在 uCore 中该状态也可表示当前正在运行。
 *   PROC_ZOMBIE   ：线程已经退出，仍需由父进程或后续回收流程处理其资源。
 *
 * 三、典型状态变化
 *   alloc_proc() 创建 PCB 后，初始状态为 PROC_UNINIT；
 *   proc_init() 初始化 idleproc，或 wakeup_proc() 唤醒新线程后，使其进入 PROC_RUNNABLE；
 *   等待资源的线程可进入 PROC_SLEEPING，事件到来后再由 wakeup_proc() 唤醒；
 *   完整退出流程应由 do_exit() 将线程转为 PROC_ZOMBIE。
 *
 * 四、进程关系与链表
 *   parent 用于记录创建者/父进程；proc_list 维护全局进程集合；
 *   hash_list 按 PID 分桶，find_proc() 可据此查找 PCB。
 *   某些 uCore 版本还定义 children/cptr、older sibling/optr、younger sibling/yptr
 *   等关系指针；但当前 proc.h 中的 proc_struct 只包含 parent，不包含这些成员，
 *   因此不能把它们当作本版本结构体中已经存在的字段。
 *
 * 五、相关系统调用的关系（用于理解整体机制）
 *   SYS_exit  通常进入 do_exit()；
 *   SYS_fork / SYS_clone 通过 do_fork() 创建新的进程或线程；
 *   SYS_wait 用于等待并回收子进程；
 *   SYS_exec 用于装载新程序并更新地址空间；
 *   SYS_yield 请求重新调度，SYS_sleep 使当前执行实体睡眠；
 *   SYS_kill 设置退出标志并推动目标进程退出，SYS_getpid 查询当前 PID。
 *   这些系统调用描述的是完整 uCore 机制；当前上传代码片段并不包含所有系统调用实现。
 *
 * 六、Lab4 的主要执行链
 *   proc_init()
 *       ├── 建立 proc_list 和 PID 哈希桶
 *       ├── 创建 idleproc（PID=0，使用启动阶段的 bootstack）
 *       └── 通过 kernel_thread() 创建 initproc（通常 PID=1）
 *
 *   kernel_thread() 准备初始 trapframe
 *       └── do_fork() 分配 PCB/内核栈、设置 copy_mm/copy_thread、登记并唤醒线程
 *
 *   idleproc 进入 cpu_idle()
 *       └── schedule() 选择 PROC_RUNNABLE 线程
 *           └── proc_run() 更新 current、切换 satp 并刷新 TLB
 *               └── switch_to() 保存旧 context、恢复新 context
 *
 *   新线程第一次运行时沿 forkret -> forkrets -> __trapret 的恢复路径，
 *   最终从 trapframe 中的 epc（本例为 kernel_thread_entry）继续执行。
 *
 * 注意：本注释块说明实验整体设计；具体功能是否完成，仍以紧随其后的实际代码为准。
 */

// the process set's list；中文说明：全局进程链表头；普通新建线程会通过 list_link 接入该链表，调度器可遍历它寻找可运行线程。
list_entry_t proc_list; // 定义全局进程链表的哨兵节点；list_init 后该链表为空的循环链表。

#define HASH_SHIFT 10 // PID 哈希函数使用的移位位数，决定哈希桶数量为 2 的 HASH_SHIFT 次方。
#define HASH_LIST_SIZE (1 << HASH_SHIFT) // PID 哈希桶总数；这里为 2^10，即 1024 个桶。
#define pid_hashfn(x) (hash32(x, HASH_SHIFT)) // 把 PID 映射到哈希桶下标；hash32 将 PID 哈希为 HASH_SHIFT 位索引。

// has list for process set based on pid；中文说明：原有说明注释，保留其内容并补充中文解释。
static list_entry_t hash_list[HASH_LIST_SIZE]; // 静态 PID 哈希桶数组；每个桶是一个链表头，保存哈希值相同的进程节点。

// idle proc；中文说明：原有说明注释，保留其内容并补充中文解释。
struct proc_struct *idleproc = NULL; // idleproc 指向空闲线程 PCB；当没有其他可运行线程时，CPU 仍由它执行。
// init proc；中文说明：原有说明注释，保留其内容并补充中文解释。
struct proc_struct *initproc = NULL; // initproc 指向系统初始化后创建的第一个普通内核线程，Lab4 中负责打印测试字符串。
// current proc；中文说明：原有说明注释，保留其内容并补充中文解释。
struct proc_struct *current = NULL; // current 指向当前正在 CPU 上执行的进程/线程 PCB，是进程管理代码最常用的全局状态。

static int nr_process = 0; // 记录系统当前已经登记的进程/线程数量；创建成功时增加，资源回收时应相应减少。

void kernel_thread_entry(void); // 声明汇编入口 kernel_thread_entry；新内核线程第一次运行时从这里进入。
void forkrets(struct trapframe *tf); // 声明 forkrets；该入口负责从 trapframe 恢复寄存器现场并按异常返回路径继续执行。
void switch_to(struct context *from, struct context *to); // 声明底层上下文切换函数；from 保存旧线程上下文，to 恢复新线程上下文。

// set_proc_name - set the name of proc；中文说明：设置 PCB 中的进程名称。
char * // 函数返回 char *，表示返回进程名称缓冲区的地址。
set_proc_name(struct proc_struct *proc, const char *name) // proc 是待设置名称的 PCB，name 是调用者提供的以 '\0' 结尾的名称字符串。
{ // 函数体开始。
    memset(proc->name, 0, sizeof(proc->name)); // 先把固定长度名称数组清零，确保未写入部分保持字符串结束符。
    return memcpy(proc->name, name, PROC_NAME_LEN); // 复制最多 PROC_NAME_LEN 个字符到 PCB 名称数组并返回数组地址；数组最后一字节由清零操作保留为 '\0'。
} // 函数体结束。

// get_proc_name - get the name of proc；中文说明：读取 PCB 中保存的进程名称。
char * // 函数返回 char *，指向函数内部的静态缓冲区。
get_proc_name(struct proc_struct *proc) // proc 指向需要读取名称的进程控制块。
{ // 函数体开始。
    static char name[PROC_NAME_LEN + 1]; // 静态缓冲区在函数返回后仍然存在；其内容会被下一次调用覆盖，因此不能长期保存其地址。
    memset(name, 0, sizeof(name)); // 清空静态缓冲区，避免上一次调用残留字符影响本次名称。
    return memcpy(name, proc->name, PROC_NAME_LEN); // 复制 PCB 名称到静态缓冲区并返回；由于目标缓冲区先清零，名称末尾保持 '\0'。
} // 函数体结束。

// get_pid - alloc a unique pid for process；中文说明：分配一个当前未被占用的 PID；PID 0 通常保留给 idleproc。
static int // static 表示该函数只在本文件内部可见。
get_pid(void) // PID 分配函数不接收参数，返回新分配的整数 PID。
{ // 函数体开始。
    static_assert(MAX_PID > MAX_PROCESS); // 编译期断言：PID 空间必须大于最大进程数量，为 PID 分配和查找留出余量。
    struct proc_struct *proc; // proc 用于暂存遍历到的进程控制块指针。
    list_entry_t *list = &proc_list, *le; // list 指向全局进程链表头，le 用于逐个遍历链表节点。
    static int next_safe = MAX_PID, last_pid = MAX_PID; // last_pid 是上次尝试分配的 PID；next_safe 记录下一个可能冲突的已用 PID 上界，用于减少重复扫描。
    if (++last_pid >= MAX_PID) // 先把候选 PID 加 1；如果达到 PID 上限，则需要从 1 重新搜索。
    { // 进入 PID 回绕处理分支。
        last_pid = 1; // 将候选 PID 重置为 1；0 留给 idleproc，不作为普通新进程 PID。
        goto inside; // 跳到 inside 标签，重新完整扫描链表并计算 next_safe。
    } // 结束该条件分支。
    if (last_pid >= next_safe) // 若候选 PID 已到达 next_safe，说明上次扫描得到的安全区间已用尽，需要重新扫描。
    { // 进入重新扫描分支。
    inside: // 标签 inside：候选 PID 回绕或可能冲突时，从这里重新计算安全范围。
        next_safe = MAX_PID; // 先将 next_safe 重置为 MAX_PID，表示尚未发现比候选 PID 更小的冲突上界。
    repeat: // 标签 repeat：若发现候选 PID 冲突，则从链表头重新扫描。
        le = list; // 每次扫描都从全局进程链表头开始。
        while ((le = list_next(le)) != list) // 逐个遍历链表节点，直到再次回到哨兵节点，表示整个链表扫描完成。
        { // 循环体开始。
            proc = le2proc(le, list_link); // 利用 list_link 成员从链表节点反推出对应的 proc_struct 指针。
            if (proc->pid == last_pid) // 判断当前 PCB 的 PID 是否与候选 last_pid 相同；相同表示候选 PID 已被占用。
            { // 进入 PID 冲突处理分支。
                if (++last_pid >= next_safe) // 发现冲突后，将候选 PID 继续加 1，寻找下一个可能空闲的值。
                { // 检查新候选 PID 是否已经到达 next_safe；若到达，说明需要重新扫描。
                    if (last_pid >= MAX_PID) // 如果候选 PID 达到最大值，则需要回绕到 PID 1。
                    { // 进入 PID 回绕判断。
                        last_pid = 1; // 把候选 PID 重置为 1，避开保留给 idleproc 的 PID 0。
                    } // 结束回绕判断。
                    next_safe = MAX_PID; // 重置安全上界，准备重新扫描当前进程集合。
                    goto repeat; // 跳回 repeat，从链表头重新检查新的候选 PID 是否冲突。
                } // 结束内层条件分支。
            } // 结束 PID 相同的处理分支。
            else if (proc->pid > last_pid && next_safe > proc->pid) // 如果当前进程 PID 大于候选 PID，并且比现有 next_safe 更小，则它是新的最近冲突上界。
            { // 进入更新安全上界的分支。
                next_safe = proc->pid; // 把 next_safe 更新为当前进程 PID；候选 PID 在它之前的值无需再次扫描即可认为安全。
            } // 结束该分支。
        } // 结束本次链表遍历循环。
    } // 结束重新扫描条件分支。
    return last_pid; // 返回找到的候选 PID；调用方随后将它登记到哈希表和进程链表。
} // 函数体结束。


// forkret -- the first kernel entry point of a new thread/process；中文说明：forkret 是新线程第一次完成 switch_to 后进入的 C 语言入口。
// NOTE: the addr of forkret is setted in copy_thread function；中文说明：说明：copy_thread 会预先把 forkret 的地址写入新线程 context.ra。
//       after switch_to, the current proc will execute here.；中文说明：当调度器切换到该线程时，switch_to 恢复 ra/sp 后就会从 forkret 继续执行。
static void // static 表示该辅助函数仅供 proc.c 内部使用。
forkret(void) // forkret 不接收参数。
{ // 函数体开始。
    forkrets(current->tf); // 调用 forkrets 恢复当前线程的 trapframe；恢复完成后将沿 trapframe 中保存的 epc 继续执行。
} // 函数体结束。

// hash_proc - add proc into proc hash_list；中文说明：把进程控制块加入按 PID 组织的哈希链表。
static void // static 表示仅在当前源文件中使用。
hash_proc(struct proc_struct *proc) // proc 是需要登记到哈希表中的进程控制块。
{ // 函数体开始。
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link)); // 根据 proc->pid 计算桶下标，并把 PCB 的 hash_link 插入对应哈希桶链表。
} // 函数体结束。

// find_proc - find proc frome proc hash_list according to pid；中文说明：根据 PID 从哈希表中查找进程控制块；哈希表减少了查找时需要遍历的节点数量。
struct proc_struct * // 查找成功返回 proc_struct 指针，失败返回 NULL。
find_proc(int pid) // pid 是待查找的进程标识号。
{ // 函数体开始。
    if (0 < pid && pid < MAX_PID) // 仅对大于 0 且小于 MAX_PID 的普通 PID 执行查找；idleproc 的 PID 0 不在此范围。
    { // PID 合法时进入哈希桶查找。
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list; // 计算 PID 对应的哈希桶，并令 le 从该桶头开始遍历。
        while ((le = list_next(le)) != list) // 逐个遍历桶内链表，直到回到桶头。
        { // 循环体开始。
            struct proc_struct *proc = le2proc(le, hash_link); // 根据 hash_link 从当前哈希链表节点反推出所属 PCB。
            if (proc->pid == pid) // 比较 PCB 中的真实 PID，避免仅凭哈希桶相同就误判为命中。
            { // 找到目标 PID。
                return proc; // 返回目标进程控制块指针。
            } // 结束命中判断。
        } // 结束哈希桶遍历。
    } // 结束 PID 合法性判断。
    return NULL; // 未找到对应 PID 或 PID 非法时返回 NULL。
} // 函数体结束。


// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack；中文说明：为新进程分配内核栈；内核栈用于处理中断、异常和内核函数调用。
static int // static 表示该资源管理辅助函数只在 proc.c 内部使用。
setup_kstack(struct proc_struct *proc) // proc 指向需要设置内核栈的进程控制块。
{ // 函数体开始。
    struct Page *page = alloc_pages(KSTACKPAGE); // 分配 KSTACKPAGE 个连续物理页面作为该线程的内核栈。
    if (page != NULL) // 检查物理页面分配是否成功。
    { // 成功分配时进入初始化分支。
        proc->kstack = (uintptr_t)page2kva(page); // 把页面描述符转换为内核虚拟地址，并保存为进程内核栈基址。
        return 0; // 返回 0，表示内核栈分配成功。
    } // 结束成功分配分支。
    return -E_NO_MEM; // 分配失败时返回 -E_NO_MEM，通知上层进行错误清理。
} // 函数体结束。

// put_kstack - free the memory space of process kernel stack；中文说明：释放进程此前分配的内核栈。
static void // static 表示仅在本文件内部调用。
put_kstack(struct proc_struct *proc) // proc 指向需要释放内核栈的进程控制块。
{ // 函数体开始。
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE); // 将保存的内核栈虚拟地址转换为 Page 描述符，再释放 KSTACKPAGE 个连续页面。
} // 函数体结束。


// do_exit - called by sys_exit；中文说明：do_exit 通常由 SYS_exit 系统调用路径触发，用于结束当前进程。
//   1. call exit_mmap & put_pgdir & mm_destroy to free the almost all memory space of process；中文说明：完整退出流程通常需要释放用户映射、页目录及 mm_struct 等进程地址空间资源。
//   2. set process' state as PROC_ZOMBIE, then call wakeup_proc(parent) to ask parent reclaim itself.；中文说明：完整退出流程还应将进程状态设为 PROC_ZOMBIE，并通知父进程回收子进程资源。
//   3. call scheduler to switch to other process；中文说明：完成清理后应触发调度，让 CPU 转去执行其他可运行进程。
int do_exit(int error_code) // 当前 do_exit 接收退出码 error_code；退出码通常用于记录进程结束原因。
{ // 函数体开始。
    panic("process exit!!.\n"); // 注意：当前上传版本在这里直接 panic，尚未实现上述完整退出流程；此注释只解释现有代码，不代表退出功能已完成。
} // 函数体结束。

// init_main - the second kernel thread used to create user_main kernel threads；中文说明：init_main 是 proc_init 创建的第一个普通内核线程所执行的函数。
static int // static 表示该线程入口函数只在当前文件内使用。
init_main(void *arg) // arg 是创建线程时由 kernel_thread 传入的参数指针。
{ // 函数体开始。
    cprintf("this initproc, pid = %d, name = \"%s\"\n", current->pid, get_proc_name(current)); // 输出 initproc 的 PID 和名称；这是 Lab4 验证第一个内核线程成功创建并运行的重要输出。
    cprintf("To U: \"%s\".\n", (const char *)arg); // 输出创建线程时传入的字符串参数，预期可看到 Hello world!!。
    cprintf("To U: \"en.., Bye, Bye. :)\"\n"); // 输出线程结束前的固定告别信息，也是实验测试脚本检查的内容之一。
    return 0; // 返回 0 表示 init_main 函数正常执行结束；线程入口汇编后续通常负责转入退出处理。
} // 函数体结束。

//LAB4 function2 2412396；中文说明：Lab4 进程创建模块的实现标记；下面实现分配一个初始 PCB 的函数。
static struct proc_struct *alloc_proc(void) // 分配并初始化一个进程控制块；成功返回指针，内存不足返回 NULL。
{ // 函数体开始。
    struct proc_struct *proc = kmalloc(sizeof(*proc)); // 通过内核动态内存分配器申请 sizeof(struct proc_struct) 大小的 PCB 空间。
    if (proc != NULL) { // 仅当 kmalloc 成功返回非 NULL 指针时才初始化字段。
        memset(proc, 0, sizeof(*proc)); // 将整个 PCB 清零，使未显式赋值的指针、计数器、标志和上下文寄存器处于安全初始值。
        proc->state = PROC_UNINIT; // 设置为 PROC_UNINIT，表示 PCB 已分配但还未进入可运行状态。
        proc->pid = -1; // PID 先设为 -1，表示尚未正式分配进程标识号。
        proc->pgdir = boot_pgdir_pa; // 将根页表物理地址初始化为启动页表 boot_pgdir_pa；Lab4 内核线程共享内核页表。
        list_init(&proc->list_link); // 初始化普通进程链表节点，确保后续可安全插入 proc_list。
        list_init(&proc->hash_link); // 初始化 PID 哈希链表节点，确保后续可安全插入 hash_list 对应桶。
    } // 结束 proc 非空时的初始化分支。
    return proc; // 返回 PCB 指针；如果 kmalloc 失败则返回 NULL。
} // 函数体结束。

static int copy_mm(uint32_t clone_flags, struct proc_struct *proc) // 处理新线程的地址空间；Lab4 创建的是共享内核地址空间的内核线程。
{ // 函数体开始。
    // Lab4 creates kernel threads sharing the kernel page table, not user fork.；中文说明：说明当前实验不执行用户进程地址空间复制，而是创建共享内核地址空间的线程。
    assert(current->mm == NULL); // 断言当前执行实体没有独立用户地址空间；在 Lab4 内核线程场景下 current->mm 应为 NULL。
    proc->mm = NULL; // 将新线程 mm 设置为 NULL，表示它不拥有独立的用户虚拟地址空间结构。
    return 0; // 返回 0，表示本阶段地址空间处理成功；clone_flags 在当前实现中没有进一步使用。
} // 函数体结束。

static void copy_thread(struct proc_struct *proc, uintptr_t stack, struct trapframe *tf) // 把父执行现场整理到新线程自己的内核栈中，建立该线程首次运行所需的 trapframe 和 context。
{ // 函数体开始。
    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE - sizeof(*tf)); // 在新线程内核栈顶部预留一个 trapframe 大小的空间，并让 proc->tf 指向该空间。
    memcpy(proc->tf, tf, sizeof(*tf)); // 把调用者传入的 trapframe 完整复制到新线程内核栈，作为新线程初始寄存器现场。
    proc->tf->gpr.a0 = 0; // 将子线程返回值寄存器 a0 设为 0，遵循 fork 语义：子执行流看到的返回值为 0。
    proc->tf->gpr.sp = stack ? stack : proc->kstack + KSTACKSIZE; // 设置新线程初始栈指针；若调用者指定 stack 则使用它，否则使用新线程内核栈顶端。
    proc->context.ra = (uintptr_t)forkret; // 把 context.ra 设置为 forkret；首次切换到该线程时，switch_to 将返回到 forkret。
    proc->context.sp = (uintptr_t)proc->tf; // 把 context.sp 设置为 trapframe 地址，使 forkret/forkrets 能从该位置恢复新线程现场。
} // 函数体结束。

int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf) // 创建一个新进程/内核线程；成功返回新 PID，失败返回负错误码。
{ // 函数体开始。
    if (nr_process >= MAX_PROCESS) return -E_NO_FREE_PROC; // 检查当前进程数量是否达到 MAX_PROCESS；达到上限时返回无可用 PCB 的错误。
    struct proc_struct *proc = alloc_proc(); // 分配一个初始 PCB；此时字段处于 alloc_proc 建立的安全初始状态。
    if (proc == NULL) return -E_NO_MEM; // 若 PCB 分配失败，返回内存不足错误。
    proc->parent = current; // 记录父进程为当前进程；新线程由 current 创建，因此 parent 指向 current。
    int error = setup_kstack(proc); // 为新线程分配独立内核栈，并保存操作结果。
    if (error != 0) goto free_proc; // 若内核栈分配失败，跳到 free_proc 释放已经分配的 PCB。
    error = copy_mm(clone_flags, proc); // 处理新线程的地址空间；Lab4 的 copy_mm 只确认共享内核地址空间条件。
    if (error != 0) goto free_stack; // 若地址空间处理失败，跳到 free_stack 先释放内核栈，再释放 PCB。
    copy_thread(proc, stack, tf); // 把 trapframe 和 context 安放到新线程内核栈，准备首次运行现场。
    bool intr_flag; // 声明中断状态变量，用于保存进入临界区前的中断使能状态。
    local_intr_save(intr_flag); // 关闭本地中断，保护 PID 分配、哈希表和全局进程链表的更新过程。
    proc->pid = get_pid(); // 为新线程分配一个当前未使用的 PID。
    hash_proc(proc); // 把新线程插入 PID 哈希表，以便后续 find_proc(pid) 能够查找到它。
    list_add_before(&proc_list, &proc->list_link); // 把新线程的 list_link 插入全局 proc_list，供调度器遍历候选进程。
    ++nr_process; // 进程数量加 1，记录新线程已经登记到系统中。
    wakeup_proc(proc); // 唤醒新线程；wakeup_proc 通常会将其状态转为 PROC_RUNNABLE，使其具备被调度的资格。
    local_intr_restore(intr_flag); // 恢复进入临界区之前的中断状态。
    return proc->pid; // 返回新线程 PID，表示创建和登记流程成功。
free_stack: // 错误清理标签：地址空间处理失败时，从这里开始回收已经申请的内核栈。
    put_kstack(proc); // 释放新线程的内核栈页面。
free_proc: // 错误清理标签：释放 PCB；内核栈分配失败时也会直接跳到这里。
    kfree(proc); // 释放 alloc_proc 分配的 PCB 内存。
    return error; // 返回导致创建失败的错误码。
} // 函数体结束。

int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags) // 创建一个内核线程：将普通 C 函数 fn 和参数 arg 包装为新线程首次运行的寄存器现场。
{ // 函数体开始。
    struct trapframe tf; // 在当前栈上建立临时 trapframe；do_fork/copy_thread 会将它复制到新线程内核栈。
    memset(&tf, 0, sizeof(tf)); // 将临时 trapframe 清零，避免未初始化寄存器值影响新线程运行。
    tf.gpr.s0 = (uintptr_t)fn; // 把线程函数地址放入保存寄存器 s0，供 kernel_thread_entry 汇编入口取出并调用。
    tf.gpr.s1 = (uintptr_t)arg; // 把线程函数参数放入保存寄存器 s1，供 kernel_thread_entry 传给 fn。
    asm volatile("mv %0, gp" : "=r"(tf.gpr.gp)); // 读取当前全局指针 gp，并保存到新线程 trapframe，保证新线程使用正确的全局数据区基址。
    asm volatile("mv %0, tp" : "=r"(tf.gpr.tp)); // 读取当前线程指针 tp，并保存到新线程 trapframe，维持线程相关寄存器环境。
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE; // 设置 sstatus：SPP=1 表示异常返回到 S-mode；SPIE=1 使返回后中断使能状态按预期恢复；SIE 清零避免首次恢复前意外开中断。
    tf.epc = (uintptr_t)kernel_thread_entry; // 设置首次异常返回地址 epc 为 kernel_thread_entry，线程首次运行时从该汇编入口开始。
    return do_fork(clone_flags | CLONE_VM, 0, &tf); // 调用 do_fork 创建线程，并强制加入 CLONE_VM，表示共享当前内核地址空间。
} // 函数体结束。

void proc_init(void) // 初始化进程管理子系统，创建空闲线程 idleproc 和第一个普通内核线程 initproc。
{ // 函数体开始。
    list_init(&proc_list); // 初始化全局进程链表头，使其成为空的循环双向链表。
    for (size_t i = 0; i < HASH_LIST_SIZE; ++i) list_init(&hash_list[i]); // 逐个初始化 PID 哈希桶链表头，确保后续 hash_proc/find_proc 可以安全操作。
    nr_process = 0; // 清零进程数量统计。
    idleproc = alloc_proc(); // 分配 idleproc 的 PCB；idleproc 特殊之处在于它承接内核当前启动执行流。
    assert(idleproc != NULL); // 断言 idleproc 分配成功；若失败，系统无法建立基本的空闲执行实体。
    idleproc->pid = 0; // 将 idleproc 的 PID 固定为 0，作为系统空闲线程的保留标识。
    idleproc->state = PROC_RUNNABLE; // 将 idleproc 设为 PROC_RUNNABLE，使它处于可运行状态。
    idleproc->kstack = (uintptr_t)bootstack; // idleproc 直接使用启动汇编预先建立的 bootstack，不再额外分配内核栈。
    idleproc->need_resched = 1; // 设置 need_resched=1，使系统进入 cpu_idle 后尽快调用 schedule 寻找其他可运行线程。
    set_proc_name(idleproc, "idle"); // 将 idleproc 的名称设置为 idle，便于调试和输出识别。
    ++nr_process; // 已创建并登记 idleproc，进程数量加 1。
    current = idleproc; // 把 current 指向 idleproc，说明当前 CPU 正在沿用 idleproc 的执行现场。
    int pid = kernel_thread(init_main, "Hello world!!", 0); // 创建第一个普通内核线程 init_main，并把字符串 Hello world!! 作为线程参数传入。
    assert(pid == 1); // 断言新线程 PID 为 1；在当前初始化顺序中 idleproc 占用 PID 0，因此 initproc 应获得 PID 1。
    initproc = find_proc(pid); // 根据 PID 找回刚创建的线程 PCB，并保存到全局 initproc 指针。
    assert(initproc != NULL); // 断言查找成功，确保 initproc 已经加入 PID 哈希表。
    set_proc_name(initproc, "init"); // 将第一个普通内核线程命名为 init，便于调试和实验输出识别。
} // 函数体结束。



//LAB4 function3 2412396；中文说明：Lab4 进程调度/切换模块的实现标记；下面负责把 CPU 切换到指定进程。
void proc_run(struct proc_struct *proc) // proc 指向准备运行的目标进程/线程。
{ // 函数体开始。
    assert(proc != NULL && proc->state == PROC_RUNNABLE); // 目标 PCB 必须存在，并且状态必须为 PROC_RUNNABLE；否则不能切换过去执行。
    if (proc != current) { // 只有目标进程不是当前进程时才需要真正执行上下文切换。
        bool intr_flag; // 保存中断状态的局部变量。
        local_intr_save(intr_flag); // 关闭本地中断，避免切换 current、satp 和寄存器现场的过程中被中断打断。
        struct proc_struct *prev = current; // 保存切换前的当前进程指针；后面 switch_to 需要保存它的 context。
        current = proc; // 更新 current，使全局状态指向即将运行的目标线程。
        write_csr(satp, ((uintptr_t)8 << 60) | (proc->pgdir >> RISCV_PGSHIFT)); // 写入 satp：MODE 字段设为 8（Sv39），PPN 字段使用目标进程根页表物理地址右移页偏移位数后的页号。
        flush_tlb(); // 刷新 TLB，避免处理器继续使用切换前缓存的地址转换结果；本实验内核线程共享页表，但切换时仍统一刷新。
        switch_to(&prev->context, &proc->context); // 调用汇编 switch_to：保存 prev->context 并恢复 proc->context；新线程首次运行会从预设的 forkret 入口开始。
        local_intr_restore(intr_flag); // 当该线程以后再次被切回并从 switch_to 返回时，恢复切换前保存的中断状态。
    } // 结束 if 分支；若 proc 已经是 current，则无需重复切换。
} // 函数体结束。

void cpu_idle(void) // 空闲线程 idleproc 的主体函数；系统没有其他工作时会一直在这里等待调度。
{ // 函数体开始。
    while (1) { // 无限循环，保证空闲线程不会正常退出。
        if (current->need_resched) schedule(); // 当当前线程被标记为需要重新调度时，调用 schedule 选择下一个可运行线程。
    } // 结束无限循环体；该循环没有正常退出路径。
} // 函数体结束。
