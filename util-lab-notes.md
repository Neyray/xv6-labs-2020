# MIT xv6 Lab: util 分支实现说明

## 分支范围

本文档对应仓库 `Neyray/xv6-labs-2020` 的 `util` 分支，重点根据当前分支中的 `user/sleep.c`、`user/pingpong.c` 和 `Makefile` 编写。

这个分支完成了 util lab 中两个基础用户态程序：

- `sleep`：让用户进程休眠指定 tick 数。
- `pingpong`：用 pipe 在父子进程之间完成一次双向通信。

## 实现的功能

### sleep

`sleep` 程序接收一个命令行参数，将它解释为 tick 数，并调用 xv6 已有的 `sleep` 系统调用让当前进程进入休眠。

实现行为如下：

- 如果用户没有传入 tick 参数，向标准错误输出 `Usage:sleep <ticks>`。
- 使用 `atoi(argv[1])` 将字符串参数转换为整数。
- 调用 `sleep(ticks)` 进入内核提供的睡眠逻辑。
- 最后调用 `exit(0)` 结束用户程序。

对应文件是 `user/sleep.c`。代码里的注释把实现拆成了“检查参数、转换参数、调用系统调用、退出程序”四个步骤，结构比较清晰。

### pingpong

`pingpong` 程序创建一个子进程，并通过两个管道完成父子进程之间的一次消息往返：

- `p1` 表示父进程到子进程的管道。
- `p2` 表示子进程到父进程的管道。
- 父进程写入一个字节，子进程读到后打印 `received ping`。
- 子进程再把这个字节写回父进程，父进程读到后打印 `received pong`。

对应文件是 `user/pingpong.c`。程序使用 `pipe`、`fork`、`read`、`write`、`close`、`getpid`、`printf` 和 `exit` 这些用户态系统调用接口完成通信流程。

## 怎么实现的

### 1. 新增用户程序源文件

分支中新增了两个用户程序：

- `user/sleep.c`
- `user/pingpong.c`

这两个文件都包含 xv6 用户程序常见的头文件：

- `kernel/types.h`
- `kernel/stat.h`
- `user/user.h`

其中 `user/user.h` 提供用户态可调用的系统调用声明。

### 2. 修改 Makefile

在 `Makefile` 的 `UPROGS` 列表中加入：

- `$U/_sleep`
- `$U/_pingpong`

这样 xv6 构建文件系统镜像时，会把这两个用户程序编译并打包进 `fs.img`，进入 xv6 shell 后就能直接运行 `sleep` 和 `pingpong`。

### 3. 使用已有系统调用完成用户态任务

这个 lab 的重点不在修改内核，而是在用户态熟悉 xv6 的系统调用使用方式：

- `sleep` 直接调用已有的 `sleep` 系统调用。
- `pingpong` 用 `pipe` 建立字节流通道，用 `fork` 创建父子进程，再用 `read/write` 做同步通信。

## 核心思想

`util` 分支的核心是“用 xv6 提供的最小系统调用组合出有行为的用户程序”。

`sleep` 展示了用户程序如何接收 shell 参数、做简单参数转换，并调用系统调用进入内核。它的关键点是区分 `argv[0]` 是程序名，`argv[1]` 才是用户输入的 tick 数。

`pingpong` 展示了进程和管道的基本模型。由于 xv6 的 pipe 是单向字节流，所以父子双向通信需要两个 pipe。程序中及时关闭不使用的读端或写端，可以避免资源泄漏，也让父子进程之间的通信方向更明确。

## 关键文件

- `user/sleep.c`：实现 `sleep ticks` 用户程序。
- `user/pingpong.c`：实现父子进程 pipe 双向通信。
- `Makefile`：将 `_sleep` 和 `_pingpong` 加入用户程序构建列表。

## 小结

这个分支主要完成了 xv6 用户态编程的入门部分。实现规模不大，但覆盖了 xv6 用户程序最基本的能力：命令行参数、系统调用、进程创建、管道通信和进程退出。它为后续 syscall lab 中“从用户态穿过系统调用入口进入内核”打下了基础。

