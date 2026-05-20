# MIT xv6 Lab lock 分支实现说明

## 分支概览

`lock` 分支主要解决 xv6 中两个高竞争锁的性能问题：

- 任务一：优化物理内存分配器 `kalloc`，减少 `kmem` 锁竞争。
- 任务二：优化 buffer cache，减少 `bcache` 锁竞争。
- 任务三：补充 spinlock 竞争统计代码与注释。

这个 lab 的核心不是新增功能，而是**在保证内核数据一致性的前提下提升多 CPU 并发性能**。

---

## 背景知识：为什么 xv6 这两把锁会成为瓶颈？

### 1. xv6 的锁有两种

xv6 中常用两类锁：

| 锁类型 | 实现位置 | 特点 | 适合的场景 |
| --- | --- | --- | --- |
| **spinlock** | `kernel/spinlock.c` | 抢不到就忙等，持有期间**关中断**，**不能睡眠** | 持有时间极短的临界区，比如改链表、改计数器 |
| **sleeplock** | `kernel/sleeplock.c` | 抢不到就睡眠到下次被唤醒，可以睡，**底层依赖 spinlock** | 持有时间长的临界区，比如等磁盘 I/O |

物理内存分配器 `kmem` 和 buffer cache 的 bucket 锁都属于 spinlock：它们的临界区很短（一次链表插入/摘除），不应该睡眠。

### 2. spinlock 为什么持有期间必须关中断？

考虑反例：CPU0 持有锁 L，此时一个定时器中断让 CPU0 跳进 `kerneltrap → yield` 切换进程；这个新进程的代码又去抢锁 L。但 L 已经被 CPU0 持有，新进程就忙等，结果 CPU0 永远不能调度回去释放——死锁。

所以 `acquire()` 中要先 `push_off()`（保存原中断状态并关中断），`release()` 中再 `pop_off()`。

由此引出一个**重要约束**：调用 `cpuid()`、`mycpu()` 这类返回"当前 CPU"的函数前，**必须先关中断**，否则刚算出 CPU id 就被切到另一个 CPU 上，结果错误。本 lab 的 `kalloc/kfree` 中到处出现 `push_off/pop_off` 就是这个原因。

### 3. 为什么原版 kmem 是一把全局锁？

`xv6` 的 `kmem` 维护一条全局空闲页链表。`kalloc()` 摘链头，`kfree()` 加链头，两边都只能在加锁下做。

在单核系统下没问题；但 RISC-V 三核或八核同时跑用户程序时：

- 所有 `fork/exec` 都要 `kalloc`；
- 所有进程退出都要 `kfree`；
- 所有 page fault（lazy/COW）也要 `kalloc`。

于是所有 CPU 都抢同一把 `kmem.lock`，CPU 数越多锁的争用越严重。`kalloctest` 就是用来量化这件事的。

### 4. 为什么原版 bcache 是一把全局锁？

xv6 的 buffer cache 是一条 LRU 双向链表 + 一把 `bcache.lock`：

- 命中时调整链表顺序（移到头部表示最近使用）；
- 未命中时遍历链表找 `refcnt==0` 的最旧 buffer 复用。

不管访问的是哪个磁盘块，所有 CPU 都得抢同一把锁。`bcachetest` 会同时让多核访问大量不同的 block，结果锁竞争把吞吐压死。

### 5. 优化的核心思想：分片 + 局部性

无论是 `kmem` 还是 `bcache`，优化方向都是同一个：**把全局共享结构按 CPU 或哈希拆开，让大多数操作只触碰一个分片**。

但拆分会破坏全局不变量（比如"任何物理页只在一条 freelist 中"、"任何磁盘块只在一个 buf 中"），所以需要在少数跨片操作时用一把额外的锁串行化，把不变量补回来。

这套"细粒度锁 + 必要的全局锁"的设计，是高性能内核的常见手段。

---

## 任务一：优化 kalloc 物理内存分配器

### 原始问题

原始 xv6 只有一个全局空闲页链表和一把锁。多个 CPU 同时执行 `kalloc()` / `kfree()` 时，都会竞争同一把 `kmem.lock`。

### 实现的功能

当前分支将全局 freelist 改成每 CPU 一个 freelist：

- 每个 CPU 有自己的 `kmem[id].freelist`；
- 每个 CPU 有自己的 `kmem[id].lock`；
- `kfree()` 把释放的页放回**当前 CPU** 的 freelist；
- `kalloc()` 优先从**当前 CPU** freelist 分配；
- 当前 CPU 没空闲页时，遍历其他 CPU freelist **偷一页**过来用。

### 核心修改代码片段

文件：`kernel/kalloc.c`

#### 1. 将单个 kmem 改成 per-CPU 数组

```c
struct {
  struct spinlock lock;
  struct run *freelist;   // 空闲列表受自旋锁保护
} kmem[NCPU];             // 把单个 kmem 改造成数组，
                          // 每个 CPU 拥有独立的空闲链表 + 独立的锁
```

`NCPU` 是 xv6 支持的最大 CPU 数（默认 8）。这样每个核都有"自己的小金库"。

#### 2. 初始化每个 CPU 的锁

```c
void
kinit()
{
  char lockname[8];
  for(int i = 0; i < NCPU; ++i){
    snprintf(lockname, sizeof(lockname), "kmem_%d", i);
    initlock(&kmem[i].lock, "kmem");
  }
  freerange(end, (void*)PHYSTOP);
}
```

注意 `freerange` 在内部会反复调用 `kfree`，按下面 `kfree` 的实现，所有内存都会先挂进**调用 kinit 时的那个 CPU 的 freelist**（一般是启动 CPU 0）。这是合法的初始状态，因为别的 CPU 可以靠 steal 拿走。

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

  push_off();                          // 关中断, 防止 cpuid() 调用期间被迁核
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();                           // 恢复中断
}
```

关键点：

- `push_off/pop_off` 保证 `cpuid()` 的结果不会因为重新调度而失效；
- 只锁当前 CPU 的锁，绝大多数 `kfree` 不需要和其他核竞争。

#### 4. kalloc 优先本地分配, 不够就偷取

```c
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  else {
    // 当前 CPU 链表为空, 遍历其他 CPU 偷一页
    for(int antid = 0; antid < NCPU; ++antid){
      if(antid == id) continue;
      acquire(&kmem[antid].lock);
      r = kmem[antid].freelist;
      if(r){
        kmem[antid].freelist = r->next;
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

实现细节：

- 走本地路径只需要一把锁；
- 偷的时候必须**同时持有自己的锁 + 目标 CPU 的锁**：自己的锁防止"等下别的 CPU 看到我也没有空闲页又跑来偷我"；
- 偷完只摘下一页，不是一次搬一大批——简单实现，避免在持锁期间做太多工作。

### 怎么实现的

把"所有 CPU 抢一把锁"改成"每个 CPU 大多数时间只抢自己的锁"。`push_off()` / `pop_off()` 包夹 `cpuid()` 是这里最容易踩坑的细节：忘了关中断，本地核刚算出 id 就被调度，后面的 acquire 就锁错了对象，看似无错实则破坏正确性。

### 核心思想

降低共享热点。空闲页链表不必全局唯一，只要保证：

1. 每个物理页**任意时刻只在一条** freelist 上；
2. 跨 CPU steal 时正确加锁；

就能在不破坏正确性的前提下减少争用。

---

## 任务二：优化 buffer cache

### 原始问题

原始 xv6 的 buffer cache 使用一条全局 LRU 链表，由一把 `bcache.lock` 保护。不同 CPU 即使访问不同磁盘块，也会竞争同一把锁。

### 实现的功能

当前分支将 buffer cache 改成哈希桶：

- 按 `blockno` 计算 bucket（`HASH(blockno) = blockno % 13`）；
- 每个 bucket 一条链表 + 一把锁；
- cache hit 时**只锁对应 bucket**；
- cache miss 需要全局换出时，用 `eviction_lock` 串行化；
- 用 `timestamp` 替代"在 LRU 链表里挪位置"。

### 核心修改代码片段

文件：`kernel/bio.c`

#### 1. 定义 bucket 和哈希函数

```c
#define NBUCKET 13
#define HASH(blockno) ((blockno) % NBUCKET)

// hash 桶是存储 buf 的, buf 总共有 NBUF (默认 30) 个
struct hashbuf {
  struct buf head;
  struct spinlock lock;
};
```

桶数取一个**质数**（这里 13）可以让取模分布更均匀，避免常见的"2 的幂"导致的对齐冲突。

#### 2. 改造 bcache 结构

```c
struct {
  struct spinlock eviction_lock;  // 仅用于换出时串行化
  struct buf buf[NBUF];

  struct hashbuf buckets[NBUCKET]; // 散列桶
} bcache;
```

为什么需要 `eviction_lock`？因为 cache miss 时要扫所有桶选 LRU，并把它从原桶搬到目标桶——这是个跨桶操作，如果两个 CPU 同时换出可能选同一个 buf，必须串行化。

#### 3. 初始化所有 bucket

```c
void
binit(void)
{
  struct buf *b;
  char lockname[16];

  initlock(&bcache.eviction_lock, "bcache_eviction");

  for(int i = 0; i < NBUCKET; ++i){
    snprintf(lockname, sizeof(lockname), "bcache_bucket_%d", i);
    initlock(&bcache.buckets[i].lock, lockname);
    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
  }

  // 初始全部挂到 bucket[0]
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    b->next = bcache.buckets[0].head.next;
    b->prev = &bcache.buckets[0].head;
    b->timestamp = 0;
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[0].head.next->prev = b;
    bcache.buckets[0].head.next = b;
  }
}
```

初始时所有 buf 挂在 bucket[0]，运行时随访问被搬到其他桶。

#### 4. cache hit: 只查目标 bucket

```c
int bid = HASH(blockno);

acquire(&bcache.buckets[bid].lock);
for(b = bcache.buckets[bid].head.next; b != &bcache.buckets[bid].head; b = b->next){
  if(b->dev == dev && b->blockno == blockno){
    b->refcnt++;
    release(&bcache.buckets[bid].lock);
    acquiresleep(&b->lock);
    return b;
  }
}
release(&bcache.buckets[bid].lock);
```

命中路径只操作一个桶，因此不同 block 落在不同桶时可以并发访问。

#### 5. cache miss: 用 eviction_lock 串行化换出

```c
acquire(&bcache.eviction_lock);

// 必须再查一次！
// 从释放桶锁到拿到 eviction_lock 之间, 别的 CPU 可能已经把这个块加进了 cache
acquire(&bcache.buckets[bid].lock);
for(b = bcache.buckets[bid].head.next; b != &bcache.buckets[bid].head; b = b->next){
  if(b->dev == dev && b->blockno == blockno){
    b->refcnt++;
    release(&bcache.buckets[bid].lock);
    release(&bcache.eviction_lock);
    acquiresleep(&b->lock);
    return b;
  }
}
release(&bcache.buckets[bid].lock);
```

**二次检查**是必须的：第一次 miss 后到拿到 `eviction_lock` 之间，其他 CPU 可能已经把同一个 block 加进了 cache。如果不复查，就会出现"同一磁盘块对应两个 buffer"的灾难（内核数据一致性被破坏）。

#### 6. 扫描所有 bucket 选择 LRU buffer

```c
struct buf *lru_b = 0;
int lru_bid = -1;

for(int i = 0; i < NBUCKET; ++i){
  acquire(&bcache.buckets[i].lock);
  int found_new = 0;
  for(b = bcache.buckets[i].head.next; b != &bcache.buckets[i].head; b = b->next){
    if(b->refcnt == 0 && (lru_b == 0 || b->timestamp < lru_b->timestamp)){
      if(lru_bid != -1 && lru_bid != i){
        release(&bcache.buckets[lru_bid].lock);
      }
      lru_b = b;
      lru_bid = i;
      found_new = 1;
    }
  }
  if(!found_new && i != lru_bid){
    release(&bcache.buckets[i].lock);
  }
}

if(lru_b == 0){
  panic("bget: no buffers");
}
```

选 LRU buffer 的条件：

- `refcnt == 0`：没人在用；
- `timestamp` 最小：最久没人用。

这段写得稍微绕，因为它"持有当前最优桶的锁"贯穿整个扫描——找到更优的就先放掉旧的，然后保住新的。这样扫描结束时正好持有 `lru_bid` 的桶锁。

#### 7. 必要时把 LRU buffer 移到目标 bucket

```c
if(lru_bid != bid){
  // 从原桶摘下
  lru_b->next->prev = lru_b->prev;
  lru_b->prev->next = lru_b->next;
  release(&bcache.buckets[lru_bid].lock);

  // 挂到目标桶
  acquire(&bcache.buckets[bid].lock);
  lru_b->next = bcache.buckets[bid].head.next;
  lru_b->prev = &bcache.buckets[bid].head;
  bcache.buckets[bid].head.next->prev = lru_b;
  bcache.buckets[bid].head.next = lru_b;
}

lru_b->dev = dev;
lru_b->blockno = blockno;
lru_b->valid = 0;
lru_b->refcnt = 1;

release(&bcache.buckets[bid].lock);
release(&bcache.eviction_lock);
acquiresleep(&lru_b->lock);
return lru_b;
```

如果 LRU buffer 不在目标桶，需要先从原桶摘下、再挂入目标桶。这样后续根据 `HASH(blockno)` 查找时才能在正确 bucket 找到它。

#### 8. brelse 中更新时间戳

文件：`kernel/bio.c`

```c
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bid = HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;

  if (b->refcnt == 0) {
    acquire(&tickslock);
    b->timestamp = ticks;
    release(&tickslock);
  }

  release(&bcache.buckets[bid].lock);
}
```

文件：`kernel/buf.h`

```c
uint timestamp;   // 原先做法是通过"在全局链表里挪位置"表示新旧顺序,
                  // 现在使用时间戳来表示
```

为什么不在 LRU 链表里挪位置？因为链表挪位需要持有"包含所有 buf 的那一把锁"——这正是我们要消除的全局锁。改用时间戳后，每个 buf 在哪个桶都行，反正比较的是数字而不是链表位置。

### 怎么实现的

把 bcache 的并发访问分成两类：

- **常见路径**：cache hit，只需要目标 bucket lock；
- **少见路径**：cache miss，需要全局选择可替换 buffer，因此用 `eviction_lock` 串行化。

这样既降低了常见路径的锁竞争，又避免了换出时破坏全局不变量。

### 核心思想

细粒度锁和不变量保护的平衡：

- bucket lock 提高并发；
- `eviction_lock` 保证全局换出正确；
- `timestamp` 让 LRU 判断不再依赖一条全局链表，从而减少锁竞争；
- 二次检查处理"两个 CPU 想 cache 同一个 block"的竞态。

---

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

- `lk->n`：调用 `acquire()` 的次数；
- `lk->nts`：自旋失败次数，即没能立刻拿到锁、需要继续等待的次数。

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
       strncmp(locks[i]->name, "kmem",   strlen("kmem"))   == 0) {
      tot += locks[i]->nts;
      n += snprint_lock(buf + n, sz - n, locks[i]);
    }
  }
  ...
  release(&lock_locks);
  return n;
}
```

### 核心思想

锁优化不能只靠感觉，必须能量化。`kalloctest` 和 `bcachetest` 关注 `kmem`/`bcache` 相关锁的自旋次数，优化目标就是让这些热点锁的 `nts` 明显下降到接近零。`__sync_fetch_and_add` 是 GCC 内建的原子递增，自身不需要再加锁。

---

## 总结

`lock` 分支的三个任务可以概括为：

- **kalloc**：把一个全局 freelist 拆成每 CPU freelist，减少内存分配锁竞争（核心是 per-CPU + steal）；
- **bcache**：把一个全局 cache 链表拆成多个哈希桶，减少块缓存锁竞争（核心是 bucket lock + eviction_lock + timestamp）；
- **spinlock**：通过统计自旋失败次数观察锁竞争。

最核心的代码思想是：**高频局部操作用细粒度锁，全局复杂操作保留必要的串行化**。这样既能提升并发，又不破坏 xv6 内核必须维护的数据一致性。

一些可推广的经验：

1. 使用 per-CPU 结构时必须 `push_off/pop_off` 包夹 `cpuid()`；
2. 多锁场景下要约定**固定加锁顺序**避免死锁（本实验中 `eviction_lock` 永远在 `bucket lock` 之外，扫描时也按 bucket 序号升序处理）；
3. 看似简单的"分桶哈希"也会引入新的竞态，比如两个 CPU 同时换出同一块到同一桶——这时必须有一道"再查一次"或一把全局锁来兜底。

