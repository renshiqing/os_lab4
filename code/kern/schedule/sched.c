#include <list.h>   // 引入双向循环链表操作接口，用于遍历全局进程链表 proc_list。
#include <sync.h>   // 引入同步与中断控制接口，例如 local_intr_save 和 local_intr_restore。
#include <proc.h>   // 引入进程管理相关定义，包括 proc_struct、proc_state、current 等。
#include <sched.h>  // 引入调度器相关函数和接口声明。
#include <assert.h> // 引入 assert 断言，用于检查进程状态是否满足函数执行条件。

// ==========================================================
// 唤醒进程：将指定进程从非运行状态转换为就绪状态
// ==========================================================

void
wakeup_proc(struct proc_struct *proc) { // 接收一个进程控制块指针，表示需要唤醒的进程。
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    // 使用断言检查进程状态：
    // 1. 不能是 PROC_ZOMBIE（僵尸状态），因为进程已经结束，不能再被唤醒。
    // 2. 不能是 PROC_RUNNABLE（就绪/运行状态），因为该进程已经具备运行条件。
    // 如果不满足条件，说明调用者的状态管理存在错误。

    proc->state = PROC_RUNNABLE;
    // 将进程状态设置为 PROC_RUNNABLE，表示该进程已经可以参与调度。
    // 注意：这里仅修改进程状态，不负责立即切换 CPU 执行权。
} // wakeup_proc 函数结束。

// ==========================================================
// Lab4 功能模块三：进程调度
// 学号：2412396
// ==========================================================

// schedule()：从全局进程链表中选择下一个可运行进程，并完成调度。
void schedule(void)
{
    bool intr_flag;
    // 定义中断状态标志变量，用于保存执行调度前的本地中断状态。
    // 后续恢复中断时，需要根据该变量恢复原来的状态。

    local_intr_save(intr_flag);
    // 关闭本地中断，并将关闭前的中断状态保存到 intr_flag。
    // 这样可以防止调度过程中被中断打断，保证进程状态和链表操作的原子性。

    current->need_resched = 0;
    // 清除当前进程的重新调度标志。
    // 表示当前这次调度请求已经开始处理。

    struct proc_struct *next = NULL;
    // 定义 next 指针，用于保存调度器最终选出的下一个可运行进程。
    // 初始设置为 NULL，表示暂时还没有找到合适的进程。

    list_entry_t *start = current == idleproc ? &proc_list : &current->list_link;
    // 确定本次遍历进程链表的起始位置：
    // 如果当前进程是 idleproc，则从全局进程链表头 proc_list 开始查找。
    // 如果当前进程不是 idleproc，则从当前进程对应的链表节点开始查找。
    // 这样可以让普通进程从当前进程之后继续寻找，实现轮转调度。

    list_entry_t *le = start;
    // 定义链表遍历指针 le，并将其初始化为起始位置 start。
    // 后续通过 list_next() 沿着双向循环链表向后遍历。

    do {
        // 开始执行链表遍历。
        // 使用 do-while，保证至少向后移动一个节点后再进行判断。

        le = list_next(le);
        // 将 le 移动到当前节点的下一个节点。
        // 如果已经到达链表末尾，循环链表会自动回到链表头。

        if (le != &proc_list) {
            // 判断当前节点是否为全局链表头。
            // proc_list 是链表的哨兵节点，不对应任何实际进程。
            // 只有非链表头节点才可以转换为进程控制块。

            struct proc_struct *candidate = le2proc(le, list_link);
            // 使用 le2proc 宏，根据当前链表节点 le 和成员 list_link，
            // 反向获取该节点所属的进程控制块 proc_struct。
            // candidate 表示当前正在检查的候选进程。

            if (candidate->state == PROC_RUNNABLE) {
                // 检查候选进程的状态是否为 PROC_RUNNABLE。
                // 只有处于就绪/运行状态的进程才有资格被调度执行。

                next = candidate;
                // 找到符合条件的可运行进程，将其保存到 next 指针中。

                break;
                // 已经找到下一个可运行进程，结束链表遍历。
            }
            // 结束候选进程状态判断。
        }
        // 结束链表头节点判断。

    } while (le != start);
    // 如果当前遍历节点尚未回到起始位置，则继续遍历。
    // 当 le 再次等于 start 时，说明已经完整扫描了一轮链表。
    // 如果没有找到可运行进程，就结束循环。

    if (next == NULL) next = idleproc;
    // 如果完整遍历后仍然没有找到可运行进程，
    // 则将系统空闲线程 idleproc 作为后备执行进程。
    // 保证系统始终有一个可运行的执行实体。

    ++next->runs;
    // 将被选中进程的运行次数加 1。
    // runs 用于记录该进程被调度执行的次数，可用于运行统计。

    if (next != current) proc_run(next);
    // 如果选出的进程与当前进程不同，则调用 proc_run() 完成进程切换。
    // proc_run() 负责更新 current、切换页表并调用 switch_to() 保存和恢复上下文。
    // 如果 next 就是 current，则不需要进行实际的上下文切换。

    local_intr_restore(intr_flag);
    // 恢复进入调度函数之前的本地中断状态。
    // 如果调用前中断是开启的，则重新开启；如果原本关闭，则继续保持关闭。
    // 至此，本次进程调度过程完成。

} // schedule() 函数结束。
