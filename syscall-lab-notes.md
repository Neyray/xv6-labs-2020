# MIT xv6 Lab syscall 分支实现说明

## 分支概览

`syscall` 分支的核心任务是给 xv6 加一条新的系统调用 `trace`。它的作用是给进程设置一个系统调用追踪掩码，使进程后续执行匹配的系统调用时，内核自动打印追踪信息。

完成 `trace` 等于走完了"添加一个新系统调用"的完整链路：用户态声明 → 用户态 stub → 系统调用号 → 内核分发表 → 内核处理函数 → 进程级状态保存 → `fork` 中继承 → 系统调用返回时打印。

分支里还有 `kernel/sysinfo.h` 中的 `struct sysinfo`，但 `sysinfo` 系统调用链路并未补全，所以本文最后单独说明。

---

# 第一部分：系统调用的完整内核流程

理解这个 lab 的前提，是理解"用户程序怎么通过 `ecall` 进入内核，内核怎么处理，又怎么把结果送回用户态"。下面从 RISC-V 硬件出发，把整条路径走一遍。

## 1. 用户态视角：调用一个系统调用发生了什么？

用户代码里写：

```c
int n = read(fd, buf, sizeof(buf));
```

实际编译后调用的不是内核里的 `sys_read`，而是 `user/usys.pl` 生成的汇编 stub：

```asm
.global read
read:
 li a7, SYS_read    # 把系统调用号塞进 a7
 ecall              # 触发 trap，从用户态陷入内核
 ret
```

按照 RISC-V 调用约定：

- 参数依次放在 `a0, a1, a2, ...`；
- 返回值放在 `a0`；
- 系统调用号约定放在 `a7`。

`ecall` 是 RISC-V 指令，执行后硬件会：

1. 把当前 `pc` 保存到 `sepc`；
2. 把异常原因写进 `scause`（值 8 表示"用户态 ecall"）；
3. 关闭中断；
4. 把 `pc` 跳到 `stvec`，开始执行 trap 处理代码。

注意 `ecall` 本身**不会切页表，也不会自动保存所有寄存器**。这两件事都得由内核自己安排。

## 2. trampoline、trapframe、sscratch：用户态陷阱的"过渡设备"

用户态发生 trap 时，CPU 还在使用用户页表。但用户页表通常不映射内核代码和内核栈，所以内核无法立刻"跳到 `usertrap()` 里"。

xv6 用三样东西解决这个问题：

| 名字 | 作用 |
| --- | --- |
| `trampoline` 页 | 一段汇编（`uservec` + `userret`），同时映射到**用户页表**和**内核页表**中，并且**两边虚拟地址相同**（`TRAMPOLINE`）。这样切换 `satp` 时当前 PC 仍然有效。 |
| `trapframe` 页 | 每进程一份，专门用来保存用户态寄存器现场，并保存内核入口所需信息（kernel_satp / kernel_sp / kernel_trap / kernel_hartid / epc）。它在用户页表里也映射到固定虚拟地址 `TRAPFRAME`。 |
| `sscratch` 寄存器 | RISC-V 提供的一个"工具寄存器"。进入用户态前，xv6 把它设为当前进程 `trapframe` 的虚拟地址。这样 `uservec` 一进入内核就可以靠它腾出一个可用寄存器。 |

可以把这三样东西想成一座"陷阱桥"：
trampoline 是桥本身、trapframe 是放行李的储物柜、sscratch 是开柜子的钥匙。

## 3. 用户态 → 内核：完整路径

```text
用户程序: read(fd, buf, n)
   ↓ a0/a1/a2 装参数, a7 装 SYS_read
ecall
   ↓ 硬件跳到 stvec → uservec (trampoline 中的汇编)
uservec:
   ① csrrw a0, sscratch, a0       # 交换 a0 和 sscratch，腾出一个可用寄存器
   ② 把 ra,sp,gp,tp,t0..a7,... 全部存到 TRAPFRAME
   ③ 从 trapframe 读出 kernel_sp、kernel_satp、kernel_trap
   ④ 切换 satp 到内核页表 (sfence.vma)
   ⑤ 跳到 kernel_trap (即 usertrap 的地址)
   ↓
usertrap (C 函数, kernel/trap.c)
   ① stvec ← kernelvec   (内核态如果再 trap，从这里进)
   ② 保存 sepc 到 p->trapframe->epc
   ③ 判断 scause:
       == 8  → 系统调用 (走 syscall())
       != 0  → 设备中断  (走 devintr())
       其他  → 异常       (kill 进程或 panic)
   ↓
syscall (kernel/syscall.c)
   num = p->trapframe->a7;
   if(num 合法)
     p->trapframe->a0 = syscalls[num]();   # 真正执行 sys_read
   ↓
usertrapret (准备返回)
   ① stvec ← uservec     (下次用户态 trap 重新走 uservec)
   ② 把 kernel_satp / kernel_sp / kernel_trap / kernel_hartid 填回 trapframe
   ③ sepc ← p->trapframe->epc
   ④ 跳到 userret (trampoline)
   ↓
userret (汇编, trampoline)
   ① satp ← 用户页表 (sfence.vma)
   ② sscratch ← 用户 a0 (从 trapframe 恢复)
   ③ 把 trapframe 里的用户寄存器全部恢复到 CPU
   ④ csrrw a0, sscratch, a0   # 同时恢复 a0 并把 sscratch 重新设为 TRAPFRAME
   ⑤ sret
   ↓
回到用户态 ecall 的下一条指令
```

## 4. 几个高频疑问及答案

**Q1：为什么 trampoline 必须在用户/内核页表中映射到同一个虚拟地址？**

因为 `uservec` 和 `userret` 在执行过程中要切换 `satp`。一旦换页表，当前 `pc` 必须在新页表里仍然有效。同地址映射就是为了切换瞬间不"跌到悬崖外"。

**Q2：为什么要先 `csrrw a0, sscratch, a0`？**

进入 `uservec` 时所有通用寄存器都是用户的，不能随便破坏。内核需要一个临时寄存器。`sscratch` 提前装着 `trapframe` 地址，和 `a0` 交换后：

- `a0` = trapframe 地址 → 可以拿来访问 trapframe；
- `sscratch` = 原用户 `a0` → 等下保存进 trapframe 即可。

这是"既腾出寄存器又不丢用户值"的标准操作。

**Q3：为什么系统调用返回前要 `epc += 4`？**

`ecall` 长度 4 字节，trap 时 `sepc` 指向 `ecall` 本身。如果直接 sret，下一条指令还是 `ecall`，立刻再 trap，死循环。所以 `usertrap` 中：

```c
if(r_scause() == 8){
  p->trapframe->epc += 4;
  ...
  syscall();
}
```

**Q4：为什么 `syscall()` 要把返回值写回 `p->trapframe->a0`？**

因为返回用户态时，`userret` 会把 `trapframe` 中的寄存器恢复回 CPU。RISC-V 调用约定要求返回值在 `a0`，所以把内核结果写到 `trapframe->a0` 就等价于"告诉用户函数：你刚才那次 `read()` 返回了这个值"。

**Q5：系统调用是怎么"按号分发"的？**

`kernel/syscall.c` 里维护了一张函数指针表：

```c
static uint64 (*syscalls[])(void) = {
    [SYS_fork]   sys_fork,
    [SYS_exec]   sys_exec,
    [SYS_read]   sys_read,
    [SYS_write]  sys_write,
    ...
    [SYS_trace]  sys_trace,   // ← 本 lab 添加
};
```

`syscall()` 的实现非常简单：

```c
void syscall(void){
  int num = myproc()->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    myproc()->trapframe->a0 = syscalls[num]();
  } else {
    printf("unknown sys call %d\n", num);
    myproc()->trapframe->a0 = -1;
  }
}
```

`a7` 当下标查表，找到的函数无参，但它在内部会自己用 `argint/argaddr/...` 从 trapframe 里取参数。

## 5. 系统调用怎么读参数？`argint / argaddr / argstr / argraw`

进入内核后，用户传入的参数不在当前 CPU 寄存器里，而是已经被 `uservec` 保存到 `trapframe` 里。内核读参数的统一入口是 `argraw(n)`：

```c
static uint64 argraw(int n){
  struct proc *p = myproc();
  switch (n) {
  case 0: return p->trapframe->a0;
  case 1: return p->trapframe->a1;
  case 2: return p->trapframe->a2;
  ...
  }
}
```

在它之上封装出：

- `argint(n, *ip)`：把第 n 个参数当 int 读；
- `argaddr(n, *ip)`：把第 n 个参数当用户虚拟地址读；
- `argstr(n, buf, max)`：把第 n 个参数当用户字符串地址，调 `copyinstr` 把内容拷到内核 buf。

**为什么不能直接 `*(char*)addr` 读用户指针？** 两个原因：

1. 用户可能传非法/恶意地址，比如指向内核空间；
2. 用户虚拟地址在用户页表中有意义，但内核页表里**不一定映射到同一个物理页**。

所以内核读写用户指针要走 `copyin / copyout / copyinstr`：它们会**用进程的用户页表**显式 `walkaddr` 出物理地址，再用内核的直接映射区访问。本 lab 中 `trace` 没用到这套，但后面的 lazy/COW lab 会大量遇到。

## 6. 用户态陷入 vs 内核态陷入

| 维度 | 用户态陷入 | 内核态陷入 |
| --- | --- | --- |
| `satp` | 用户页表 | 内核页表 |
| `sp` | 用户栈，可能非法 | 内核栈 |
| 入口（`stvec`） | `uservec` | `kernelvec` |
| 处理函数 | `usertrap()` | `kerneltrap()` |
| 是否切页表 | 是 | 否 |
| 是否需要 trampoline | 是 | 否 |
| 寄存器保存到哪里 | 进程的 `trapframe` | 当前内核线程的内核栈 |

所以 xv6 在用户态进入内核时，会立刻把 `stvec` 改成 `kernelvec`；返回用户态前再把它改回 `uservec`。这一段"切换窗口"靠 RISC-V 进入 trap 时硬件自动关中断来保护安全。

---

# 第二部分：trace 系统调用的实现

下面按"用户态 → 系统调用号 → 内核分发 → 进程状态 → fork 继承 → 打印"顺序看每一处修改。

## 任务一：trace 系统调用

### 实现的功能

用户可以运行：

```sh
trace 32 grep hello README
```

`32 == 1 << 5`，而 `SYS_read` 的编号是 5，所以这条命令的含义是：**追踪后续 `grep` 进程及其子进程触发的 `read` 系统调用**。

每次被追踪的系统调用返回后，内核打印：

```text
pid: syscall name -> return_value
```

### 修改 1：分配系统调用号

文件：`kernel/syscall.h`

```c
#define SYS_trace  22
```

这是用户态和内核态识别 `trace` 的共同约定。用户态把它放进 `a7`，内核 `syscall()` 用它查 `syscalls[]`。

### 修改 2：用户态声明 + 用户态 stub

文件：`user/user.h`

```c
int trace(int);   // 在用户态声明函数原型
```

文件：`user/usys.pl`

```perl
entry("trace");
```

`usys.pl` 是一个 Perl 脚本，它会被 Makefile 调用，生成 `user/usys.S`。每个 `entry("name")` 都会生成一段：

```asm
.global trace
trace:
 li a7, SYS_trace
 ecall
 ret
```

这样用户调用 `trace(32)` 时，参数 32 已经被编译器放在 `a0`，stub 再把系统调用号放进 `a7`，然后 `ecall` 进内核。

### 修改 3：在进程 PCB 中保存掩码

文件：`kernel/proc.h`

```c
struct proc {
  ...
  char name[16];
  int trace_mask;   // 新增：用于存储 trace 掩码
};
```

`trace_mask` 是**进程级**状态，所以放在 `struct proc` 里，每个进程独立。

### 修改 4：实现 sys_trace

文件：`kernel/sysproc.c`

```c
// 当用户调用 trace(mask) 时，内核会执行这个函数。
// 它的唯一作用就是把 mask 存到当前进程的 trace_mask 中。
uint64 sys_trace(void){
  int mask;
  if(argint(0, &mask) < 0){   // 从 trapframe->a0 取第 0 个参数
    return -1;
  }
  myproc()->trace_mask = mask;
  return 0;
}
```

注意：这里**没有直接读 a0**，而是通过 `argint(0, ...)`。原因前面解释过——参数已经从用户寄存器搬到了 `trapframe`，内核统一从 trapframe 拿。

### 修改 5：fork 时把 trace_mask 继承给子进程

文件：`kernel/proc.c`，`fork()` 函数中：

```c
// 在 allocproc 成功并复制完页表/文件描述符之后
np->trace_mask = p->trace_mask;
```

为什么要继承？因为用户实际运行的命令是：

```text
trace 32 grep hello README
```

`trace.c` 设置完 mask 后会 `exec` 成 `grep`。`exec` 不换 `struct proc`，所以 mask 保留。但是 `grep` 内部可能再 `fork` 出子进程（比如 shell 启动它的方式），如果不继承，子进程就追踪不到了。

### 修改 6：在分发表注册 sys_trace

文件：`kernel/syscall.c`

```c
extern uint64 sys_trace(void);

static uint64 (*syscalls[])(void) = {
  ...
  [SYS_trace]   sys_trace,
};
```

到这一步为止，`trace(mask)` 已经能调通了。但还差一件事——打印。

### 修改 7：补一张"系统调用名字表"

`syscalls[]` 存的是函数指针，只能从编号找到函数，没法反查"5 号对应的名字"。所以再开一个**同下标**的字符串数组：

```c
static char *syscalls_name[] = {
  [SYS_fork]    "fork",
  [SYS_exit]    "exit",
  [SYS_wait]    "wait",
  [SYS_pipe]    "pipe",
  [SYS_read]    "read",
  [SYS_kill]    "kill",
  [SYS_exec]    "exec",
  [SYS_fstat]   "fstat",
  [SYS_chdir]   "chdir",
  [SYS_dup]     "dup",
  [SYS_getpid]  "getpid",
  [SYS_sbrk]    "sbrk",
  [SYS_sleep]   "sleep",
  [SYS_uptime]  "uptime",
  [SYS_open]    "open",
  [SYS_write]   "write",
  [SYS_mknod]   "mknod",
  [SYS_unlink]  "unlink",
  [SYS_link]    "link",
  [SYS_mkdir]   "mkdir",
  [SYS_close]   "close",
  [SYS_trace]   "trace",
};
```

C99 的 `[SYS_xxx] "..."` 是**指定初始化**语法，它保证编号和名字按下标对齐，即使中间留了空位也不会错。

### 修改 8：在 syscall 分发后打印追踪信息

文件：`kernel/syscall.c`

```c
void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  // 从 trapframe 读出系统调用号
  num = p->trapframe->a7;

  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // 真正执行系统调用并把返回值保存到 a0
    p->trapframe->a0 = syscalls[num]();

    // 追踪打印逻辑：
    // 检查掩码，如果 (1 << 系统调用号) 命中，就打印
    if((1 << num) & p->trace_mask){
      printf("%d: syscall %s -> %d\n",
             p->pid, syscalls_name[num], p->trapframe->a0);
    }
  } else {
    printf("%d %s: unknown sys call %d\n",
           p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
```

这就是 `trace` 的"魔法"所在。系统调用照常执行，返回值照常写进 `trapframe->a0`，然后通过 `1 << num` 取掩码中对应的位，决定是否打印。

### 修改 9：用户态 trace 包装程序

文件：`user/trace.c`

```c
if (trace(atoi(argv[1])) < 0) {
  fprintf(2, "%s: trace failed\n", argv[0]);
  exit(1);
}

// trace.c 存完 mask 后，继续调用 exec 把自己变成 grep。
// 注意：exec 不会换 struct proc，它只替换进程的内存映像，
// 所以 trace_mask = 32 仍然在原进程结构里。
// 这就是为什么 trace 设一次、后面整个 grep 运行都生效。
for(i = 2; i < argc && i < MAXARG; i++){
  nargv[i-2] = argv[i];
}
exec(nargv[0], nargv);
```

### 用户运行时的完整数据流

以 `trace 32 grep hello README` 为例：

```text
shell fork 一个子进程
子进程 exec("trace", ["trace", "32", "grep", "hello", "README"])
   ↓
trace.c 调用 trace(32)
   ↓ usys.pl 生成的 stub: li a7, SYS_trace; ecall
   ↓ uservec → usertrap → syscall
sys_trace(): myproc()->trace_mask = 32
   ↓ 返回用户态
trace.c 调用 exec("grep", ["grep", "hello", "README"])
   ↓ 用户内存被替换为 grep 的 image
   ↓ 但 struct proc 没变，trace_mask 还是 32
grep 运行过程中调用 read()
   ↓ syscall() 执行 sys_read，把返回值写 trapframe->a0
   ↓ 判断 (1 << SYS_read) & 32 = 1，命中
   ↓ printf("pid: syscall read -> ret")
```

### 核心思想

`trace` 的代码量不大，价值在于走通系统调用的完整链路：

- **协议层**：系统调用号在 `kernel/syscall.h` 中分配，用户/内核必须一致；
- **用户态**：`user.h` 声明 + `usys.pl` 生成 stub；
- **进程状态**：mask 是进程级，要放进 `struct proc`；
- **内核派发**：`syscalls[]` 数组 + 配套的 `syscalls_name[]`；
- **进程语义**：`fork` 必须继承，`exec` 不会清掉（exec 不换 struct proc，因此 mask 自然保留）；
- **位掩码协议**：`mask & (1 << num)` 决定是否打印。

理解了这条链路，再加任何一个新系统调用都是机械操作。

---

## 任务二：sysinfo.h 预留结构

### 当前代码

文件：`kernel/sysinfo.h`

```c
struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)
  uint64 nproc;     // number of process
};
```

### 当前完成状态

该结构定义了 `sysinfo` lab 需要返回给用户态的两项信息：

- `freemem`：当前空闲内存字节数；
- `nproc`：当前进程数量。

但当前分支里没有看到完整接入，缺以下东西：

- `kernel/syscall.h` 中没有 `SYS_sysinfo` 编号；
- `kernel/sysproc.c` 中没有 `sys_sysinfo()`；
- `kernel/syscall.c` 的 `syscalls[]` 中没有注册；
- `user/user.h` 没声明；
- `user/usys.pl` 没 entry。

因此 `sysinfo.h` 当前更像是为后续做的结构占位，不是完整功能。

如果要补完，主要思路是：

1. 在 `kalloc.c` 中加 `freemem()`，遍历 `kmem.freelist` 计算空闲页数 × `PGSIZE`；
2. 在 `proc.c` 中加 `nproc()`，遍历 `proc[]` 数组数 `state != UNUSED` 的进程；
3. 实现 `sys_sysinfo()`：`argaddr` 取出用户传入的 `struct sysinfo *` 指针，填入两个字段后 `copyout` 回去；
4. 走一遍上面任务一的"加系统调用八件套"。

---

## 总结

`syscall` 分支完成的主要任务是 `trace`。它的真正价值不在功能本身（功能确实很简单），而在于它逼着你走过一遍 xv6 完整的系统调用机制：

1. 用户态调用 `trace(mask)`；
2. usys stub `ecall` 进内核；
3. trampoline 的 `uservec` 保存现场、切页表；
4. `usertrap()` 判断 `scause==8` → 进入 `syscall()`；
5. `syscall()` 从 `trapframe->a7` 拿系统调用号，分发到 `sys_trace`；
6. `sys_trace` 用 `argint(0, &mask)` 从 `trapframe->a0` 拿参数，写到 `proc->trace_mask`；
7. 返回值写回 `trapframe->a0`；
8. `usertrapret` 准备返回信息，`userret` 切回用户页表、恢复寄存器、`sret` 回用户态；
9. 后续每次该进程的任何系统调用，`syscall()` 都按 mask 选择性打印一行。

最关键的一行代码是：

```c
if((1 << num) & p->trace_mask){
  printf("%d: syscall %s -> %d\n", p->pid, syscalls_name[num], p->trapframe->a0);
}
```

它把"用户给的 32"这一个数字翻译成"对后续若干个系统调用的选择性追踪"，正好是 xv6 系统调用全套机制的一次贯通运用。

