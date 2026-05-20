# MIT xv6 Lab lazy 分支实现说明

## 分支概览

`lazy` 分支实现了 **lazy allocation（惰性分配）**：用户调用 `sbrk(n)` 申请扩堆时，内核只增大 `p->sz`，不真正分配物理页；等程序第一次访问那段地址引发 page fault 时，内核才在 `usertrap()` 里现场分配并建立映射。

涉及四个文件的修改：

- `kernel/sysproc.c` —— 改写 `sys_sbrk`：增大不分配，缩小走原路径；
- `kernel/trap.c` —— `usertrap()` 增加 page fault 处理；
- `kernel/vm.c` —— `uvmunmap`、`uvmcopy` 容忍"PTE 不存在"的情况；
- `kernel/syscall.c` —— `argaddr` 中补齐用户传入的 lazy 地址。

---

## 背景知识

### 1. 页面错误（page fault）不是"坏事"

CPU 把虚拟地址翻译成物理地址时，如果页表里没有合法映射，就触发 page fault。RISC-V 中区分三类：

| scause 值 | 类型 | 触发条件 |
| --- | --- | --- |
| 12 | instruction page fault | 取指失败 |
| 13 | load page fault | 读内存失败 |
| 15 | store page fault | 写内存失败 |

异常发生时硬件会写两个寄存器：

- `scause`：异常原因；
- `stval`：导致异常的虚拟地址。

xv6 默认对用户 page fault 的反应是"杀死进程"。但现代操作系统会把 page fault 当成一种**机制**：内核可以故意在页表里留下"不可访问"的标记，等程序真的访问时再补齐。这套思路衍生出：

- **lazy allocation**：sbrk 不真正分配，访问时再分配；
- **copy-on-write fork**：fork 时父子共享页面并标记只读，写时才复制；
- **demand paging from disk**：物理内存换出到磁盘，访问时再换入；
- **栈自动扩展**：栈访问越过边界时自动扩；
- **mmap 文件映射**：访问时按需从文件读入。

它们的统一模式都是：**"先在页表里设置一个会触发 page fault 的状态 → 程序访问时 page fault → 内核补齐 → 重新执行原指令"**。

### 2. xv6 中 sbrk 原本怎么做？

原始 `sys_sbrk` 直接调用 `growproc(n)`：

- 把 `p->sz` 增大 n；
- 调 `uvmalloc` **立刻** `kalloc` 出 n / PGSIZE 个物理页；
- 调 `mappages` 把它们装进用户页表。

问题在于，很多用户程序 `sbrk` 申请的内存并不会全部用上（malloc 通常一次扩很多）。这部分"被申请但没访问"的内存被白白吃掉了。

### 3. lazy allocation 改成什么样？

改造后：

- **增大** `sbrk(n>0)`：只让 `p->sz += n`，**不动页表**；
- **缩小** `sbrk(n<0)`：仍然立刻 `uvmdealloc` 释放页（缩小不能 lazy，否则物理页就泄漏了）；
- 当用户程序访问 `p->sz` 范围内、但尚未映射的虚拟页时，`usertrap` 捕获 page fault，`kalloc` 出一页 + `mappages` 把映射补上，然后 `sret` 重新执行那条指令。

注意"用户程序访问"还包括两条隐蔽路径：

1. **内核代表用户访问**用户内存：典型的就是 `read()/write()/exec()` 中传给内核的用户指针。这条路径不会触发硬件 page fault（内核访问内存时走的是内核页表 + 直接映射），但内核会调用 `walkaddr` 查用户页表，如果发现 PTE 缺失就返回 0 —— 此时内核必须自己补一页，否则系统调用会失败。这就是为什么 `argaddr` 也要改。
2. **fork 复制页表**：`uvmcopy` 会遍历 `[0, p->sz)` 的每一页复制；但 lazy 让中间可能有"还没访问过、PTE 缺失"的页，原版 `uvmcopy` 会 panic，必须改成 `continue`。
3. **进程退出 unmap**：`uvmunmap` 释放 `[0, p->sz)` 时同样可能碰到"被 sbrk 增长但从未访问"的页，原版也会 panic，必须改成 `continue`。

### 4. 合法 fault 地址的边界

判断一次 page fault 是不是"合法的 lazy 缺页"，需要两条判据：

```text
PGROUNDUP(p->trapframe->sp) - 1 < fault_va  &&  fault_va < p->sz
```

- 下界：`PGROUNDUP(sp) - 1`。**用户栈下面紧挨一个 guard page**，未映射、专门用来抓栈溢出。如果 fault_va 落在 guard page 或更低（位于栈和栈底之间的"无人区"），那就是真的非法访问，应该杀进程，而不是补页。
- 上界：`p->sz`。说明这地址确实在进程"声明拥有"的堆范围里。

只有同时满足这两条，才是 lazy 缺页，可以补。

---

## 实现总览

下面四节按文件展开，把每一处改动都拉出来加注释。

---

## 修改 1：`kernel/sysproc.c` — sys_sbrk 改成 lazy

```c
uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;

  struct proc *p = myproc();
  addr = p->sz;        // sbrk 的语义: 返回扩展前的旧 brk
  uint64 sz = p->sz;

  if(n > 0) {
    // 关键点: 只增大 p->sz, 不调用 growproc 也不分配物理页
    // 等用户真正访问这段地址时由 usertrap() 现场分配
    p->sz += n;
  } else if(sz + n > 0) {
    // 缩小仍然走原路径: 必须立刻释放对应物理页, 不然就泄漏了
    sz = uvmdealloc(p->pagetable, sz, sz + n);
    p->sz = sz;
  } else {
    // n < 0 且会把 sz 缩到 0 以下: 不合法
    return -1;
  }
  return addr;
}
```

要点：

- 增长 (n>0)：**不分配**，只改 `p->sz`；
- 缩小 (n<0)：**立刻释放**物理页（lazy 思想不适用于"释放"，否则物理页会失踪）；
- 边界保护：缩到 0 以下拒绝。

---

## 修改 2：`kernel/trap.c` — usertrap 处理 page fault

在 `usertrap()` 的 `if/else if` 链里追加一个分支：

```c
} else if(r_scause() == 13 || r_scause() == 15) {
    // lazy allocation 页面错误
    // scause == 13 是 load page fault, 15 是 store page fault
    // (我们不处理 12 = instruction page fault, 因为代码段不应该 lazy)

    uint64 fault_va = r_stval();   // 拿出错的虚拟地址
    char *pa;

    // 合法范围:
    //   下界: PGROUNDUP(sp) - 1 < va   → 在 guard page 之上
    //   上界: va < p->sz              → 在进程声明的地址空间内
    // 并且还要有物理内存可分配
    if(PGROUNDUP(p->trapframe->sp) - 1 < fault_va
       && fault_va < p->sz
       && (pa = kalloc()) != 0) {

       memset(pa, 0, PGSIZE);     // 新页清零, 防止把内核数据泄给用户
       if(mappages(p->pagetable, PGROUNDDOWN(fault_va), PGSIZE,
                   (uint64)pa, PTE_R | PTE_W | PTE_X | PTE_U) != 0) {
          // 建立映射失败 (页表分配不出来): 释放刚分的物理页, 杀进程
          kfree(pa);
          p->killed = 1;
       }
       // 如果走到这里没 kill, 那 PTE 已建好, sret 回去会重新执行原指令
    } else {
       // fault_va 不在合法范围, 或 kalloc 失败 → 杀进程
       p->killed = 1;
    }
}
```

注意几个细节：

1. **`PGROUNDDOWN(fault_va)`**：补的是包含 fault_va 的整页，对齐到 4K。
2. **`PTE_R | PTE_W | PTE_X | PTE_U`**：lazy 堆页可读写可执行，并标记为用户页。
3. **`memset(pa, 0, PGSIZE)`**：清零是安全要求——不清零可能把别的进程或内核遗留的数据泄漏出去。
4. **mappages 失败也要 kfree**：避免物理页泄漏。
5. **不处理 scause==12**：取指 fault 不应该出现在 lazy 堆里（堆里没有代码），出现就是真的出错。

返回路径不需要特殊处理：`usertrap` 处理完后正常走 `usertrapret → userret → sret`。`sret` 会根据 `sepc` 重新执行触发异常的那条指令，这次因为页表已经补上，就能成功。

---

## 修改 3：`kernel/vm.c` — uvmunmap & uvmcopy 容忍缺页

### 3.1 uvmunmap

进程退出时会用 `uvmunmap` 释放 `[0, p->sz)`。lazy 之后，这段区间里可能有从未访问过的虚拟页，对应 PTE 还是 0。原版遇到这种情况会 panic，必须放过。

```c
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
   uint64 a;
   pte_t *pte;

   if((va % PGSIZE) != 0)
     panic("uvmunmap: not aligned");

   for(a = va; a < va + npages * PGSIZE; a += PGSIZE){
     if((pte = walk(pagetable, a, 0)) == 0)
       continue;                  // ← 原来是 panic("uvmunmap: walk")
                                  //   走到这里说明该 va 连中间级页表都没建,
                                  //   是 lazy 但从未访问过的页, 跳过就行
     if((*pte & PTE_V) == 0)
       continue;                  // ← 原来是 panic("uvmunmap: not mapped")
                                  //   PTE 存在但无效, 同样是 lazy 占位, 跳过
     if(PTE_FLAGS(*pte) == PTE_V)
       panic("uvmunmap: not a leaf");   // 只有 V 没有 RWX, 是个目录页, 不该出现在叶子层
     if(do_free){
       uint64 pa = PTE2PA(*pte);
       kfree((void*)pa);          // 真正映射了物理页, 该释放
     }
     *pte = 0;                    // 清掉 PTE
   }
}
```

为什么 `walk` 返回 0 可能发生？因为 RISC-V Sv39 是三级页表，如果上层目录都没建（lazy 后从未触摸过这一段），`walk` 直接返回 0。这是合法状态，不是 bug。

### 3.2 uvmcopy

`fork` 时调 `uvmcopy(old, new, sz)`，遍历 `[0, sz)` 把父进程每一页复制给子进程。lazy 之后中间可能有缺页：

```c
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;                   // ← 原来是 panic("uvmcopy: pte should exist")
                                  //   父进程这一页是 lazy 但从未访问
                                  //   不复制, 让子进程将来 fault 时也走 lazy
    if((*pte & PTE_V) == 0)
      continue;                   // ← 原来是 panic("uvmcopy: page not present")
                                  //   同上, lazy 占位, 跳过
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);    // 物理页内容复制给子进程
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;
 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
```

行为变化：父进程中尚未实际分配的页（lazy 占位），子进程也维持"lazy 占位"，将来访问时自己 fault 一次再补。语义上一致，开销最低。

---

## 修改 4：`kernel/syscall.c` — argaddr 兜底 lazy 地址

这是最容易被遗漏的一步。考虑这个调用：

```c
char buf[N];
sbrk(8192);            // lazy: p->sz 增加, 但页表没建
read(0, sbrk(0)-8192, 100);   // 把这块"还没真分配"的内存当读缓冲
```

`read` 进入内核后，`sys_read` 会调用 `argaddr(1, &p)` 拿用户地址，然后通过 `copyout` 把数据写进去。`copyout` 内部走 `walkaddr` 查用户页表——这时发现该 va 没映射，返回 0，`copyout` 失败，`read` 返回 -1。

为了让 lazy 对系统调用透明，**在 `argaddr` 里就把"用户传进来但还没映射的合法 lazy 地址"补上**：

```c
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  struct proc *p = myproc();

  // 用户传进来的地址若在 lazy 区域且还没映射, 现场分配
  if(walkaddr(p->pagetable, *ip) == 0) {
    // 同样的合法范围判断: 栈顶之上, 且小于 p->sz
    if(PGROUNDUP(p->trapframe->sp) - 1 < *ip && *ip < p->sz) {
      char *pa = kalloc();
      if(pa == 0)
        return -1;
      memset(pa, 0, PGSIZE);            // 清零, 避免泄漏
      if(mappages(p->pagetable, PGROUNDDOWN(*ip), PGSIZE,
                  (uint64)pa, PTE_R | PTE_W | PTE_X | PTE_U) != 0) {
        kfree(pa);
        return -1;
      }
    } else {
      // 地址非法(越界或落在 guard page): 拒绝, 由调用方返回错误
      return -1;
    }
  }
  return 0;
}
```

逻辑和 `usertrap` 那段几乎一模一样。区别在于这里是**内核主动检查**而不是**硬件触发**，但补页的动作完全相同。

> **小提示**：这种实现只处理了"参数本身是 lazy 地址"的情况。如果用户传的是一个**指向 lazy 区域的指针**（比如 `struct stat *st` 落在 lazy 页里），那 `argaddr` 拿到的是 `st` 这个地址，能补上；但如果 lazy 区域跨多页，后续 `copyin/copyout` 还可能 fault。严格来说应该让 `copyin/copyout` 内部按页检查并补齐——本实现走的是"在参数处补一页"，对 grade 测试够用；若做更鲁棒的实现，应改 `walkaddr` 或 `copyin/copyout`。

---

## 整体调用关系

```text
用户调 sbrk(big_n)
   ↓
sys_sbrk: p->sz += big_n  (页表不动)
   ↓
用户访问新地址 → 硬件 page fault → uservec → usertrap
   ↓
usertrap 判 scause == 13 / 15
   ↓ 通过合法范围检查
kalloc + memset(0) + mappages
   ↓
sret 重新执行那条指令 → 成功

------------------------------------------------------------

用户调 read(fd, lazy_buf, n)
   ↓
sys_read → argaddr(lazy_buf)
   ↓
walkaddr 发现没映射, 范围合法
   ↓ kalloc + mappages 当场补
copyout 正常工作 → read 成功

------------------------------------------------------------

fork:
   uvmcopy 遍历 [0, sz)
   碰到 lazy 占位 → continue (子进程也维持 lazy)

exit:
   uvmunmap 遍历 [0, sz)
   碰到 lazy 占位 → continue (本来就没物理页可释放)
```

---

## 易错点速查

| 错误现象 | 原因 |
| --- | --- |
| `panic: uvmunmap: walk` | 忘了把 `uvmunmap` 里的 `panic` 改成 `continue` |
| `panic: uvmcopy: pte should exist` | 忘了改 `uvmcopy` |
| `usertrap(): unexpected scause 0xf` 然后进程被杀 | 忘了在 `usertrap` 加 scause==13/15 分支 |
| 用户 `echo hi | grep h` 之类命令直接 `read returned -1` | 忘了改 `argaddr`，导致用户传的 lazy 缓冲在内核侧读不出来 |
| 栈溢出测试本来应该 kill 进程，但你给它补页了 | 边界条件 `PGROUNDUP(sp) - 1 < va` 写反，把 guard page 也补成可用 |
| 物理内存很快耗尽 | 缩小 sbrk 时没有真正 free，或 mappages 失败时没 kfree |

---

## 核心思想总结

lazy allocation 的本质是：**把"声明地址"和"真正占用物理页"这两个动作解耦**。

- `sbrk` 只更新进程的逻辑地址空间大小 `p->sz`；
- 真正访问那段地址时才付物理页的代价；
- 所有遍历地址空间的代码（`uvmunmap`、`uvmcopy`、`copyin/copyout`、`argaddr`）都要容忍"声明了但还没分配"这种中间状态。

这套思路在 OS 中是通用模板。后面的 COW fork、demand paging、栈自动扩展、mmap，本质都是同一个模式：

```text
预先在页表里留下一种"会触发 page fault 的状态"
        ↓
程序访问时 page fault
        ↓
内核判定 fault 是否"合法 / 可修复"
        ↓ 是
做必要补齐 (分配 / 复制 / 读盘)
        ↓
sret 重新执行原指令, 一切对程序透明
```

所以 lazy lab 是把 page fault 从"灾难"转成"机制"的第一步。

