#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

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

//当用户定义的 handler 执行完毕后，必须调用这个系统调用来回到被中断打断的原始代码行。
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  //将之前备份在 alarm_trapframe 里的所有寄存器状态，原封不动地覆盖回当前的 p->trapframe）。
  memmove(p->trapframe, p->alarm_trapframe, sizeof(struct trapframe));

  //将标志位重新设为0，这相当于解锁
  p->is_alarming = 0;

  //如果直接返回 0，内核会自动把 a0 改成 0，这可能会破坏被中断程序原本在 a0 里的值
  return p->trapframe->a0;
}
