#ifndef _NCX_SLAB_H_INCLUDED_
#define _NCX_SLAB_H_INCLUDED_


#include "ncx_core.h"
#include "ncx_lock.h"
#include "ncx_log.h"

typedef struct ncx_slab_page_s  ncx_slab_page_t;

// 页结构体
struct ncx_slab_page_s {
    uintptr_t         slab;//多种情况，多个用途（1.分配新页时：剩余页数量 2.分配obj内存时：一对多，表示分配obj的占用情况(是否使用)，以比特位表示）
    ncx_slab_page_t  *next;//分配较小slob时，next指向slab page在pool->pages的位置
    uintptr_t         prev;//上一个
};


typedef struct {
    size_t            min_size;//最小分配单元
    size_t            min_shift;//最小分配单元，对应位移 3

    ncx_slab_page_t  *pages; //页数组
    ncx_slab_page_t   free; //空闲页链表

    u_char           *start; //可分配空间的起始地址
    u_char           *end; //内存块的结束地址

	ncx_shmtx_t		 mutex;

    void             *addr; //指向ncx_slab_pool_t开头
} ncx_slab_pool_t;

typedef struct {
	size_t 			pool_size, used_size, used_pct; 
	size_t			pages, free_page;
	size_t			p_small, p_exact, p_big, p_page; /* 四种slab占用的page数 */
	size_t			b_small, b_exact, b_big, b_page; /* 四种slab占用的byte数 */
	size_t			max_free_pages;					 /* 最大的连续可用page数 */
} ncx_slab_stat_t;

void ncx_slab_init(ncx_slab_pool_t *pool);
void *ncx_slab_alloc(ncx_slab_pool_t *pool, size_t size);
void *ncx_slab_alloc_locked(ncx_slab_pool_t *pool, size_t size);
void ncx_slab_free(ncx_slab_pool_t *pool, void *p);
void ncx_slab_free_locked(ncx_slab_pool_t *pool, void *p);

void ncx_slab_dummy_init(ncx_slab_pool_t *pool);
void ncx_slab_stat(ncx_slab_pool_t *pool, ncx_slab_stat_t *stat);

#endif /* _NCX_SLAB_H_INCLUDED_ */
