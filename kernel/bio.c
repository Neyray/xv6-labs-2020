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

struct hashbuf{
	struct buf head;
	struct spinlock lock;
};

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
  for(int i=0;i<NBUCKET;++i){
	 snprintf(lockname,sizeof(lockname),"bcache_bucket_%d",i);
	 initlock(&bcache.buckets[i].lock,lockname);
	 bcache.buckets[i].head.prev=&bcache.buckets[i].head;
	 bcache.buckets[i].head.next=&bcache.buckets[i].head;
}

  //初始全部挂到bucket[0]
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
  struct buf *b;
  int bid=HASH(blockno);

  //第一阶段：在自己桶里查找（查找锁吗？）
  acquire(&bcache.buckets[bid].lock);
  for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
	  if(b->dev==dev && b->blockno==blockno){  //这里的dev,blockno是什么？
		  b->refcnt++;
		  release(&bcache.buckets[bid].lock);
		  acquiresleep(&b->lock);
		  return b;
	  }
  }
  release(&bcache.buckets[bid].lock);



  //第二阶段：未命中，进入换出阶段（串行化）
  acquire(&bcache.eviction_lock);

  //必须再查一次！！！
  //从释放桶锁到拿到 eviction_lock 之间，别的 CPU 可能已经把这个块加进了 cache
  acquire(&bcache.buckets[bid].lock);
  for(b=bcache.buckets[bid].head.next;b!=&bcache.buckets[bid].head;b=b->next){
	  if(b->dev==dev && b->blockno==blockno){
		  b->refcnt++;
		  release(&bcache.buckets[bid].lock);
		  release(&bcache.eviction_lock);
		  acquiresleep(&b->lock);   //睡眠锁？
		  return b;
	  }
  }
  release(&bcache.buckets[bid].lock);



  //第三阶段：扫描所有桶，找 refcnt==0 且 timestamp 最小的
  struct buf *lru_b=0;
  int lru_bid=-1;

  for(int i=0;i<NBUCKET;++i){
	  acquire(&bcache.buckets[i].lock);
	  int found_new=0;
	  for(b=bcache.buckets[i].head.next;b!=&bcache.buckets[i].head;b=b->next){
		  if(b->refcnt==0 && (lru_b==0 || b->timestamp < lru_b->timestamp)){
			  //找到了更优的候选
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

   lru_b->dev=dev;
   lru_b->blockno=blockno;
   lru_b->valid=0;
   lru_b->refcnt=1;

   release(&bcache.buckets[bid].lock);
   release(&bcache.eviction_lock);
   acquiresleep(&lru_b->lock);
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

  releasesleep(&b->lock);

  int bid=HASH(b->blockno);   //只操作自己桶
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;

  if (b->refcnt == 0) {
    //由于使用了时间戳，所以不需要用头插法了
    acquire(&tickslock);
    b->timestamp=ticks;   //释放时更新时间戳?这些参数哪来的？
    release(&tickslock);
  }
  
  release(&bcache.buckets[bid].lock);
}

void
bpin(struct buf *b) {
  int bid=HASH(b->blockno);//HASH是什么函数？
  acquire(&bcache.buckets[bid].lock);
  b->refcnt++;   //这是什么参数？
  release(&bcache.buckets[bid].lock);
}

void
bunpin(struct buf *b) {
  int bid=HASH(b->blockno);
  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  release(&bcache.buckets[bid].lock);
}


