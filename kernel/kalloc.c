// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.
//每个空闲页的列表元素是一个struct run
struct run {
  struct run *next;
};

//将每个空闲页的run结构存储在空闲页本身
struct {
  struct spinlock lock;
  struct run *freelist;   //空闲列表受到自旋锁（spin lock）的保护,列表和锁被封装在一个结构体中，以明确锁在结构体中保护的字段
} kmem[NCPU];    //把单个kmem改造成数组，每个CPU拥有独立的空闲链表+独立的锁

//分配器开始时没有内存；这些对kfree的调用给了它一些管理空间
//kinit只由一个CPU调用，所以freerange->free会把所有页面都挂在当前CPU的链表上，其他CPu需要时再去偷
void
kinit()
{
  char lockname[8];
  for(int i=0;i<NCPU;++i){
	  snprintf(lockname,sizeof(lockname),"kmem_%d",i);
	  initlock(&kmem[i].lock, "kmem");    //kinit初始化空闲列表以保存从内核结束到PHYSTOP之间的每一页
  }
  freerange(end, (void*)PHYSTOP);   //kinit调用freerange将内存添加到空闲列表中，在freerange中每页都会调用kfree
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//
// 函数kfree (kernel/kalloc.c:47)首先将内存中的每一个字节设置为1。
// 这将导致使用释放后的内存的代码（使用“悬空引用”）读取到垃圾信息而不是旧的有效内容，从而希望这样的代码更快崩溃
// kfree将页面前置（头插法）到空闲列表中：它将pa转换为一个指向struct run的指针r，在r->next中记录空闲列表的旧开始，并将空闲列表设置为等于r
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();   //关中断（防止调用cpuid期间被调度到其他CPU）
  int id=cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();    //开中断
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// kalloc删除并返回空闲列表中的第一个元素。
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id=cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  else{
	  //当前CPU链表为空，遍历其他CPU偷一页
	  for(int antid=0;antid<NCPU;++antid){
		  if(antid==id)continue;   //这是遍历自己的情况
		  acquire(&kmem[antid].lock);
		  r=kmem[antid].freelist;
		  if(r){
			  //就从这里偷了
			  kmem[antid].freelist=r->next;
			  release(&kmem[antid].lock);
			  break;
		  }
		  release(&kmem[antid].lock);
	  }
  }
  release(&kmem[id].lock);
  pop_off();    //打开中断

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
