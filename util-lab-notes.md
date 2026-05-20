# MIT xv6 Lab util 分支实现说明

## 分支概览

`util` 分支主要完成了两个用户态工具：

- 任务一：`sleep`，让当前用户进程休眠指定 tick 数。
- 任务二：`pingpong`，用 pipe 完成父子进程之间的一次双向通信。

这部分没有改内核，重点是熟悉 xv6 用户程序如何调用系统调用，以及如何把新用户程序加入构建系统。

---

## 背景知识：xv6 用户程序是怎么"被加进系统"的？

要理解这一节，先要明白 xv6 中"添加一个用户程序"到底意味着什么。

### 1. xv6 没有动态加载器

xv6 中的用户程序不是动态加载的可执行文件，而是在编译 OS 镜像时一并打包进文件系统镜像 `fs.img` 中。
所以新增一个用户程序，需要：

1. 在 `user/` 目录下创建 `.c` 源文件；
2. 在 `Makefile` 的 `UPROGS` 变量中加上 `$U/_xxx`，让 `mkfs` 把它打包进镜像；
3. 重新 `make qemu`。

### 2. 用户程序通过 `user/user.h` 看到系统调用

`user/user.h` 中声明了用户态可以调用的所有"系统调用函数原型"，比如：

```c
int fork(void);
int exit(int) __attribute__((noreturn));
int pipe(int*);
int read(int, void*, int);
int sleep(int);
int getpid(void);
```

它们看起来是普通函数，但其实是 `user/usys.pl` 生成的汇编 stub。每个 stub 的内容很简单：

```asm
.global sleep
sleep:
 li a7, SYS_sleep
 ecall
 ret
```

也就是：把系统调用号放到 `a7`，执行 `ecall` 触发 trap，等内核处理完后 `ret` 回到用户代码。
所以"调用一个系统调用"等价于"执行一次 ecall trap"。

### 3. xv6 的 pipe 是什么？

pipe 是 UNIX 经典的进程间通信机制：

- `pipe(int p[2])` 在内核里创建一段环形缓冲区，并返回两个文件描述符；
- `p[0]` 是读端，`p[1]` 是写端；
- 写端写入的字节会按顺序从读端读出，是字节流模型；
- pipe 是**单向**的，要双向通信就得开两条 pipe。

pipe 的另一个关键特性：**fork 后父子进程共享同一个 pipe 的两端**。
所以 `pipe + fork` 是 xv6 中最自然的"进程间通信"组合。

### 4. fork 复制了什么？

`fork()` 在 xv6 中会：

- 复制父进程的整个用户内存（lazy 实验之前是直接 memcpy）；
- 复制父进程的所有文件描述符；
- 父进程返回子进程的 pid，子进程返回 0。

所以 `fork` 之后，父子进程**各自都有 p1[0]/p1[1]/p2[0]/p2[1] 共四个 fd**。
也就是说，pipe 的读端和写端在两个进程里都存在。

这就引出了 pingpong 中"关闭不需要的端"的原因：如果不关，pipe 的读端因为还有引用就不会返回 EOF，会让 `read()` 一直阻塞。

---

## 任务一：sleep

### 实现的功能

`sleep` 接收一个命令行参数，把它转换成整数 tick 数，然后调用 xv6 已有的 `sleep` 系统调用。

运行形式：

```sh
sleep 10
```

表示当前进程休眠 10 个 tick。

### 核心修改代码片段

文件：`user/sleep.c`

```c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char* argv[]){
  // 步骤1：检查参数个数，如果没有提供参数就提醒用户
  if(argc < 2){
    fprintf(2, "Usage:sleep <ticks>\n"); // 2 是 stderr 文件描述符
    exit(0);
  }

  // 步骤2：将字符串参数转换为整数
  // argv[0] 是程序名 "sleep"，argv[1] 才是用户输入的数字
  int ticks = atoi(argv[1]);

  // 步骤3：发起 sleep 系统调用
  sleep(ticks);

  // 步骤4：退出程序
  exit(0);
}
```

文件：`Makefile`

```makefile
UPROGS=\
  ...
  $U/_sleep\
  ...
```

### 怎么实现的

`argv[0]` 是程序名，真正的参数从 `argv[1]` 开始，所以先检查 `argc < 2`。参数存在时，用 `atoi` 转成整数，再调用 `sleep(ticks)`。

`sleep` 是 xv6 内核已经提供的系统调用，定义在 `kernel/sysproc.c::sys_sleep`：它会把当前进程挂到 `&ticks` 这个等待队列上，定时器中断每次进入内核都会 `wakeup(&ticks)`，从而让休眠的进程返回。

用户程序不需要自己实现计时逻辑，只是把命令行参数转成数字然后调用现成接口。

### 核心思想

这个任务的核心是理解"用户态程序通过 `user/user.h` 中声明的函数进入系统调用"。`sleep.c` 本身很短，但它展示了 xv6 用户程序的标准骨架：

1. 包含三大头文件（`types.h`、`stat.h`、`user.h`）；
2. 检查 `argc`；
3. 用 `atoi`/`fprintf` 等用户库函数处理；
4. 调用系统调用；
5. `exit()` 而不是 `return`（xv6 的用户程序入口不会自动调用 exit）。

---

## 任务二：pingpong

### 实现的功能

`pingpong` 创建一个子进程，父进程先向子进程发送一个字节，子进程收到后打印 `received ping`，再把字节发回父进程，父进程收到后打印 `received pong`。

预期输出：

```text
<子进程 pid>: received ping
<父进程 pid>: received pong
```

### 核心修改代码片段

文件：`user/pingpong.c`

```c
#include "kernel/types.h"   // 定义基本类型，如 int、uint 等
#include "kernel/stat.h"    // 文件状态相关
#include "user/user.h"      // 用户可用的系统调用声明

int
main(){
  // 定义两个数组来表示两个管道的两端
  int p1[2], p2[2];   // p1: 父→子，p2: 子→父

  pipe(p1);
  pipe(p2);

  // 创建子进程
  int pid = fork();

  if(pid == 0){
    // 子进程
    close(p1[1]);   // 子进程不需要写 p1
    close(p2[0]);   // 子进程不需要读 p2

    char buf[1];
    read(p1[0], buf, 1);                       // 阻塞读父进程发来的字节
    printf("%d:received ping\n", getpid());
    write(p2[1], buf, 1);                      // 把字节回传给父进程

    close(p1[0]);
    close(p2[1]);
    exit(0);
  } else {
    // 父进程
    close(p1[0]);
    close(p2[1]);

    char buf[1] = {'x'};
    write(p1[1], buf, 1);                      // 先发一个字节给子进程
    read(p2[0], buf, 1);                       // 等待子进程的回信
    printf("%d:received pong\n", getpid());

    close(p1[1]);
    close(p2[0]);
    exit(0);
  }
}
```

文件：`Makefile`

```makefile
UPROGS=\
  ...
  $U/_pingpong\
  ...
```

### 怎么实现的

因为 xv6 的 pipe 是单向通信通道，所以这里用了两个 pipe：

- `p1`：父进程写，子进程读。
- `p2`：子进程写，父进程读。

`fork()` 后父子进程会同时拥有两个 pipe 的读写端。父子两边都关闭自己不用的端口，原因有二：

1. 防止 fd 一直被占用造成泄漏；
2. 更关键：pipe 只有当**所有写端都被关闭**时，读端的 `read()` 才会返回 0（EOF）。如果不关闭不需要的端，对方关闭后 read 仍然会阻塞，造成挂死。

### 时序分析

```text
父进程                          子进程
  |  pipe(p1), pipe(p2)
  |  fork()  ──────────────────►  fork 返回 0
  |                                close(p1[1]), close(p2[0])
  |  close(p1[0]), close(p2[1])    read(p1[0])  ← 阻塞
  |  write(p1[1], "x")  ─────────► 读到 "x"
  |  read(p2[0])  ← 阻塞           printf("received ping")
  |                                write(p2[1], "x")
  |  读到 "x"   ◄──────────────── exit
  |  printf("received pong")
  |  exit
```

### 核心思想

`pingpong` 把 xv6 中"进程"和"IPC"两个最基本概念串了起来：

- `fork()`：复制进程；
- `pipe()`：在内核里建一段可读可写的字节流；
- `read/write`：同步阻塞地交换数据；
- `close()`：除了释放 fd，还充当 EOF 信号。

理解了 pingpong，再去看 shell 中的 `cat | grep` 就会发现，本质上就是把若干个进程的标准输入/输出用 pipe 连起来，加 `fork + dup2` 而已。

---

## 总结

`util` 分支的两个任务都很小，但正好覆盖了 xv6 用户态编程最重要的入门能力：

- `sleep` 练习命令行参数和系统调用调用方式；
- `pingpong` 练习 `fork`、`pipe`、文件描述符和进程同步。

这两个程序的代码量都不大，关键是要理解背后的机制：用户程序经过 `usys.pl` 生成的 stub 通过 `ecall` 陷入内核，内核 `syscall()` 分发到对应的 `sys_xxx`，处理完再返回用户态。这条路径在后续 `syscall` lab 里会被显著地展开。

