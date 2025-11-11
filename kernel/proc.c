#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define MLFQ_DEBUG 1

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc_queue queues[NQUEUE]; // Q0, Q1, Q2
struct spinlock queue_locks[NQUEUE]; // 每个队列的锁

int promote_needed = 0;
struct spinlock promote_lock;
// 将进程添加到队列尾部
void enqueue_proc(struct proc *p, int level) {
  acquire(&queue_locks[level]);
  if(p==0);
  else if (queues[level].head == 0) {
    // 队列为空
    queues[level].head = p;
    queues[level].tail = p;
    p->next_in_queue = 0;
  } else {
    // 添加到队列尾部
    queues[level].tail->next_in_queue = p;
    queues[level].tail = p;
    p->next_in_queue = 0;
  }
  
  release(&queue_locks[level]);
}

// 从队列头部移除进程
struct proc* dequeue_proc(int level) {
  acquire(&queue_locks[level]);
  
  struct proc *p = queues[level].head;
  if (p != 0) {
    queues[level].head = p->next_in_queue;
    if (queues[level].tail == p) {
      queues[level].tail = 0;
    }
    p->next_in_queue = 0;
  }
  
  release(&queue_locks[level]);
  return p;
}

// 从队列中移除特定进程（用于进程状态变化时）
void remove_from_queue(struct proc *p, int level) {
  acquire(&queue_locks[level]);
  if(p==0);
  else if (queues[level].head == p) {
    // 进程在队列头部
    queues[level].head = p->next_in_queue;
    if (queues[level].tail == p) {
      queues[level].tail = 0;
    }
  } else {
    // 在队列中间查找
    struct proc *prev = queues[level].head;
    while (prev != 0 && prev->next_in_queue != p) {
      prev = prev->next_in_queue;
    }
    if (prev != 0) {
      prev->next_in_queue = p->next_in_queue;
      if (queues[level].tail == p) {
        queues[level].tail = prev;
      }
    }
  }
  p->next_in_queue = 0;
  
  release(&queue_locks[level]);
}

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }

  for(int i = 0; i < 3; i++) {
    queues[i].head = 0;
    queues[i].tail = 0;
    initlock(&queue_locks[i], "queue_lock");
  }
  
  initlock(&promote_lock, "promote_lock");
}


void
promote_processes(void) {//TODO
  // 提升Q1和Q2中的所有进程到Q0
  for(int level = 1; level <= 2; level++) {
    struct proc *to_promote_head = 0;
    struct proc *to_promote_tail = 0;

    // 在持有队列锁的情况下，从队列中摘出需要提升的进程，
    // 并把它们串成一个临时链表 (使用 next_in_queue 字段)。
    acquire(&queue_locks[level]);
    struct proc *prev = 0;
    struct proc *current = queues[level].head;
    while(current != 0) {
      struct proc *next = current->next_in_queue;
      // 只在队列锁下检查并移除节点，不获取 p->lock，避免死锁
      if(current->state == RUNNABLE) {
        // 从队列中摘除 current
        if(prev == 0) {
          queues[level].head = next;
        } else {
          prev->next_in_queue = next;
        }
        if(queues[level].tail == current) {
          queues[level].tail = prev;
        }

        // 把 current 插入到本地待提升链表尾部
        current->next_in_queue = 0;
        if(to_promote_tail == 0) {
          to_promote_head = to_promote_tail = current;
        } else {
          to_promote_tail->next_in_queue = current;
          to_promote_tail = current;
        }
        // prev 不变，因为 current 已被移除
      } else {
        prev = current;
      }
      current = next;
    }
    release(&queue_locks[level]);

    // 现在对每个要提升的进程获取 p->lock，更新其队列信息并加入 Q0。
    current = to_promote_head;
    while(current != 0) {
      struct proc *nxt = current->next_in_queue;
      acquire(&current->lock);
      // 可能状态在此间发生变化，再次确认
      if(current->state == RUNNABLE) {
  int old_level = current->queue_level;
  current->queue_level = 0;
  current->remaining_ticks = Q0_TICKS;
#if MLFQ_DEBUG
  printf("[PROMOTE] PID %d promoted from level %d to 0\n", current->pid, old_level);
#endif
      }
      release(&current->lock);

      // 将其加入 Q0 队列（enqueue_proc 会获取 Q0 的队列锁）
      enqueue_proc(current, 0);

      current = nxt;
    }

    // 还要提升正在 RUNNING 的进程（它们不在队列中），但不提升 SLEEPING 的进程。
    // 这些进程不会被加入队列（因为它们当前在运行），但将它们的 queue_level 设置为 0
    // 以便下一次让出 CPU 回到 Q0。
    for(struct proc *pp = proc; pp < &proc[NPROC]; pp++) {
      acquire(&pp->lock);
      if(pp->state == RUNNING && pp->queue_level > 0) {
#if MLFQ_DEBUG
  printf("[PROMOTE] PID %d (RUNNING) promoted from level %d to 0\n", pp->pid, pp->queue_level);
#endif
  pp->queue_level = 0;
  pp->remaining_ticks = Q0_TICKS;
      }
      release(&pp->lock);
    }
  }
}


// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;


  p->queue_level = 0;           // 新进程从Q0开始
  p->remaining_ticks = Q0_TICKS; // Q0时间片为3 ticks
  p->original_queue = 0;        // 原始队列为Q0
  enqueue_proc(p, 0);


  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();//TODO


    acquire(&promote_lock);
    if (promote_needed) {
      promote_needed = 0;
      release(&promote_lock);
      promote_processes();
    } else {
      release(&promote_lock);
    }


    int found = 0;
     // 按优先级顺序检查队列：Q0 → Q1 → Q2
    for(int level = 0; level < NQUEUE && !found; level++) {
      // 尝试从当前队列获取一个进程
      p = dequeue_proc(level);
      if(p != 0) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          // 运行进程
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);
          
          // 进程运行结束或让出CPU
          c->proc = 0;
          // 如果进程仍然是RUNNABLE状态，将其放回队列尾部（轮转调度）
          if(p->state == RUNNABLE) {//TODO
            enqueue_proc(p, p->queue_level);
          }
          found = 1;
          release(&p->lock);
        }
      }
    }
    
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  p->original_queue = p->queue_level;
  remove_from_queue(p, p->queue_level);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;

        p->queue_level = p->original_queue;
        switch(p->queue_level) {
          case 0: p->remaining_ticks = Q0_TICKS; break;
          case 1: p->remaining_ticks = Q1_TICKS; break;
          case 2: p->remaining_ticks = Q2_TICKS; break;//TODO
        }
        enqueue_proc(p, p->queue_level);

      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
