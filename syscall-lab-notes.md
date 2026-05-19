# MIT xv6 Lab syscall 分支实现说明

## 分支概览

`syscall` 分支重点完成了 `trace` 系统调用。它的作用是给进程设置一个系统调用追踪掩码，使进程后续执行指定系统调用时，内核打印追踪信息。

当前分支中还存在 `kernel/sysinfo.h`，定义了 `struct sysinfo`，但没有看到完整的 `sysinfo` 系统调用链路。因此本文将 `sysinfo.h` 单独作为“预留结构”说明。

## 任务一：trace 系统调用

### 实现的功能

用户可以运行：

```sh
trace 32 grep hello README
```

这里 `32` 是掩码。因为 `32 == 1 << 5`，而 xv6 中 `SYS_read` 的系统调用号是 5，所以这个命令表示：追踪后续命令运行过程中触发的 `read` 系统调用。

当被追踪的系统调用返回后，内核输出：

```text
pid: syscall name -> return_value
```

### 核心修改代码片段

#### 1. 增加系统调用号

文件：`kernel/syscall.h`

```c
#define SYS_trace  22
//定义系统调用号
```

这个编号是用户态和内核态识别 `trace` 的共同约定。用户态把它放进 `a7` 寄存器，内核从 `trapframe->a7` 取出并分发。

#### 2. 增加用户态声明和系统调用桩

文件：`user/user.h`

```c
int trace(int);//在用户态声明函数原型
```

文件：`user/usys.pl`

```perl
entry("trace");
```

`usys.pl` 会生成汇编入口，使用户程序能够像调用普通函数一样调用 `trace(mask)`。真正进入内核靠的是生成代码里的 `ecall`。

#### 3. 在进程结构中保存 trace_mask

文件：`kernel/proc.h`

```c
struct proc {
  ...
  char name[16];               // Process name (debugging)
  int trace_mask; //新增：用于存储trace掩码（在进程的PCB中加）
};
```

`trace_mask` 放在 `struct proc` 中，表示追踪设置是进程级状态。这样每个进程都可以有自己的追踪掩码。

#### 4. 实现 sys_trace

文件：`kernel/sysproc.c`

```c
//当用户调用 trace(mask) 时，内核会执行这个函数。
//它的唯一作用就是把 mask 存到当前进程的 trace_mask 中
uint64 sys_trace(void){
  int mask;
  //使用argint获取用户传进来的第0个参数
  if(argint(0,&mask)<0){
    return -1;
  }
  myproc()->trace_mask=mask;
  return 0;
}
```

这个函数负责从用户参数中取出 `mask`，然后保存到当前进程的 `trace_mask` 字段。

#### 5. fork 时继承 trace_mask

文件：`kernel/proc.c`

```c
//在分配和初始化子进程np后
//将trace_mask拷贝到子进程
np->trace_mask=p->trace_mask;
```

这样父进程设置过 trace 后，子进程也会继承同样的追踪设置。

#### 6. 注册系统调用处理函数

文件：`kernel/syscall.c`

```c
extern uint64 sys_trace(void);
```

```c
static uint64 (*syscalls[])(void) = {
  ...
  [SYS_trace]   sys_trace,
};
```

这一步把 `SYS_trace` 和真正的内核函数 `sys_trace` 关联起来。

#### 7. 增加系统调用名表

文件：`kernel/syscall.c`

```c
//syscalls[] 存的是函数指针（sys_read 的地址），没法反查"5 号对应的名字是啥"。
//所以要再平行地开一个字符串数组，下标与 syscalls[] 对齐。
static char *syscalls_name[] = {
  [SYS_fork]    "fork",
  [SYS_exit]    "exit",
  [SYS_wait]    "wait",
  [SYS_pipe]    "pipe",
  [SYS_read]    "read",
  ...
  [SYS_trace]   "trace",
};
```

系统调用表只能从编号找到函数，不能直接知道函数名字。为了打印 `read`、`write` 这种名字，额外维护了一个同下标的字符串数组。

#### 8. 在 syscall 分发后打印追踪信息

文件：`kernel/syscall.c`

```c
void
syscall(void)
{
  int num;
  struct proc *p = myproc();

  //从 trapframe 读出系统调用号
  num = p->trapframe->a7;

  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    //执行系统调用并保存返回值到a0
    p->trapframe->a0 = syscalls[num]();

    //追踪打印逻辑
    //检查掩码：如果（1<<系统调用号）在掩码中，则打印
    if((1<<num) & p->trace_mask){
      printf("%d: syscall %s -> %d\n",
             p->pid,syscalls_name[num],p->trapframe->a0);
    }
  }
  else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}
```

这里是 `trace` 的核心：系统调用先正常执行，返回值写入 `a0`，然后再判断当前系统调用号对应的 bit 是否在 `trace_mask` 中打开。如果打开，就打印追踪信息。

### 用户态 trace 程序

文件：`user/trace.c`

```c
if (trace(atoi(argv[1])) < 0) {
  fprintf(2, "%s: trace failed\n", argv[0]);
  exit(1);
}

//trace.c 存完 mask 后，继续调用 exec 把自己变成 grep。
//注意：exec 不会换进程，它只换进程的内存内容，所以 proc 结构体还是同一个，trace_mask = 32 仍然在。
//这就是为什么 trace 设一次、后面整个 grep 运行都生效。
for(i = 2; i < argc && i < MAXARG; i++){
  nargv[i-2] = argv[i];
}
exec(nargv[0], nargv);
```

`trace` 用户程序先给当前进程设置 `trace_mask`，再通过 `exec` 执行目标程序。`exec` 替换的是用户地址空间，不会换掉 `struct proc`，所以 `trace_mask` 能继续生效。

### 怎么实现的

完整链路是：

```text
user/trace.c 调用 trace(mask)
  -> user/usys.pl 生成 trace syscall stub
  -> ecall 进入内核
  -> syscall.c 从 a7 取 SYS_trace
  -> sysproc.c::sys_trace 保存 mask 到 proc
  -> 后续 syscall 执行后按 mask 判断是否打印
```

### 核心思想

`trace` 的核心不是打印，而是把系统调用机制串起来：

- 用户态需要声明和 syscall stub。
- 内核需要系统调用号和分发表。
- 进程级状态放进 `struct proc`。
- `fork` 要继承 `trace_mask`。
- `exec` 不改变 `struct proc`，所以包装器程序设置的 mask 可以影响后续命令。
- 位掩码用 `1 << num` 表示“是否追踪第 num 号系统调用”。

## 任务二：sysinfo.h 预留结构

### 当前代码片段

文件：`kernel/sysinfo.h`

```c
struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)
  uint64 nproc;     // number of process
};
```

### 当前完成状态

这个结构体定义了 `sysinfo` lab 需要返回给用户态的信息：

- `freemem`：空闲内存字节数。
- `nproc`：当前进程数量。

但当前分支里没有看到完整系统调用接入，例如：

- `SYS_sysinfo` 编号。
- `sys_sysinfo()` 实现。
- `syscalls[]` 注册。
- `user/user.h` 声明。
- `user/usys.pl` 入口。

所以它目前更像是为 `sysinfo` 做的结构准备，还不是完整功能。

## 总结

`syscall` 分支完成的主要任务是 `trace`。它展示了 xv6 新增系统调用的标准路径：分配编号、生成用户态入口、注册内核处理函数、保存进程级状态、在系统调用分发点扩展行为。

其中最核心的片段是 `syscall.c` 中的判断：

```c
if((1<<num) & p->trace_mask){
  printf("%d: syscall %s -> %d\n",p->pid,syscalls_name[num],p->trapframe->a0);
}
```

这一行把 `trace(mask)` 的用户输入转化成了内核对后续系统调用的选择性追踪。

