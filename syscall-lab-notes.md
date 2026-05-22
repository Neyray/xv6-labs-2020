# MIT xv6 Lab syscall 分支实现说明

## 分支概览

`syscall` 分支的核心任务是给 xv6 加一条新的系统调用 `trace`。它的作用是给进程设置一个系统调用追踪掩码，使进程后续执行匹配的系统调用时，内核自动打印追踪信息。

完成 `trace` 等于走完了"添加一个新系统调用"的完整链路：用户态声明 → 用户态 stub → 系统调用号 → 内核分发表 → 内核处理函数 → 进程级状态保存 → `fork` 中继承 → 系统调用返回时打印。

分支里还有 `kernel/sysinfo.h` 中的 `struct sysinfo`，但 `sysinfo` 系统调用链路并未补全，所以本文最后单独说明。

---

# 第一部分：系统调用的完整内核流程

理解这个 lab 的前提，是理解"用户程序怎么通过 `ecall` 进入内核，内核怎么处理，又怎么把结果送回用户态"。一句话主线就是：

> 用户态出事了 → 先跳到 trampoline → 切换到内核页表 → 进入内核 C 函数处理 → 准备返回 → 再跳回 trampoline → 切回用户页表 → 恢复用户寄存器 → `sret` 回用户态。

下面从 RISC-V 硬件出发，把整条路径一步一步走一遍。

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
3. 关闭中断（这一点非常重要，下面 §8 会再谈）；
4. 把 `pc` 跳到 `stvec`，开始执行 trap 处理代码。

注意 `ecall` 本身**不会切页表，也不会自动保存所有寄存器**。这两件事都得由内核自己安排。

## 2. 为什么用户态陷阱特别麻烦？

用户程序执行时，CPU 处于用户态，并且：

1. `satp` 指向的是**用户页表**；
2. 用户页表通常**不映射内核代码、内核栈**；
3. 用户的 `sp` 可能指向坏地址，甚至是恶意伪造的；
4. RISC-V 发生 trap 时**硬件不会自动切换页表**；
5. RISC-V **也不会自动保存所有寄存器**。

所以内核不能直接"跳进 C 函数 `usertrap` 处理"——当前页表是用户页表，根本看不到 `usertrap` 的代码和它要用的内核栈。

因此 xv6 需要一个"过渡区"，把"还在用户页表下执行"和"已经在内核页表下执行"两段时间安全地拼起来。这个过渡区就是 **trampoline 页**。

## 3. 三件关键设施：trampoline、trapframe、sscratch

| 名字 | 作用 |
| --- | --- |
| `trampoline` 页 | 一段汇编（`uservec` + `userret`），**同时映射到用户页表和内核页表**，并且**两边虚拟地址相同**（`TRAMPOLINE`）。这样在 `satp` 切换的瞬间，当前 PC 在新旧页表中都有效，CPU 不会"跌出页表"。 |
| `trapframe` 页 | 每进程一份，专门用来保存用户态寄存器现场，并预先填好内核入口所需信息（`kernel_satp` / `kernel_sp` / `kernel_trap` / `kernel_hartid` / `epc`）。它在用户页表里映射到固定虚拟地址 `TRAPFRAME`，这样 `uservec` 在切页表之前就能访问它。 |
| `sscratch` 寄存器 | RISC-V 提供的一个"工具寄存器"。进入用户态前，xv6 把它设为当前进程 `trapframe` 的虚拟地址。`uservec` 一进入内核就靠它"凭空"弄出一个可用通用寄存器。 |

可以把这三样东西想成一座"陷阱桥"：trampoline 是桥本身、trapframe 是放行李的储物柜、sscratch 是开柜子的钥匙。

**为什么 trampoline 必须在两个页表里映射到同一个虚拟地址？** 因为 `uservec` / `userret` 中途会写 `satp` 切换页表，切完之后 CPU 还要继续执行下一条指令。如果新页表里没有当前 PC 对应的映射，CPU 立刻就崩了。同地址映射就是为了切换瞬间不"跌到悬崖外"。

**为什么 trapframe 也要映射到用户页表？** 因为 `uservec` 刚开始执行时 `satp` 还是用户页表，但它马上就要把寄存器保存到 `trapframe`，所以这块虚拟地址在用户页表里必须能访问到。

## 4. 用户态 → 内核：完整路径

```text
用户程序: read(fd, buf, n)
   ↓ a0/a1/a2 装参数, a7 装 SYS_read
ecall
   ↓ 硬件: sepc←pc, scause←8, 关中断, pc←stvec
   ↓ 跳到 stvec → uservec (trampoline 中的汇编)
uservec:
   ① csrrw a0, sscratch, a0       # 交换 a0 和 sscratch，腾出一个可用寄存器
                                  # 交换后 a0 = TRAPFRAME 地址, sscratch = 用户原来的 a0
   ② 把 ra,sp,gp,tp,t0..t6,s0..s11,a1..a7 全部存到 TRAPFRAME
       并把 sscratch (= 用户原 a0) 也存到 trapframe->a0
   ③ 从 trapframe 读出 kernel_sp、kernel_satp、kernel_trap、kernel_hartid
   ④ 切换 satp 到内核页表，并 sfence.vma 刷 TLB
   ⑤ 跳到 kernel_trap (即 usertrap 的地址)
   ↓
usertrap (C 函数, kernel/trap.c)
   ① stvec ← kernelvec     # 内核态如果再发生 trap，要走 kernelvec
   ② 保存 sepc 到 p->trapframe->epc
                           # sepc 是 CPU 控制寄存器，后面可能 yield/调度，
                           # 万一被覆盖就找不回返回地址了，必须先存进 trapframe
   ③ 判断 scause:
       == 8  → 系统调用：epc += 4; syscall()
       是设备中断 → devintr()，如是定时器中断可能 yield()
       其他  → 异常：kill 当前进程 (内核异常则 panic)
   ↓
syscall (kernel/syscall.c)
   num = p->trapframe->a7;
   if(num 合法)
     p->trapframe->a0 = syscalls[num]();   # 真正执行 sys_read
   ↓
usertrapret (准备返回, kernel/trap.c)
   ① 关中断（即将改一组与返回有关的寄存器，不能被打断）
   ② stvec ← uservec        # 下次用户态 trap 重新走 uservec
   ③ 把 kernel_satp / kernel_sp / kernel_trap / kernel_hartid 重新填进 trapframe
                            # 因为下次用户 trap 时 uservec 还要靠这些字段
   ④ sepc ← p->trapframe->epc
   ⑤ 跳到 userret (trampoline)，传 a0 = 用户页表, a1 = TRAPFRAME
   ↓
userret (汇编, trampoline)
   ① satp ← 用户页表 (sfence.vma)
                            # 现在已经在用户页表下运行，但因为 trampoline 是同地址
                            # 映射，所以 PC 仍然有效
   ② 把 trapframe 中保存的"用户原 a0"放进 sscratch（为后面 csrrw 做准备）
   ③ 把 trapframe 里的用户寄存器全部恢复到 CPU（除 a0 之外）
   ④ csrrw a0, sscratch, a0  # 同时完成两件事：
                            #   - a0 ← 用户原来的 a0
                            #   - sscratch ← TRAPFRAME（为下一次 trap 进入 uservec 准备好）
   ⑤ sret                   # 回到 sepc 指定的用户指令
   ↓
回到用户态 ecall 的下一条指令
```

## 5. 把四个函数的分工记清楚

四个函数的分工非常清楚，可以这样记忆：

```text
uservec     负责"安全进内核"   ：保存用户现场 + 切换到内核页表 + 跳到 usertrap
usertrap    负责"真正处理事情" ：判断 trap 原因、做系统调用 / 处理设备中断 / 杀进程
usertrapret 负责"准备回用户态" ：把 trapframe / sepc / stvec 都设回去
userret     负责"安全回用户态" ：切回用户页表 + 恢复寄存器 + sret
```

## 6. 几个高频疑问及答案

**Q1：为什么进 `uservec` 第一句一定是 `csrrw a0, sscratch, a0`？**

进入 `uservec` 时所有通用寄存器都是用户的，不能随便破坏。但内核又必须用一个寄存器才能"动手"。`sscratch` 在进用户态之前已经被 xv6 写成了 `TRAPFRAME` 地址，所以这条指令一次性做了两件事：

- `a0` ← `TRAPFRAME`（拿到一个能干活的寄存器，并且它正好指向要写的位置）；
- `sscratch` ← 用户原来的 `a0`（用户值没丢，先寄存一下，等会儿存进 trapframe）。

这是"既腾出寄存器又不丢用户值"的标准操作。

**Q2：为什么返回时还要再来一次 `csrrw a0, sscratch, a0`？**

返回前 `userret` 已经把"用户原 `a0`"放到了 `sscratch`，把 `a0` 设成了 `TRAPFRAME`。最后再交换一次，就同时完成：

- 恢复用户的 `a0`；
- `sscratch` 重新变成 `TRAPFRAME`，为**下一次** trap 进入 `uservec` 准备好工具。

所以 `sscratch` 一直在两个角色间循环：用户态期间它装着 `TRAPFRAME`，内核态期间它短暂装着"用户原 `a0`"。

**Q3：为什么系统调用返回前要 `epc += 4`？**

`ecall` 长度 4 字节，trap 时硬件保存的 `sepc` 指向 `ecall` 本身。如果直接 `sret`，下一条指令还是 `ecall`，立刻再次陷入内核，死循环。所以 `usertrap` 在判定是系统调用时：

```c
if(r_scause() == 8){
  p->trapframe->epc += 4;   // 跳过 ecall，回到它后面那条指令
  intr_on();
  syscall();
}
```

设备中断和异常则**不能**加 4，因为被打断的那条指令并没执行完。

**Q4：为什么 `usertrap` 一开始就要保存 `sepc` 到 `trapframe->epc`？**

因为 `sepc` 是 CPU 控制寄存器，是全局的。后续 `syscall` 内可能因为 sleep / I/O / 定时器中断触发 `yield`，把 CPU 让给别的进程；别的进程返回时会改写自己的 `sepc`。如果不先存进 trapframe，等切回本进程时返回地址就丢了。

**Q5：为什么 `syscall()` 要把返回值写回 `p->trapframe->a0`？**

因为返回用户态时 `userret` 会把 `trapframe` 中的寄存器恢复回 CPU。RISC-V 调用约定要求返回值在 `a0`，所以"写到 `trapframe->a0`"就等价于"告诉用户：你刚才那次 `read()` 返回了这个值"。

**Q6：系统调用是怎么"按号分发"的？**

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

`a7` 当下标查表，找到的 `sys_*` 函数声明里是无参的，但它在内部会自己用 `argint/argaddr/...` 从 trapframe 里取参数。

## 7. 系统调用怎么读参数？`argint / argaddr / argstr / argraw`

进入内核后，用户传入的参数**已经不在当前 CPU 寄存器里了**（`uservec` 已经把寄存器全部覆盖掉去做内核的事了），它们躺在 `trapframe` 中。内核读参数的统一入口是 `argraw(n)`：

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
- `argaddr(n, *ip)`：把第 n 个参数当用户虚拟地址读（注意：只是把"地址值"取出来，**没有去读这个地址指向的数据**）；
- `argstr(n, buf, max)`：把第 n 个参数当用户字符串地址，调 `copyinstr` 把内容拷到内核 buf；
- `argfd(n, *fd, **file)`：把第 n 个参数当文件描述符，并查到对应的 `struct file`。

**为什么不能直接 `*(char*)addr` 读用户指针？** 两个原因：

1. **安全**：用户可能传非法/恶意地址（比如随便一个数，或故意指向内核空间企图让内核帮它读内核数据）。内核必须先确认这个地址是合法的用户地址。
2. **页表不同**：用户虚拟地址在**用户页表**中有意义，但内核运行时 `satp` 是**内核页表**，同一个虚拟地址在内核页表里不一定映射到同一个物理页，甚至根本无效。

所以内核读写用户指针要走 `copyin / copyout / copyinstr`：它们会**用进程的用户页表**显式 `walkaddr` 出物理地址，确认合法后，再用内核的直接映射区访问。

记忆口诀：

```text
copyin     ：用户空间 → 内核（拷普通数据）
copyout    ：内核 → 用户空间
copyinstr  ：用户空间 → 内核（拷字符串，直到遇到 '\0'）
```

本 lab 中 `trace` 只传一个 int，没用到这套；但后面的 lazy / COW lab 会大量遇到。

## 8. 用户态陷入 vs 内核态陷入

如果 CPU 已经在内核里运行，又发生中断或异常，走的就不是 `uservec / usertrap`，而是 `kernelvec / kerneltrap`：

| 维度 | 用户态陷入 | 内核态陷入 |
| --- | --- | --- |
| `satp` | 用户页表 | 内核页表 |
| `sp` | 用户栈，可能非法 | 内核栈 |
| 入口（`stvec`） | `uservec` | `kernelvec` |
| 处理函数 | `usertrap()` | `kerneltrap()` |
| 是否切页表 | 是 | 否（已经在内核页表了） |
| 是否需要 trampoline | 是 | 否 |
| 寄存器保存到哪里 | 进程的 `trapframe` | 当前内核线程的**内核栈** |

`kernelvec` 把所有寄存器压到当前内核线程的内核栈上（因为这些寄存器属于"当前正被打断的内核线程"，将来即使切换线程，原线程的栈仍然安全地保存着它的现场）。

`kerneltrap` 做的事：

- 是设备中断 → `devintr()`；如果是定时器中断且当前在某个进程的内核线程中，可能 `yield()` 让出 CPU；
- 是内核异常 → `panic`（内核自己写出错代码不能继续跑）。

注意 `kerneltrap` 一开始要把 `sepc` 和 `sstatus` **先备份到 C 局部变量**，结束前再恢复回去。原因和 `usertrap` 保存 epc 一样：中间可能 `yield`，调度过程会动这些控制寄存器。

### `stvec` 为什么要在 user/kernel 间反复切换？

xv6 通过修改 `stvec` 来切换"trap 入口"：

```text
用户态运行时：stvec = uservec     （触发 trap → uservec）
内核态运行时：stvec = kernelvec   （触发 trap → kernelvec）
```

切换时机：

- `usertrap` 一开始就把 `stvec` 改成 `kernelvec`（已经在内核了，再 trap 走 kernelvec）；
- `usertrapret` 在返回用户态前把 `stvec` 改回 `uservec`（用户态再 trap 重新走 uservec）。

### "窗口期"为什么不会出事？

`usertrap` 设置 `stvec = kernelvec` 之前有一段短暂窗口：CPU 已经在内核了，但 `stvec` 仍指着 `uservec`。如果此时来个设备中断，硬件就会错误地跳到 `uservec`——但 `uservec` 是按"刚从用户态进来"假设来写的，会立刻搞坏一切。

xv6 不需要专门防这件事，因为 RISC-V 进 trap 时硬件会**自动关中断**（`sstatus.SIE` 被清零）。`usertrap` 一直要等 `stvec` 改完、`trapframe` 处理好之后才会重新开中断（系统调用路径下 `intr_on()`）。同理，`usertrapret` 在返回前会再次关中断，避免 `stvec` 改回 `uservec` 之前被设备中断打中。

## 9. 整条链路的总览图

把上面所有东西合成一张图：

```text
用户程序运行
   ↓ ecall / 异常 / 中断
跳到 stvec 指向的 uservec  (trampoline)
   ↓
uservec：
   保存用户寄存器到 trapframe
   切换 satp 到内核页表
   跳到 usertrap
   ↓
usertrap：
   stvec ← kernelvec
   保存 sepc 到 trapframe->epc
   判断 scause：syscall / devintr / 异常
   ↓
syscall：
   num = trapframe->a7
   trapframe->a0 = syscalls[num]()
   ↓
usertrapret：
   关中断
   stvec ← uservec
   重新填好 trapframe 中的内核字段
   sepc ← trapframe->epc
   跳到 userret
   ↓
userret  (trampoline)：
   切回用户页表
   恢复用户寄存器
   csrrw a0, sscratch, a0
   sret
   ↓
回到用户态继续执行
```

内核态被中断则是另一条更简单的线：

```text
内核代码运行
   ↓ 设备中断 / 异常
kernelvec：把寄存器压到当前内核栈
   ↓
kerneltrap：
   备份 sepc / sstatus
   devintr / panic
   定时器中断 → 可能 yield
   恢复 sepc / sstatus
   ↓
kernelvec 恢复寄存器
   ↓
sret  → 回到被中断的内核指令
```

把这两张图记住，"加一条系统调用"的工作就只剩下"在合适的位置往这条链上接东西"——这正是第二部分要做的事。

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

