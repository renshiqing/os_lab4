/* Appended only to the disposable copy of proc.c by run_validation.py. */
extern int lab4_check_switch_registers(int id, int rounds);
static volatile int probe_steps[3], probe_done[3];
static struct proc_struct *probe_workers[2], *probe_sleep, *probe_zombie;

void lab4_worker_yield(int id)
{
    assert(id == 1 || id == 2);
    ++probe_steps[id];
    current->need_resched = 1;
    schedule();
    assert(current == probe_workers[id - 1]);
}

static int probe_worker(void *argument)
{
    int id = (int)(intptr_t)argument;
    assert(lab4_check_switch_registers(id, 25) == 0);
    probe_done[id] = 1;
    current->state = PROC_SLEEPING;
    schedule();
    panic("A sleeping worker was scheduled");
}

void lab4_proc_probe(void)
{
    struct proc_struct *blank = alloc_proc();
    assert(blank != NULL && blank->state == PROC_UNINIT && blank->pid == -1);
    assert(blank->pgdir == boot_pgdir_pa && blank->mm == NULL && blank->parent == NULL);
    assert(blank->tf == NULL && blank->kstack == 0 && blank->runs == 0 && blank->flags == 0);
    assert(!blank->need_resched && list_empty(&blank->list_link) && list_empty(&blank->hash_link));
    for (size_t i = 0; i < sizeof(blank->context); ++i) assert(((unsigned char *)&blank->context)[i] == 0);
    for (size_t i = 0; i < sizeof(blank->name); ++i) assert(blank->name[i] == 0);
    kfree(blank);
    int process_count = nr_process;
    size_t free_before = nr_free_pages();
    list_entry_t *first = list_next(&proc_list), *last = list_prev(&proc_list);
    for (int failure = 1; failure <= 3; ++failure) {
        probe_fail_pcb = failure == 1;
        probe_fail_stack = failure == 2;
        probe_fail_mm = failure == 3;
        assert(kernel_thread(probe_worker, (void *)1, 0) == -E_NO_MEM);
        assert(nr_process == process_count && nr_free_pages() == free_before);
        assert(list_next(&proc_list) == first && list_prev(&proc_list) == last);
    }
    probe_fail_pcb = probe_fail_stack = probe_fail_mm = 0;
    nr_process = MAX_PROCESS;
    assert(kernel_thread(probe_worker, (void *)1, 0) == -E_NO_FREE_PROC);
    nr_process = process_count;
    assert(nr_free_pages() == free_before);
    cprintf("Lab4 extra: PCB defaults, PCB/stack/mm failures and process-limit rollback PASS\n");

    int pid = kernel_thread(probe_worker, (void *)1, 0);
    probe_sleep = find_proc(pid);
    probe_sleep->state = PROC_SLEEPING;
    pid = kernel_thread(probe_worker, (void *)1, 0);
    probe_zombie = find_proc(pid);
    probe_zombie->state = PROC_ZOMBIE;
    for (int i = 0; i < 2; ++i) {
        pid = kernel_thread(probe_worker, (void *)(intptr_t)(i + 1), 0);
        probe_workers[i] = find_proc(pid);
        struct proc_struct *p = probe_workers[i];
        assert(p && p->parent == idleproc && p->mm == NULL && p->pgdir == boot_pgdir_pa);
        assert(p->context.ra == (uintptr_t)forkret && p->context.sp == (uintptr_t)p->tf);
        assert(p->tf == (struct trapframe *)(p->kstack + KSTACKSIZE - sizeof(*p->tf)));
        assert((p->context.sp & 15) == 0 && (p->tf->gpr.sp & 15) == 0);
        assert(p->tf->gpr.sp == p->kstack + KSTACKSIZE && p->tf->gpr.a0 == 0);
        assert(p->tf->epc == (uintptr_t)kernel_thread_entry && p->tf->gpr.s0 == (uintptr_t)probe_worker);
        assert(p->tf->gpr.s1 == (uintptr_t)(i + 1));
        assert((p->tf->status & (SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SIE)) == (SSTATUS_SPP | SSTATUS_SPIE));
        for (int j = 0; j < i; ++j) assert(p->kstack != probe_workers[j]->kstack);
    }
    cprintf("Lab4 extra: unique PID/hash, parent, independent aligned stacks and first-run frame PASS\n");
}

void lab4_schedule_probe(void)
{
    for (int i = 0; i < 35; ++i) {
        volatile uintptr_t stack_cookie = 0x2412396000UL + i;
        current->need_resched = 1;
        schedule();
        assert(current == initproc && stack_cookie == 0x2412396000UL + i);
    }
    assert(probe_steps[1] == 25 && probe_steps[2] == 25);
    assert(probe_done[1] && probe_done[2]);
    assert(probe_sleep->runs == 0 && probe_zombie->runs == 0);
    assert(probe_workers[0]->runs >= 25 && probe_workers[1]->runs >= 25);
    cprintf("Lab4 extra: two workers x 25 yields, s0-s11/sp/ra and stack sentinels PASS\n");
    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *saved_current = current;
    enum proc_state saved_state = initproc->state;
    int idle_runs = idleproc->runs;
    initproc->state = PROC_SLEEPING;
    current = idleproc;
    schedule();
    assert(current == idleproc && idleproc->runs == idle_runs + 1);
    current = saved_current;
    initproc->state = saved_state;
    int runs = current->runs;
    schedule();
    assert(current == saved_current && current->runs == runs + 1);
    assert((read_csr(sstatus) & SSTATUS_SIE) == 0);
    proc_run(current);
    assert((read_csr(sstatus) & SSTATUS_SIE) == 0);
    set_csr(sstatus, SSTATUS_SIE);
    schedule();
    assert(current == saved_current && (read_csr(sstatus) & SSTATUS_SIE));
    clear_csr(sstatus, SSTATUS_SIE);
    local_intr_restore(intr_flag);
    cprintf("Lab4 extra: skip sleeping/zombie, synthetic idle fallback, self-selection and IRQ state PASS\n");
    cprintf("Lab4 extra: ALL PASS\n");
}
