// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define HASH(blockno)((blockno)%NBUCKET)

//hash桶是存储buf的，buf总共有三十个（里面包含data数组，睡眠锁---访问数据 等等）
struct hashbuf{
	struct buf head;
	struct spinlock lock;
};


//bcache是整个缓存系统，eviction_lock是整个大锁，控制所有hash桶
//保证同一时刻只有一个 CPU 在做"找 LRU 并换出"的操作
struct {
  struct spinlock eviction_lock;   //仅用于换出时串行化
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct hashbuf buckets[NBUCKET];   //散列桶
} bcache;

//初始化
void
binit(void)
{
  struct buf *b;
  char lockname[16];

  initlock(&bcache.eviction_lock, "bcache_eviction");

  // Create linked list of buffers
  // 对每一个hash桶进行操作，创造NBUCKET个空桶
  for(int i=0;i<NBUCKET;++i){
	 snprintf(lockname,sizeof(lockname),"bcache_bucket_%d",i);
	 initlock(&bcache.buckets[i].lock,lockname);
	 bcache.buckets[i].head.prev=&bcache.buckets[i].head;
	 bcache.buckets[i].head.next=&bcache.buckets[i].head;
}

  //初始全部挂到bucket[0]
  //仅仅是为了图方便，把所有数据放到bucket[0]
  for(b=bcache.buf;b<bcache.buf+NBUF;b++){
	  //采用头插法
	  b->next=bcache.buckets[0].head.next;
	  b->prev=&bcache.buckets[0].head;
	  b->timestamp=0;
	  initsleeplock(&b->lock,"buffer");
	  bcache.buckets[0].head.next->prev=b;
	  bcache.buckets[0].head.next=b;
  }
}





// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;//数据块
  int bid=HASH(blockno);//hash算法根据blockno（磁盘号）得到的hash桶号

  //第一部分：在自己桶里查找
  //查找满足条件的buf数据结构的b
  acquire(&bcache.buckets[bid].lock);//打开桶锁，允许访问桶的数据结构
  //C语言特有的指针遍历方式，从buckets[bid]的第一个buf遍历到最后一个
  for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
	  if(b->dev==dev && b->blockno==blockno){  //dev是设备号，blockno是磁盘号
		  b->refcnt++;//refcnt是使用buf的数量，这里说明它要被调走了
		  release(&bcache.buckets[bid].lock);
		  acquiresleep(&b->lock);//打开b的睡眠锁，允许访问数据块(read操作等)
		  return b;
	  }
  }
  release(&bcache.buckets[bid].lock);//其余情况也要归还锁



  //第二部分：未命中，进入换出阶段（串行化）
  //
  //第一阶段：再次在自己这个桶里面找（可能是换出）
  acquire(&bcache.eviction_lock);//新增，允许访问整个缓存系统

  //必须再查一次！！！
  //从释放桶锁到拿到 eviction_lock 之间，别的 CPU 可能已经把这个块加进了 cache
  acquire(&bcache.buckets[bid].lock);
  for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
	  if(b->dev==dev && b->blockno==blockno){
		  b->refcnt++;
		  release(&bcache.buckets[bid].lock);
		  release(&bcache.eviction_lock);
		  acquiresleep(&b->lock);   //睡眠锁
		  return b;
	  }
  }
  release(&bcache.buckets[bid].lock);



  //第二阶段：扫描所有桶，找 refcnt==0 且 timestamp 最小的
  struct buf *lru_b=0;//指向"目前找到的最佳候选 buf"的指针
  int lru_bid=-1;//这个候选 buf当前所在桶的编号

  //遍历所有桶
  for(int i=0;i<NBUCKET;++i){
	  acquire(&bcache.buckets[i].lock);
	  int found_new=0;// 本桶有没有产生更优候选
	  //遍历当前这个桶的每一个buf
	  for(b=bcache.buckets[i].head.next;b!=&bcache.buckets[i].head;b=b->next){
		  if(b->refcnt==0 && (lru_b==0 || b->timestamp < lru_b->timestamp)){
			  //找到了更优的候选
			  if(lru_bid!=-1 && lru_bid!=i){
				  // 如果之前的候选在别的桶，释放那个桶的锁
				  release(&bcache.buckets[lru_bid].lock);
			  }
			  //更新
			  lru_b=b;
			  lru_bid=i;
			  found_new=1;
		  }
	  }
	  // 本桶没有产生更优候选 → 可以松开本桶的锁
	  // 注意：如果本桶就是当前候选所在桶（i == lru_bid），不能释放
	  if(!found_new && i!=lru_bid){
		  release(&bcache.buckets[i].lock);
	  }
  }

  if(lru_b==0){
	  panic("bget:no buffers");
  }

   // 现在还持有 lru_bid 那个桶的锁
   // 若 LRU buf 在别的桶，需要搬到 bid 桶
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

   //更新数据
   lru_b->dev=dev;
   lru_b->blockno=blockno;
   lru_b->valid=0;
   lru_b->refcnt=1;

   release(&bcache.buckets[bid].lock);
   release(&bcache.eviction_lock);
   acquiresleep(&lru_b->lock);//允许访问数据块
   return lru_b;
}









// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock); // 先把 buf 的睡眠锁释放（别人可以开始用 data 了）


  int bid=HASH(b->blockno);   //只操作自己桶
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;

  if (b->refcnt == 0) {
    //由于使用了时间戳，所以不需要用头插法了

    //tickslock 是 xv6 全局时钟计数的锁（定义在 kernel/trap.c）。ticks 是一个全局 uint，每个时钟中断时 ticks++。因为多 CPU 可能同时读写 ticks，所以配了一把 tickslock 保护它。
    //用时间戳 = 最近一次 refcnt 归零的时刻来判定 LRU，此刻refcnt=0了，所以需要更新timestamp
    acquire(&tickslock);
    b->timestamp=ticks;   
    release(&tickslock);
  }
  
  release(&bcache.buckets[bid].lock);
}

void
bpin(struct buf *b) {
  int bid=HASH(b->blockno);//HASH是开头自定义的
  acquire(&bcache.buckets[bid].lock);
  b->refcnt++;   //增加使用buf的个数
  release(&bcache.buckets[bid].lock);
}

void
bunpin(struct buf *b) {
  int bid=HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  release(&bcache.buckets[bid].lock);
}


