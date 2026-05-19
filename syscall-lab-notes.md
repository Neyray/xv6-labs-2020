# MIT xv6 Lab: syscall 分支实现说明

## 分支范围

本文档对应仓库 `Neyray/xv6-labs-2020` 的 `syscall` 分支，重点根据当前分支中的 `trace` 实现和相关注释编写。

从代码现状看，这个分支完整接入的是 `trace` 系统调用；同时存在 `kernel/sysinfo.h`，定义了 `struct sysinfo`，但暂未看到 `sys_sysinfo`、系统调用表、用户态桩等完整链路接入。因此本文会把 `sysinfo.h` 作为预留结构说明，而不把 `sysinfo` 写成已经完整完成的系统调用。

## 实现的功能

### trace 系统调用

`trace(mask)` 的功能是：给当前进程设置一个系统调用追踪掩码。之后当前进程执行系统调用时，如果该系统调用号对应的 bit 被打开，内核就在系统调用返回后打印一行追踪信息。

输出格式为：

```text
pid: syscall name -> return_value
```

例如用户运行类似：

```text
trace 32 grep hello README
```

其中 `32` 是二进制掩码，表示打开第 5 位，对应 xv6 中的 `SYS_read`。因此它追踪的不是 `grep` 程序本身这个名字，而是 `grep` 运行过程中触发的 `read` 系统调用。

## 怎么实现的

### 1. 为系统调用分配编号

在 `kernel/syscall.h` 中新增：

```c
#define SYS_trace 22
```

这个编号会被用户态 syscall stub 放进 RISC-V 的 `a7` 寄存器。进入内核后，`syscall()` 从 `p->trapframe->a7` 取出系统调用号，并据此找到真正的内核处理函数。

### 2. 生成用户态系统调用入口

在 `user/usys.pl` 中加入：

```perl
entry("trace");
```

这样构建时会生成 `trace` 的汇编桩：把 `SYS_trace` 放入 `a7`，执行 `ecall` 进入内核，然后返回用户态。

同时在 `user/user.h` 中声明：

```c
int trace(int);
```

这让用户程序可以像普通函数一样调用 `trace(mask)`。

### 3. 在进程结构中保存追踪状态

在 `kernel/proc.h` 的 `struct proc` 中新增：

```c
int trace_mask;
```

这个字段属于进程控制块，用来记录当前进程想追踪哪些系统调用。把追踪状态放在 `proc` 中的好处是，它天然跟随进程存在，不需要额外的全局表。

### 4. 实现 sys_trace

在 `kernel/sysproc.c` 中新增 `sys_trace()`：

- 使用 `argint(0, &mask)` 读取用户传进来的第 0 个参数。
- 参数读取失败时返回 `-1`。
- 成功时将 `mask` 保存到 `myproc()->trace_mask`。
- 返回 `0` 表示设置成功。

这一步完成了“用户态 trace(mask) 参数进入内核并保存到当前进程”的核心路径。

### 5. fork 时继承 trace_mask

在 `kernel/proc.c` 的 `fork()` 中，分配并初始化子进程后增加：

```c
np->trace_mask = p->trace_mask;
```

这样通过 `fork` 创建出来的子进程会继承父进程的追踪设置。这个设计符合 lab 要求，也解释了为什么 `trace mask command` 这种包装器能够让后续执行的命令被追踪。

### 6. 在 syscall 分发处打印追踪结果

在 `kernel/syscall.c` 中做了两件事：

- 给 `SYS_trace` 注册处理函数 `sys_trace`。
- 新增 `syscalls_name[]`，用系统调用号反查系统调用名字。

`syscall()` 原本只负责根据系统调用号调用 `syscalls[num]()`，并把返回值写回 `p->trapframe->a0`。现在在系统调用执行之后，增加掩码判断：

```c
if ((1 << num) & p->trace_mask) {
  printf("%d: syscall %s -> %d\n", p->pid, syscalls_name[num], p->trapframe->a0);
}
```

这里的核心是位图思想：`mask` 的第 `num` 位为 1，就表示追踪编号为 `num` 的系统调用。

### 7. 用户态 trace 程序

`user/trace.c` 负责把 shell 命令包装成可追踪执行：

- 检查参数数量和 mask 格式。
- 调用 `trace(atoi(argv[1]))` 设置当前进程的追踪掩码。
- 把后续参数整理成新的 `argv`。
- 调用 `exec(nargv[0], nargv)` 执行目标命令。

代码注释中强调了一个关键点：`exec` 不会换掉 `proc` 结构，它只是替换当前进程的用户态地址空间。因此 `trace_mask` 仍然保留在同一个进程结构中，目标命令运行期间仍会被追踪。

## 核心思想

`syscall` 分支的核心是“打通一次完整的系统调用链路”：

```text
用户程序 trace(mask)
  -> user/usys.pl 生成的 syscall stub
  -> ecall
  -> kernel/syscall.c 根据 a7 分发
  -> kernel/sysproc.c::sys_trace 保存 mask
  -> 后续 syscall 返回时按 mask 打印
```

这个实现把几个 xv6 关键概念串起来了：

- `a7` 保存系统调用号。
- `a0` 既可作为参数寄存器，也保存系统调用返回值。
- `struct proc` 是保存进程级状态的合适位置。
- `fork` 复制进程状态，`exec` 替换用户内存但保留进程结构。
- 用位掩码可以用一个整数高效表达“追踪哪些系统调用”。

## sysinfo.h 的代码现状

当前分支有 `kernel/sysinfo.h`：

```c
struct sysinfo {
  uint64 freemem;
  uint64 nproc;
};
```

它定义了 `sysinfo` lab 所需的数据结构：空闲内存字节数和进程数量。不过当前分支没有完整看到以下配套内容：

- `SYS_sysinfo` 系统调用编号。
- `sys_sysinfo()` 内核实现。
- `syscalls[]` 中的注册项。
- `user/user.h` 中的用户态声明。
- `user/usys.pl` 中的 `entry("sysinfo")`。

所以它更像是 `sysinfo` 功能的准备工作，而完整实现仍需要继续接上内核统计和 `copyout` 返回用户态结构体。

## 关键文件

- `kernel/syscall.h`：新增 `SYS_trace` 系统调用号。
- `user/usys.pl`：生成 `trace` 用户态系统调用桩。
- `user/user.h`：声明 `int trace(int)`。
- `kernel/proc.h`：在 `struct proc` 中新增 `trace_mask`。
- `kernel/proc.c`：在 `fork()` 中继承追踪掩码。
- `kernel/sysproc.c`：实现 `sys_trace()`。
- `kernel/syscall.c`：注册系统调用、维护系统调用名数组、执行追踪打印。
- `user/trace.c`：用户态命令包装程序。
- `kernel/sysinfo.h`：定义 `struct sysinfo`，当前属于预留/部分实现。

## 小结

这个分支最有价值的部分是 `trace` 的完整系统调用闭环。它不仅添加了一个新 syscall，还展示了 xv6 中用户态、trapframe、系统调用表、进程结构和 fork/exec 语义之间的关系。核心并不是打印本身，而是理解“系统调用如何从用户态进入内核，又如何借助进程状态影响后续行为”。

