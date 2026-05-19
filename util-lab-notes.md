# MIT xv6 Lab util 分支实现说明

## 分支概览

`util` 分支主要完成了两个用户态工具：

- 任务一：`sleep`，让当前用户进程休眠指定 tick 数。
- 任务二：`pingpong`，用 pipe 完成父子进程之间的一次双向通信。

这部分没有改内核，重点是熟悉 xv6 用户程序如何调用系统调用，以及如何把新用户程序加入构建系统。

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
int
main(int argc,char* argv[]){
  //步骤1：检查参数个数，如果没有提供参数就提醒用户
  if(argc<2){
    fprintf(2,"Usage:sleep <ticks>\n");//2是报错文件标志符
    exit(0);
  }

  //步骤2：将字符串参数转换为整数
  //argv[0]是程序名"sleep"，argv[1]才是用户输入的数字
  int ticks=atoi(argv[1]);

  //步骤3：添加sleep系统调用
  sleep(ticks);

  //步骤4：退出程序
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

`argv[0]` 是程序名，真正的参数从 `argv[1]` 开始，所以程序先检查 `argc < 2`。参数存在时，用 `atoi` 转成整数，再调用 `sleep(ticks)`。

这里的 `sleep` 不是自己实现计时，而是直接使用 xv6 内核已经提供的系统调用。用户程序只负责参数处理和调用入口。

### 核心思想

这个任务的核心是理解“用户态程序通过 `user/user.h` 中声明的函数进入系统调用”。`sleep.c` 本身很短，但它展示了 xv6 用户程序的基本结构：参数检查、调用系统调用、退出。

## 任务二：pingpong

### 实现的功能

`pingpong` 创建一个子进程，父进程先向子进程发送一个字节，子进程收到后打印 `received ping`，再把字节发回父进程，父进程收到后打印 `received pong`。

预期输出类似：

```text
子进程pid:received ping
父进程pid:received pong
```

### 核心修改代码片段

文件：`user/pingpong.c`

```c
int p1[2],p2[2];// p1: 父→子，p2: 子→父

pipe(p1);
pipe(p2);

//创建子进程
int pid=fork();
```

子进程核心逻辑：

```c
if(pid==0){
  //子进程
  close(p1[1]);//子进程不需要写p1
  close(p2[0]);//子进程不需要读p2

  char buf[1];
  //子进程读父进程
  read(p1[0],buf,1);
  printf("%d:received ping\n",getpid());
  write(p2[1],buf,1);

  close(p1[0]);
  close(p2[1]);
  exit(0);
}
```

父进程核心逻辑：

```c
else{
  //父进程
  close(p1[0]);
  close(p2[1]);

  char buf[1]={'x'};
  write(p1[1],buf,1);
  read(p2[0],buf,1);
  printf("%d:received pong\n",getpid());

  close(p1[1]);
  close(p2[0]);
  exit(0);
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

`fork()` 后父子进程会同时拥有两个 pipe 的读写端。为了让通信方向清晰，也为了避免多余文件描述符一直占用，父子进程都会关闭自己不用的端口。

### 核心思想

这个任务的核心是进程间通信模型：`fork` 复制文件描述符，`pipe` 提供字节流，`read/write` 完成同步。

父进程写入 `p1[1]` 后，子进程从 `p1[0]` 读；子进程再写入 `p2[1]`，父进程从 `p2[0]` 读。两个 pipe 刚好构成一次“ping-pong”往返。

## 总结

`util` 分支的两个任务都很小，但正好覆盖了 xv6 用户态编程最重要的入门能力：

- `sleep` 练习命令行参数和系统调用。
- `pingpong` 练习 `fork`、`pipe`、文件描述符和进程同步。

真正的核心不是代码量，而是理解 xv6 中“用户程序如何借助系统调用使用内核能力”。

