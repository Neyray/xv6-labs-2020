# MIT xv6 Lab lock 分支实现说明

## 分支概览

`lock` 分支主要解决 xv6 中两个高竞争锁的问题：

- 任务一：优化物理内存分配器 `kalloc`，减少 `kmem` 锁竞争。
- 任务二：优化 buffer cache，减少 `bcache` 锁竞争。
- 任务三：补充 spinlock 竞争统计和自旋锁原理注释。

这个 lab 的核心不是新增功能，而是提高多 CPU 并发场景下的性能，同时保持内核数据结构的一致性。

## 任务一：优化 kalloc 物理内存分配器

### 原始问题

原始 xv6 只有一个全局空闲页链表和一把锁。多个 CPU 同时执行 `kalloc()` / `kfree()` 时，都会竞争同一把 `kmem.lock`。

### 实现的功能

当前分支将全局 freelist 改成每 CPU 一个 freelist：

- 每个 CPU 有自己的 `kmem[id].freelist`。
- 每个 CPU 有自己的 `kmem[id].lock`。
- `kfree()` 把释放的页放回当前 CPU 的 freelist。
- `kalloc()` 优先从当前 CPU freelist 分配。
- 当前 CPU 没有空闲页时，从其他 CPU freelist 偷取一页。

### 核心修改代码片段

文件：`kernel/kalloc.c`

#### 1. 将单个 kmem 改成 per-CPU 数组

```c
struct {
  struct spinlock lock;
  struct run *freelist;   //空闲列表受到自旋锁（spin lock）的保护
} kmem[NCPU];    //把单个kmem改造成数组，每个CPU拥有独立的空闲链表+独立的锁
```

#### 2. 初始化每个 CPU 的锁

```c
void
kinit()
{
  char lockname[8];
  for(int i=0;i<NCPU;++i){
    snprintf(lockname,sizeof(lockname),"kmem_%d",i);
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}
```

#### 3. kfree 释放到当前 CPU 的 freelist

```c
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

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
```

#### 4. kalloc 优先本地分配，不够就偷取

```c
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
      if(antid==id)continue;
      acquire(&kmem[antid].lock);
      r=kmem[antid].freelist;
      if(r){
        kmem[antid].freelist=r->next;
        release(&kmem[antid].lock);
        break;
      }
      release(&kmem[antid].lock);
    }
  }
  release(&kmem[id].lock);
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
```

### 怎么实现的

这个任务把“所有 CPU 抢一把锁”改成“每个 CPU 大多数时间只抢自己的锁”。`push_off()` / `pop_off()` 很关键，因为 `cpuid()` 必须在关闭中断时使用，避免当前执行流在读取 CPU id 之后被切换到另一个 CPU 上。

### 核心思想

核心是降低共享热点。空闲页链表不一定必须全局唯一，只要保证每个页面只在某一个 freelist 中，并且跨 CPU steal 时正确加锁，就能在保证正确性的同时减少锁竞争。

## 任务二：优化 buffer cache

### 原始问题

原始 xv6 的 buffer cache 使用一条全局 LRU 链表，由一把 `bcache.lock` 保护。不同 CPU 即使访问不同磁盘块，也会竞争同一把锁。

### 实现的功能

当前分支将 buffer cache 改成哈希桶：

- 根据 `blockno` 计算 bucket。
- 每个 bucket 有自己的链表和锁。
- cache hit 时只锁对应 bucket。
- cache miss 需要全局换出时，用 `eviction_lock` 串行化。
- 用 `timestamp` 替代全局 LRU 链表移动。

### 核心修改代码片段

文件：`kernel/bio.c`

#### 1. 定义 bucket 和哈希函数

```c
#define NBUCKET 13
#define HASH(blockno)((blockno)%NBUCKET)

//hash桶是存储buf的，buf总共有三十个
struct hashbuf{
  struct buf head;
  struct spinlock lock;
};
```

#### 2. 改造 bcache 结构

```c
struct {
  struct spinlock eviction_lock;   //仅用于换出时串行化
  struct buf buf[NBUF];

  struct hashbuf buckets[NBUCKET];   //散列桶
} bcache;
```

#### 3. 初始化所有 bucket

```c
void
binit(void)
{
  struct buf *b;
  char lockname[16];

  initlock(&bcache.eviction_lock, "bcache_eviction");

  for(int i=0;i<NBUCKET;++i){
    snprintf(lockname,sizeof(lockname),"bcache_bucket_%d",i);
    initlock(&bcache.buckets[i].lock,lockname);
    bcache.buckets[i].head.prev=&bcache.buckets[i].head;
    bcache.buckets[i].head.next=&bcache.buckets[i].head;
  }

  //初始全部挂到bucket[0]
  for(b=bcache.buf;b<bcache.buf+NBUF;b++){
    b->next=bcache.buckets[0].head.next;
    b->prev=&bcache.buckets[0].head;
    b->timestamp=0;
    initsleeplock(&b->lock,"buffer");
    bcache.buckets[0].head.next->prev=b;
    bcache.buckets[0].head.next=b;
  }
}
```

#### 4. cache hit：只查目标 bucket

```c
int bid=HASH(blockno);

acquire(&bcache.buckets[bid].lock);
for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
  if(b->dev==dev && b->blockno==blockno){
    b->refcnt++;
    release(&bcache.buckets[bid].lock);
    acquiresleep(&b->lock);
    return b;
  }
}
release(&bcache.buckets[bid].lock);
```

命中路径只操作一个桶，因此不同 block 落在不同桶时可以并发访问。

#### 5. cache miss：用 eviction_lock 串行化换出

```c
acquire(&bcache.eviction_lock);

//必须再查一次！！！
//从释放桶锁到拿到 eviction_lock 之间，别的 CPU 可能已经把这个块加进了 cache
acquire(&bcache.buckets[bid].lock);
for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
  if(b->dev==dev && b->blockno==blockno){
    b->refcnt++;
    release(&bcache.buckets[bid].lock);
    release(&bcache.eviction_lock);
    acquiresleep(&b->lock);
    return b;
  }
}
release(&bcache.buckets[bid].lock);
```

这里二次检查很关键：第一次 miss 后到拿到 `eviction_lock` 之间，其他 CPU 可能已经把同一个 block 加入 cache。如果不复查，就可能出现同一个磁盘块对应多个 buffer 的错误。

#### 6. 扫描所有 bucket 选择 LRU buffer

```c
struct buf *lru_b=0;
int lru_bid=-1;

for(int i=0;i<NBUCKET;++i){
  acquire(&bcache.buckets[i].lock);
  int found_new=0;
  for(b=bcache.buckets[i].head.next;b!=&bcache.buckets[i].head;b=b->next){
    if(b->refcnt==0 && (lru_b==0 || b->timestamp < lru_b->timestamp)){
      if(lru_bid!=-1 && lru_bid!=i){
        release(&bcache.buckets[lru_bid].lock);
      }
      lru_b=b;
      lru_bid=i;
      found_new=1;
    }
  }
  if(!found_new && i!=lru_bid){
    release(&bcache.buckets[i].lock);
  }
}

if(lru_b==0){
  panic("bget:no buffers");
}
```

选择条件是 `refcnt == 0`，说明没有进程正在使用；`timestamp` 最小，说明最久没有被使用。

#### 7. 必要时把 LRU buffer 移到目标 bucket

```c
if(lru_bid!=bid){
  //从原桶摘下
  lru_b->next->prev=lru_b->prev;
  lru_b->prev->next=lru_b->next;
  release(&bcache.buckets[lru_bid].lock);

  //挂到目标桶
  acquire(&bcache.buckets[bid].lock);
  lru_b->next=bcache.buckets[bid].head.next;
  lru_b->prev = &bcache.buckets[bid].head;
  bcache.buckets[bid].head.next->prev = lru_b;
  bcache.buckets[bid].head.next = lru_b;
}

lru_b->dev=dev;
lru_b->blockno=blockno;
lru_b->valid=0;
lru_b->refcnt=1;

release(&bcache.buckets[bid].lock);
release(&bcache.eviction_lock);
acquiresleep(&lru_b->lock);
return lru_b;
```

如果 LRU buffer 不在目标桶，需要先从原桶链表摘下，再挂入目标桶。这样后续根据 `HASH(blockno)` 查找时才能在正确 bucket 找到它。

#### 8. brelse 中更新时间戳

文件：`kernel/bio.c`

```c
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bid=HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;

  if (b->refcnt == 0) {
    acquire(&tickslock);
    b->timestamp=ticks;
    release(&tickslock);
  }

  release(&bcache.buckets[bid].lock);
}
```

文件：`kernel/buf.h`

```c
uint timestamp;   //原先做法是通过“在全局链表里挪位置”表示新旧顺序,现在使用时间戳来表示
```

### 怎么实现的

这个任务把 bcache 的并发访问分成两类：

- 常见路径：cache hit，只需要目标 bucket lock。
- 少见路径：cache miss，需要全局选择可替换 buffer，因此用 `eviction_lock` 串行化。

这样既降低了常见路径的锁竞争，又避免了换出时破坏全局不变量。

### 核心思想

核心是细粒度锁和不变量保护的平衡。bucket lock 提高并发，`eviction_lock` 保证全局换出正确。`timestamp` 则让 LRU 判断不再依赖一条全局链表，从而减少锁竞争。

## 任务三：spinlock 统计与注释

### 实现的功能

`spinlock.c` 中保留并完善了 lock lab 的统计逻辑，用于观察哪些锁发生了严重竞争。

### 核心修改代码片段

文件：`kernel/spinlock.c`

```c
#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->n), 1);
#endif
```

```c
while(__sync_lock_test_and_set(&lk->locked, 1) != 0) {
#ifdef LAB_LOCK
    __sync_fetch_and_add(&(lk->nts), 1);
#else
   ;
#endif
}
```

含义：

- `lk->n`：调用 `acquire()` 的次数。
- `lk->nts`：自旋失败次数，也就是没有立刻拿到锁、需要继续等待的次数。

文件：`kernel/spinlock.c`

```c
int
statslock(char *buf, int sz) {
  int n;
  int tot = 0;

  acquire(&lock_locks);
  n = snprintf(buf, sz, "--- lock kmem/bcache stats\n");
  for(int i = 0; i < NLOCK; i++) {
    if(locks[i] == 0)
      break;
    if(strncmp(locks[i]->name, "bcache", strlen("bcache")) == 0 ||
       strncmp(locks[i]->name, "kmem", strlen("kmem")) == 0) {
      tot += locks[i]->nts;
      n += snprint_lock(buf +n, sz-n, locks[i]);
    }
  }
  ...
  release(&lock_locks);
  return n;
}
```

### 核心思想

锁优化不能只靠感觉，需要通过统计看竞争是否下降。`kalloctest` 和 `bcachetest` 会关注 `kmem`、`bcache` 相关锁的自旋次数，优化目标就是让这些热点锁的争用明显下降。

## 总结

`lock` 分支的三个任务可以概括为：

- `kalloc`：把一个全局 freelist 拆成每 CPU freelist，减少内存分配锁竞争。
- `bcache`：把一个全局 cache 链表拆成多个哈希桶，减少块缓存锁竞争。
- `spinlock`：通过统计自旋失败次数观察锁竞争。

最核心的代码思想是：高频局部操作用细粒度锁，全局复杂操作保留必要的串行化。这样既能提升并发，又不破坏 xv6 内核必须维护的数据一致性。

