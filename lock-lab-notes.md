# MIT xv6 Lab: lock 分支实现说明

## 分支范围

本文档对应仓库 `Neyray/xv6-labs-2020` 的 `lock` 分支，重点根据当前分支中的 `kernel/kalloc.c`、`kernel/bio.c`、`kernel/buf.h` 和 `kernel/spinlock.c` 编写。

这个分支主要完成了 xv6 lock lab 的两个性能优化方向：

- 优化物理内存分配器，减少 `kmem` 全局锁竞争。
- 优化 buffer cache，减少 `bcache` 全局锁竞争。

此外，分支中也对 `spinlock.c` 添加了锁统计和自旋锁原理相关注释。

## 实现的功能

### 1. 每 CPU 物理页 freelist

原始 xv6 的物理内存分配器只有一个全局 `kmem`：

```c
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
```

多个 CPU 同时执行 `kalloc()` 或 `kfree()` 时，都会竞争同一把 `kmem.lock`。在高并发测试中，这会造成明显的锁争用。

当前分支将它改成：

```c
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];
```

也就是每个 CPU 都拥有自己的空闲页链表和自己的锁。大多数情况下，一个 CPU 只操作自己的 freelist，从而把原先集中在一把锁上的竞争分散到多把锁上。

### 2. 空闲页偷取机制

每 CPU freelist 带来一个问题：某个 CPU 的链表可能为空，而其他 CPU 的链表还有空闲页。

因此 `kalloc()` 中增加了 steal 逻辑：

- 先关闭中断并读取当前 CPU id。
- 优先从当前 CPU 的 `kmem[id].freelist` 取页。
- 如果当前 CPU 没有空闲页，就遍历其他 CPU 的 freelist。
- 找到可用页后，从其他 CPU 的链表中取走一个页面。
- 最后恢复中断。

这个机制保证了局部性和可用性之间的平衡：平时尽量本地分配，本地没有时再跨 CPU 获取。

### 3. 哈希桶 buffer cache

原始 xv6 的 buffer cache 使用一条全局 LRU 链表，并由一把 `bcache.lock` 保护。所有 block cache 查找、引用计数更新、LRU 更新都会竞争这同一把锁。

当前分支将 buffer cache 改为哈希桶结构：

```c
#define NBUCKET 13
#define HASH(blockno) ((blockno) % NBUCKET)
```

每个桶由 `struct hashbuf` 表示：

```c
struct hashbuf {
  struct buf head;
  struct spinlock lock;
};
```

`bcache` 中包含多个桶：

```c
struct hashbuf buckets[NBUCKET];
```

这样不同 block 会根据 `blockno` 分散到不同桶里。访问不同桶时，只需要拿对应桶的锁，不再集中竞争一把全局 `bcache.lock`。

### 4. 换出阶段串行化

哈希桶能减少命中路径的锁竞争，但 cache miss 时需要从所有桶里找一个可替换的 buffer。如果多个 CPU 同时做全局换出，容易出现重复选择、链表移动冲突或同一 block 被重复加入 cache 的问题。

因此分支中新增：

```c
struct spinlock eviction_lock;
```

它只在 miss 后进入换出阶段时使用，保证同一时间只有一个 CPU 执行“扫描所有桶、选择 LRU、移动 buffer 到目标桶”的全局操作。

这是一种折中设计：命中路径保持细粒度桶锁，复杂的换出路径用一把全局锁保证正确性。

### 5. 用 timestamp 维护 LRU 语义

原始 xv6 通过在全局链表中移动 `buf` 的位置表示最近使用顺序。改成哈希桶后，buffer 分散在多个桶里，再维护一条全局 LRU 链表会重新引入全局锁竞争。

当前分支在 `kernel/buf.h` 的 `struct buf` 中新增：

```c
uint timestamp;
```

当 `brelse()` 使 `refcnt` 降为 0 时，读取全局 `ticks` 并保存到 `b->timestamp`。换出时扫描所有桶，选择 `refcnt == 0` 且 `timestamp` 最小的 buffer，作为最久未使用的候选。

这保留了 LRU 的核心语义，同时避免每次释放 buffer 都要移动全局链表。

## 怎么实现的

### kalloc.c：拆分 kmem 锁

`kernel/kalloc.c` 的关键变化如下：

- `kmem` 从单个结构体改为 `kmem[NCPU]`。
- `kinit()` 循环初始化每个 CPU 的锁。
- `kfree()` 使用 `push_off()`/`pop_off()` 包住 `cpuid()`，避免读取 CPU id 期间被调度到其他 CPU。
- `kfree()` 将释放的页面插入当前 CPU 的 freelist。
- `kalloc()` 优先从当前 CPU freelist 取页面。
- 当前 CPU freelist 为空时，遍历其他 CPU freelist 偷取页面。

这里的核心正确性点是：读 `cpuid()` 前必须关中断，否则当前执行流可能被调度到其他 CPU，导致把页面放错链表或拿错锁。

### bio.c：拆分 bcache 锁

`kernel/bio.c` 的 `bget()` 被改造成两段式流程。

第一段是命中查找：

- 根据 `HASH(blockno)` 计算目标桶。
- 只加目标桶锁。
- 如果找到匹配的 `(dev, blockno)`，增加 `refcnt`，释放桶锁，获取 buffer 的 sleep lock，然后返回。

第二段是 miss 后换出：

- 获取 `eviction_lock`，串行化全局换出。
- 再次检查目标桶，避免别的 CPU 在空窗期已经把目标 block 加入 cache。
- 扫描所有桶，寻找 `refcnt == 0` 且 `timestamp` 最小的 buffer。
- 如果候选 buffer 不在目标桶，将它从原桶摘下并挂入目标桶。
- 更新 `dev`、`blockno`、`valid`、`refcnt`。
- 释放桶锁和 `eviction_lock`，获取 buffer 的 sleep lock 后返回。

这里“miss 后再次检查目标桶”很重要。因为第一次释放桶锁到拿到 `eviction_lock` 之间，其他 CPU 可能已经完成了同一个 block 的加载；如果不复查，就可能破坏“一块磁盘 block 在 cache 中只有一个 buf”的不变量。

### brelse/bpin/bunpin：只操作对应桶

`brelse()`、`bpin()` 和 `bunpin()` 都改为：

- 根据 `b->blockno` 计算桶 id。
- 只获取对应桶锁。
- 修改 `refcnt`。

`brelse()` 在 `refcnt` 变为 0 时更新 `timestamp`，用于后续 LRU 选择。

### spinlock.c：锁统计和注释

`kernel/spinlock.c` 中保留了 lock lab 的统计逻辑：

- 每次调用 `acquire()` 时增加 `lk->n`。
- 自旋失败时增加 `lk->nts`。
- `statslock()` 输出 `kmem` 和 `bcache` 相关锁的竞争统计。

分支还补充了对 RISC-V 原子交换的解释：`__sync_lock_test_and_set` 最终会落到类似 `amoswap` 的原子指令；循环中不断尝试把 `lk->locked` 从 0 改成 1，成功则拿到锁，失败则继续自旋。

## 核心思想

`lock` 分支的核心是“用细粒度锁减少共享热点，同时保留必要的不变量”。

对于 `kalloc`：

- 共享热点是单个 `kmem.lock`。
- 解决办法是按 CPU 拆分 freelist 和锁。
- 为避免某个 CPU 缺页，增加跨 CPU steal。

对于 `bcache`：

- 共享热点是单个 `bcache.lock`。
- 解决办法是按 blockno 哈希到多个 bucket，每个 bucket 一把锁。
- 命中路径只锁一个 bucket。
- miss 换出路径使用 `eviction_lock` 串行化，保证全局替换正确。
- 用 `timestamp` 替代全局链表移动来保留 LRU 近似语义。

这个分支不是简单地“把一把锁拆成很多把锁”，更重要的是识别哪些操作可以并发，哪些操作必须串行。命中查找和 refcnt 更新可以局部化；跨桶换出和全局 LRU 选择则需要额外保护。

## 关键文件

- `kernel/kalloc.c`：每 CPU freelist、`kalloc()` steal、`kfree()` 本地释放。
- `kernel/bio.c`：哈希桶 bcache、桶锁、全局换出锁、timestamp LRU。
- `kernel/buf.h`：为 `struct buf` 增加 `timestamp`。
- `kernel/spinlock.c`：锁竞争统计和自旋锁原理注释。
- `Makefile`：lock lab 下包含 `_kalloctest` 和 `_bcachetest`。

## 小结

这个分支完成了 lock lab 最核心的性能优化：把高频路径上的全局锁竞争拆散。`kalloc` 通过每 CPU freelist 降低内存分配压力，`bcache` 通过哈希桶降低 block cache 访问压力。真正的关键在于并发不变量：同一个空闲页不能被重复分配，同一个磁盘 block 在 cache 中不能出现多个有效副本，refcnt 和链表移动必须被正确加锁保护。

