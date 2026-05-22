# MIT xv6 Lab traps 分支实现说明

## 分支概览

`traps` 分支主要任务是给 xv6 添加一种"用户态定时回调"机制 —— 也就是 UNIX 里的 `signal(SIGALRM)` 的简化版。具体做了两个新系统调用：

- `sigalarm(int ticks, void (*handler)())`：让内核每隔 `ticks` 个 tick 自动跳到用户提供的 `handler` 执行一次。
- `sigreturn(void)`：在 `handler` 跑完之后，把控制流"还回"被打断的用户指令，且寄存器状态完全保留。

完成 `sigalarm/sigreturn` 等于把上一节 `syscall` lab 的"加系统调用八件套"再走一遍，但这次的难点不是流程，而是**在内核里安全地伪造一次用户态的函数调用并能完整地回得来**。这是理解信号、用户态调度、协程切换的关键一步。

本分支同时把 `syscall` 分支里加的 `trace` 系统调用清理掉了（编号 22 让给 `sigalarm`，23 留给 `sigreturn`），所以 `syscall.c` / `syscall.h` / `user.h` / `usys.pl` / `proc.h` / `proc.c` 里所有跟 `trace` 有关的代码都被替换掉了。

> 本分支已完成 alarm（test0 / test1 / test2 全部）。lab4 还有一节 backtrace（在 `kernel/printf.c` 里加一个回溯函数 + 在 `sys_sleep` 里调用），这部分**当前分支还没写**。下文重点解释 alarm。

---

## 背景知识：为什么 alarm 本质上是"在用户态被劫持一次"？

`sigalarm` 这种东西，初看是个普通的"定时器回调"，但落到 xv6 这种没有 libc、没有信号机制的小内核上，要实现它实际上要解决**一个非常根本的问题**：

> 内核运行在内核态，handler 运行在用户态。怎么让 CPU 从内核态"跳到"用户态的一个函数地址执行？又怎么"跳回来"？

下面把这件事拆细讲清楚。

### 1. 用户态唯一的入口：从 trap 返回

xv6 中**只有一种方式可以从内核态跑到用户态代码**：
走 `usertrapret → userret → sret`，由硬件根据 `sepc`（保存的用户 PC）跳回去执行。

所以"让用户进程跑某段用户代码"等价于"把它的 `trapframe->epc` 设成那段代码的入口地址，然后正常返回用户态"。

### 2. 那么"定时器 → 跳到 handler"应该怎么做？

xv6 的时钟中断本来就在每个 tick 进 `usertrap()`，进入这条分支：

```c
if(which_dev == 2)
    yield();
```

我们要做的事其实极简单 —— 在它走到 `yield()` 之前，做这么几件事：

1. 给当前进程的"tick 计数器"加 1；
2. 如果计数器达到了用户设定的 `alarm_interval`：
   - **把当前 trapframe 备份**（因为接下来要把 epc 改掉，原指令地址不能丢）；
   - **把 `trapframe->epc` 改成 `alarm_handler`**；
   - 重置计数器，标记"正在处理 alarm"。

之后 `usertrapret → userret → sret` 正常返回用户态，但因为 epc 被替换，CPU 跳到的不再是原指令，而是用户 handler 函数。

**整个"劫持"动作只是改了 trapframe 里的一个字段**。这就是 xv6 信号机制最关键的小聪明。

### 3. 跑完 handler，怎么"还原现场"？

handler 写完一行 `printf("alarm!\n")` 之后，**必须能回到被打断的那条用户指令**，并且：

- `pc` 回到原指令；
- 通用寄存器 a0~a7、s0~s11、t0~t6、ra、sp 都和当时一模一样；
- 否则被打断的程序就乱了（比如 a0 里原本算了一半的中间值被覆盖）。

xv6 内核**自己**没法在 handler 跑完那一刻插桩，怎么办？答案是：**让用户 handler 跑完后主动调用一个内核约定的系统调用 `sigreturn`，由内核去做"恢复现场"**。

也就是说：

```c
void periodic() {
    count = count + 1;
    printf("alarm!\n");
    sigreturn();   // ← 关键：用户必须自己写这一行
}
```

`sigreturn` 在内核里做的事就两件：

1. `memmove(p->trapframe, p->alarm_trapframe, sizeof(struct trapframe))` —— 把当初备份的 trapframe 整个覆盖回去；
2. 把 `is_alarming` 标志位清零，表示"alarm 处理完了，下一次 tick 可以重新计数"。

`sigreturn` 调完之后走 `usertrapret → userret → sret` 返回，这次 trapframe 里的 epc 已经是当初的原指令地址了，所有寄存器也都是原来的值 → CPU 跳回去继续执行 → 用户程序毫无察觉地继续往下跑。

### 4. 为什么 `sigreturn` 的返回值有讲究？

普通系统调用返回时 `syscall()` 会做：

```c
p->trapframe->a0 = syscalls[num]();
```

也就是说 `sys_xxx()` 的 C 函数返回值会被写进 `trapframe->a0`，作为系统调用的返回值。问题是：我们要恢复的"原现场"里 a0 可能有一个被打断时正在用的值（比如某个中间结果）。如果 `sys_sigreturn` 老老实实 `return 0`，那 a0 就被强行改成 0，原程序就崩了。

技巧是让 `sys_sigreturn` `return p->trapframe->a0` —— 也就是返回"刚才已经 memmove 恢复好的那个 a0"。这样 `syscall()` 写回 trapframe->a0 时写的还是原值，等于没动。这是个非常细节但非常重要的小手术，alarmtest 的 test1 / test2 就是专门测这件事的。

### 5. 为什么要 `is_alarming` 标志位？

考虑这种坏情况：

```
alarm_interval = 2
tick 0：进入 handler
tick 1：handler 还没跑完，又一次时钟中断
        → 又要触发一次 alarm
        → 把 trapframe 备份成"当前正在跑 handler"的状态
        → 备份污染了真正的"用户原现场"
        → sigreturn 之后回不到原指令了
```

也就是说，**alarm 不能"递归触发"**。`is_alarming` 就是这个互斥锁：进 handler 之前设 1，`sigreturn` 把它清 0。`usertrap` 里只有 `is_alarming == 0` 时才会计数和触发，从根本上避免重入。

### 6. 为什么 `alarm_trapframe` 要在 `allocproc` 里 `kalloc`、在 `freeproc` 里 `kfree`？

`alarm_trapframe` 是一段**用来存储 trapframe 快照的物理页**。它是进程级状态，每个进程独立持有一份；进程死掉时必须释放。所以申请/释放完全跟在 `allocproc/freeproc` 里就行 —— 这两个函数已经在做"进程级资源"的初始化和清理，加一行 `kalloc`、一行 `kfree` 就够，不需要新写"进程 alarm 初始化"。

### 7. alarm 和 trap 路径的关系图

```text
用户程序运行 (count = count + 1; ...)
   ↓ 时钟中断
uservec → usertrap
   ↓ which_dev == 2 (定时器)
   ↓ alarm_interval > 0 且 is_alarming == 0
   ↓ ticks_count++
   ↓ ticks_count == alarm_interval
   ┌────────────── 劫持发生 ──────────────┐
   │ memmove(alarm_trapframe, trapframe) │  备份现场
   │ trapframe->epc = alarm_handler      │  改返回地址
   │ ticks_count = 0; is_alarming = 1    │  标记正在处理
   └─────────────────────────────────────┘
   ↓ yield()  (照常让出 CPU)
   ↓ ...调度回来...
usertrapret → userret → sret
   ↓ sepc = handler 地址
跳到用户的 periodic() {
   count++;
   printf("alarm!\n");
   sigreturn();         ← ecall, SYS_sigreturn
}
   ↓
uservec → usertrap → syscall → sys_sigreturn
   ↓ memmove(trapframe, alarm_trapframe)
   ↓ is_alarming = 0
   ↓ return trapframe->a0      ← 注意不是 return 0
usertrapret → userret → sret
   ↓ sepc = 被打断的原指令
回到 count = count + 1 中间的下一条指令继续跑
```

这条路径走通了，alarm 就完事。下面看具体修改。

---

## 修改 1：进程结构体加 alarm 字段

文件：`kernel/proc.h`

```c
struct proc {
  ...
  char name[16];                       // Process name (debugging)
  int alarm_interval;                  // 闹钟间隔（用户 sigalarm 传入的 ticks）
  uint64 alarm_handler;                // 用户态 handler 地址
  int ticks_count;                     // 当前 tick 计数（每次时钟中断 +1）
  int is_alarming;                     // 是否正在执行 handler（防递归）
  struct trapframe *alarm_trapframe;   // 保存被中断时的 trapframe 快照
};
```

五个字段对应五件事：

| 字段 | 作用 |
| --- | --- |
| `alarm_interval` | 周期。`sigalarm(0, 0)` 表示关闭闹钟。 |
| `alarm_handler` | 用户 handler 函数地址。`usertrap` 把它写到 `trapframe->epc`。 |
| `ticks_count` | 累加 tick；达到 interval 就触发。 |
| `is_alarming` | 互斥锁，防 handler 重入。 |
| `alarm_trapframe` | 一页物理内存，保存被打断时的 32 个用户寄存器快照。 |

注意 `alarm_trapframe` 是指针，所以**必须先在 `allocproc` 里 `kalloc()` 出实际内存**，否则 `memmove` 时就会写坏内核。

---

## 修改 2：分配/释放 alarm_trapframe

文件：`kernel/proc.c::allocproc`

```c
//为alarm_trapframe分配内存
//当定时器触发、准备跳转到用户自定义的 handler 之前，
//操作系统需要用这个特殊的 trapframe 备份用户当前的寄存器状态（以便后续恢复）。
if((p->alarm_trapframe = (struct trapframe*)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
}

//初始化闹钟的控制状态字段
p->is_alarming = 0;
p->alarm_interval = 0;
p->alarm_handler = 0;
p->ticks_count = 0;
```

文件：`kernel/proc.c::freeproc`

```c
if(p->alarm_trapframe)
    kfree((void*)p->alarm_trapframe);
p->alarm_trapframe = 0;

p->is_alarming = 0;
p->alarm_interval = 0;
p->alarm_handler = 0;
p->ticks_count = 0;
```

要点：

- **`alarm_trapframe` 用整页**：`kalloc()` 一次返回 4KB，而 `struct trapframe` 不到 4KB，整页放足够；
- **失败要清理**：`kalloc` 返回 0 就要回滚（`freeproc` + 释放锁），否则进程会进入半构造状态；
- **`freeproc` 里要清零字段**：防止 `struct proc` 被复用时残留数据误触发 alarm。

---

## 修改 3：分配系统调用号

文件：`kernel/syscall.h`

```c
#define SYS_close  21
//给两个系统调用分配编号
#define SYS_sigalarm 22
#define SYS_sigreturn 23
```

22 是原来 `trace` 的位置，被替换；23 是新加的。两个号必须用户态和内核态约定一致。

---

## 修改 4：用户态声明 + stub

文件：`user/user.h`

```c
//两个实际执行的函数需要声明
int sigalarm(int ticks, void(*handler)());
int sigreturn(void);
```

文件：`user/usys.pl`

```perl
#这个文件会自动生成 user/usys.S，不用手动改 usys.S
entry("sigalarm");
entry("sigreturn");
```

每个 `entry("name")` 会被 Perl 脚本展开成下面这种 stub：

```asm
.global sigalarm
sigalarm:
 li a7, SYS_sigalarm
 ecall
 ret
```

用户调用 `sigalarm(2, periodic)` 时，编译器按 RISC-V 调用约定把 `2` 放进 `a0`、`periodic` 函数地址放进 `a1`，stub 再把 `22` 放进 `a7`，`ecall` 进内核。

---

## 修改 5：把 `sys_trace` 替换为 `sys_sigalarm` / `sys_sigreturn`

文件：`kernel/sysproc.c`

```c
uint64
sys_sigalarm(void)
{
  int ticks;
  uint64 handler;

  //从寄存器提取第0个参数（闹钟周期n）
  if(argint(0, &ticks) < 0)
     return -1;

  //提取第1个参数（用户定义的处理函数的内存地址）
  if(argaddr(1, &handler) < 0)
     return -1;

  struct proc *p = myproc();

  p->alarm_interval = ticks;
  p->alarm_handler = handler;
  p->ticks_count = 0;
  p->is_alarming = 0;

  return 0;
}
```

`argint(0, ...)` 拿 `trapframe->a0`（间隔），`argaddr(1, ...)` 拿 `trapframe->a1`（用户函数地址）。

注意这里**没有验证 `handler` 这个地址是否合法**（比如是否在用户地址空间内）。xv6 的简化策略是：handler 错了就让用户自己 fault。

```c
//当用户定义的 handler 执行完毕后，必须调用这个系统调用来回到被中断打断的原始代码行。
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  //将之前备份在 alarm_trapframe 里的所有寄存器状态，原封不动地覆盖回当前的 p->trapframe
  memmove(p->trapframe, p->alarm_trapframe, sizeof(struct trapframe));

  //将标志位重新设为0，这相当于解锁
  p->is_alarming = 0;

  //如果直接返回 0，内核会自动把 a0 改成 0，这可能会破坏被中断程序原本在 a0 里的值
  return p->trapframe->a0;
}
```

最后一行 `return p->trapframe->a0` 是 alarm lab **最容易翻车**的一行。`syscall()` 在分发完之后会做：

```c
p->trapframe->a0 = syscalls[num]();
```

也就是说 sys_xxx 的返回值**一定**会被写进 `trapframe->a0`。所以这里返回什么，最后用户的 a0 就被改成什么。我们刚刚 memmove 把 a0 恢复成"原现场的 a0"了，要让它**保持不变**，唯一的写法就是 `return trapframe->a0` —— 自己读自己写一遍，等于什么也没动。

如果写成 `return 0`，test1 立刻报错："i != j"：因为 a0 被偷偷篡改，被打断的算式算出了错的值。

---

## 修改 6：分发表更新

文件：`kernel/syscall.c`

```c
extern uint64 sys_sigalarm(void);
extern uint64 sys_sigreturn(void);

static uint64 (*syscalls[])(void) = {
  ...
  [SYS_close]   sys_close,
  [SYS_sigalarm] sys_sigalarm,
  [SYS_sigreturn] sys_sigreturn,
};
```

注意：上一节 `syscall` lab 里加的 `syscalls_name[]`、`trace_mask` 相关代码都被移除掉了，回到了"只分发不打印"的标准 `syscall()`。

---

## 修改 7：`usertrap` 里识别定时器并触发 alarm

文件：`kernel/trap.c`

```c
// give up the CPU if this is a timer interrupt.
// 2代表时钟中断
if(which_dev == 2){
    //检查用户是否开启了闹钟功能 + 如果当前进程已经在执行闹钟处理函数了，
    //就停止计数，直到它执行完 sigreturn
    if(p->alarm_interval > 0 && p->is_alarming == 0){
        p->ticks_count++;

        //当计数达到用户设定的阈值时，触发闹钟。
        if(p->ticks_count >= p->alarm_interval){
            //在跳转之前，把当前的寄存器快照（p->trapframe）备份到我们申请好的 alarm_trapframe 中
            memmove(p->alarm_trapframe, p->trapframe, sizeof(struct trapframe));

            //epc决定了当 CPU 从内核态返回用户态时，第一条指令从哪里开始执行。
            //通过把它改成 alarm_handler 的地址，CPU 下一秒就会跑去执行用户写的处理函数。
            p->trapframe->epc = p->alarm_handler;

            p->ticks_count = 0;
            p->is_alarming = 1;
        }
    }
    yield();
}
```

这就是**整个 alarm 机制的关键 12 行**。要注意的点：

1. **`alarm_interval > 0` 当作开关**：用户 `sigalarm(0, 0)` 等于关掉 alarm，这里就不会进里层逻辑；
2. **`is_alarming == 0` 防递归**：handler 跑期间，再来时钟中断也不会再次劫持；
3. **先 memmove 再改 epc**：顺序不能反，否则备份里就已经是 handler 地址，回不去了；
4. **`memmove` 拷整个 `struct trapframe`**：这是为什么 a0~a7、s0~s11、t0~t6、ra、sp、epc 等全部被精准保存——拷的是整页内容；
5. **`yield()` 仍然要调用**：因为定时器中断的副作用就是让 CPU 让出，alarm 只是顺路在让出之前做了一次劫持。

---

## 修改 8：清理 fork 中的 `trace_mask` 继承

文件：`kernel/proc.c::fork`

```c
//在分配和初始化子进程np后
//将trace_mask拷贝到子进程
np->trace_mask = p->trace_mask;          ← 这一行被删除
```

`trace_mask` 已经从 `proc` 结构里去掉了，所以 fork 中的继承也跟着删。注意 **alarm 字段不需要在 fork 中继承**：测试中并不要求 fork 后的子进程继承父进程的闹钟设置，所以保持 `allocproc` 里的默认 0 即可。

---

## 修改 9：清掉 `trace` 相关用户程序

被删除的文件：

- `user/trace.c`
- `user/sysinfotest.c`（syscall lab 留下的 sysinfo 占位程序）
- `kernel/sysinfo.h`

被加进来的文件：

- `user/alarmtest.c`：alarm lab 的标准测试程序，包含 `test0/test1/test2`；
- `user/call.c`：lab 第一节"RISC-V assembly"那段需要 `objdump` 看汇编的样例代码；
- `user/bttest.c`：backtrace 的占位测试程序（这里只是 `sleep(1); exit(0)`，因为本分支还没做 backtrace）；
- `user/track.c`：空占位文件。

---

## 修改 10：scheduler 关掉 WFI（针对 LAB_FS）

文件：`kernel/proc.c::scheduler`

```c
#if !defined (LAB_FS)
if(found == 0) {
  intr_on();
  asm volatile("wfi");
}
#else
;
#endif
```

`wfi` 是 RISC-V 的"wait for interrupt"指令，让 CPU 停下等中断节省功耗。某些 lab 环境下（fs lab）的 grade 脚本会因为 wfi 时间不准确导致计时偏差，所以加了一个条件编译开关。这一段不是 traps lab 任务，是 upstream 仓库合并进来顺带带上的。

---

## 用户程序运行时的完整数据流

以 `alarmtest test0` 为例：

```text
sigalarm(2, periodic)
   ↓ usys stub: li a7, SYS_sigalarm; ecall
sys_sigalarm():
   p->alarm_interval = 2
   p->alarm_handler  = periodic 的地址
   p->ticks_count    = 0
   p->is_alarming    = 0
   ↓ return 用户态
进入 for 循环 (1000*500000 次)
   ↓
某次 tick 进 usertrap (which_dev == 2)
   ticks_count = 1  → 不够，照常 yield
   ↓ ...再来一 tick...
   ticks_count = 2  → 达到阈值
      ┌──────劫持──────┐
      │ memmove 备份    │
      │ epc = periodic │
      │ ticks_count = 0│
      │ is_alarming = 1│
      └────────────────┘
   yield → 调度回来
   sret → 跳到 periodic
periodic() {
   count++;
   printf("alarm!\n");
   sigreturn();   ← ecall SYS_sigreturn
}
   ↓
sys_sigreturn():
   memmove(trapframe, alarm_trapframe)  // 把现场拷回来
   is_alarming = 0
   return trapframe->a0                 // 保持 a0 不变
   ↓ sret
回到 test0 的 for 循环原指令继续跑
   ↓ 不久 if(count > 0) break;
sigalarm(0, 0) → 关掉闹钟
打印 "test0 passed"
```

---

## 几个高频疑问

**Q1：`alarm_trapframe` 为什么不直接放在 `struct proc` 里，要单独 `kalloc`？**

`struct proc` 是有大小限制的内核数据结构，每个进程都要常驻一份。`struct trapframe` 本身将近 300 字节，再算上别的状态会让 `struct proc` 体积膨胀；而 trampoline 已经规定 trapframe 必须是一整页，所以把 alarm 备份也单独 `kalloc` 一页是最干净的做法。

**Q2：handler 跑完了直接 `return` 不行吗？**

不行。handler 是被 epc 跳过去的，**调用约定上没有 ra**（ra 是上一段被打断代码的 ra，不是 handler 的）。`return` 会 `jr ra`，跳回去的是被打断的"原 ra"，但 sp 是 handler 自己用的栈帧，整个寄存器视图完全错位，立即崩。所以必须靠 `sigreturn` 系统调用让内核来做"原子级现场恢复"。

**Q3：handler 内部如果再 `sigalarm(0,0)` 关掉闹钟，会有事吗？**

不会。`sigalarm(0,0)` 只是把 `alarm_interval` 设成 0，等 handler 跑完 `sigreturn` 把 `is_alarming` 清 0 之后，下一次 tick 会因为 `alarm_interval > 0` 不成立而不再触发。安全。

**Q4：如果 handler 永远不调用 `sigreturn`，会发生什么？**

`is_alarming` 永远是 1，alarm 永远不会再触发；同时 `alarm_trapframe` 里的原现场永远丢了，被打断的指令永远回不去 —— 程序大概率会一直循环执行 handler 末尾的 `ret` 跑出未知地址。所以 alarmtest 的所有 handler 都必须以 `sigreturn()` 结尾。

**Q5：为什么 `sigalarm(0, 0)` 关闭闹钟的语义"自动生效"？**

因为 `usertrap` 里第一行就是 `if(p->alarm_interval > 0 && ...)`，`alarm_interval` 一变 0，触发条件直接整段跳过。同时 `ticks_count` 也清 0，下次开启不会用上一次累积的计数。

---

## 核心思想

alarm lab 的全部"魔法"集中在两个动作上：

1. **下行劫持**：`usertrap` 改 `trapframe->epc`，让用户态返回时跳到任意函数；
2. **上行还原**：`sigreturn` 把整页 trapframe `memmove` 覆盖回去，连 a0 都保持不动。

这种"通过修改 trapframe 实现用户态行为劫持"的套路是真实操作系统中信号机制、setjmp/longjmp、用户态协程切换的**统一原理**。比如 Linux 内核也是用类似办法把 `pt_regs->pc` 改成 `__kernel_rt_sigreturn`，再让用户 handler 跑完返回时跌入它从而进入 `sys_rt_sigreturn`。区别只是 Linux 把"用户必须显式调用 sigreturn"这一点用 libc 自动塞了一段 trampoline 而已。

理解了 alarm，再去读 Linux 信号、协程库（如 ucontext、boost::context），其实都是同一棵树长出来的。

---

## 未完成部分：Backtrace

lab4 的 task 2 是 **Backtrace**：在 `kernel/printf.c` 里写一个函数遍历当前内核栈帧链，把每一帧的返回地址打印出来；并在 `sys_sleep` 里调一下 `backtrace()`，配合 `kernel/printf.c::panic` 也调用一下，便于内核出错时排查。

实现思路（备忘）：

```c
// kernel/riscv.h
static inline uint64 r_fp() {
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x));   // s0/fp 寄存器
  return x;
}

// kernel/printf.c
void backtrace(void) {
  uint64 fp = r_fp();
  uint64 stack_top = PGROUNDUP(fp);    // 内核栈每页一帧
  while(fp < stack_top){
    uint64 ra = *(uint64*)(fp - 8);   // 调用者返回地址
    printf("%p\n", ra);
    fp = *(uint64*)(fp - 16);          // 上一帧 fp
  }
}
```

关键是 RISC-V 的栈帧布局：

```
高地址
  ┌─────────┐
  │  ...    │
  │ ra      │ ← fp - 8   保存调用者返回地址
  │ prev_fp │ ← fp - 16  保存调用者 fp
  │  ...    │ ← fp       本帧基址
  └─────────┘
低地址
```

`kernel/riscv.h` 加 `r_fp()`、`kernel/defs.h` 加 `backtrace()` 声明、`kernel/printf.c` 实现、`sys_sleep` 里加 `backtrace();` —— 即可通过 bttest。本分支等后续补。

---

## 总结

`traps` 分支完成的核心任务是 alarm。它表面看是"加两个系统调用"，本质是**第一次主动改 trapframe 来劫持用户态执行流**。整条改动按一行一意分布：

| 位置 | 改了什么 | 意图 |
| --- | --- | --- |
| `proc.h` | 新增 5 个 alarm 字段 | 进程级 alarm 状态 |
| `proc.c::allocproc` | `kalloc(alarm_trapframe)` + 字段清零 | 申请备份页、初始化 |
| `proc.c::freeproc` | `kfree(alarm_trapframe)` + 字段清零 | 释放备份页 |
| `syscall.h` | `SYS_sigalarm=22`、`SYS_sigreturn=23` | 系统调用号约定 |
| `user/user.h` + `usys.pl` | 加用户态声明和 stub | 用户态入口 |
| `sysproc.c::sys_sigalarm` | 把参数存进 PCB | 内核接收设置 |
| `sysproc.c::sys_sigreturn` | `memmove` 恢复现场 + `return a0` | 内核还原现场 |
| `syscall.c` | 改 `syscalls[]`、删 `trace` 相关 | 分发表 |
| `trap.c::usertrap` | 时钟中断里改 `trapframe->epc` | 触发劫持 |

最关键的两行代码是：

```c
// usertrap: 把用户即将返回的目标地址改成 handler
p->trapframe->epc = p->alarm_handler;

// sys_sigreturn: 整页拷回，连 a0 都不能动
memmove(p->trapframe, p->alarm_trapframe, sizeof(struct trapframe));
return p->trapframe->a0;
```

这两行就是 alarm 全部的灵魂。

