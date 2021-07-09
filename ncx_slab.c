#include "ncx_slab.h"
#include <unistd.h>

#define NCX_SLAB_PAGE_MASK   3
#define NCX_SLAB_PAGE        0
#define NCX_SLAB_BIG         1
#define NCX_SLAB_EXACT       2
#define NCX_SLAB_SMALL       3

#if (NCX_PTR_SIZE == 4)

#define NCX_SLAB_PAGE_FREE   0
#define NCX_SLAB_PAGE_BUSY   0xffffffff
#define NCX_SLAB_PAGE_START  0x80000000

#define NCX_SLAB_SHIFT_MASK  0x0000000f
#define NCX_SLAB_MAP_MASK    0xffff0000
#define NCX_SLAB_MAP_SHIFT   16

#define NCX_SLAB_BUSY        0xffffffff

#else /* (NCX_PTR_SIZE == 8) */

#define NCX_SLAB_PAGE_FREE   0
#define NCX_SLAB_PAGE_BUSY   0xffffffffffffffff
#define NCX_SLAB_PAGE_START  0x8000000000000000

#define NCX_SLAB_SHIFT_MASK  0x000000000000000f
#define NCX_SLAB_MAP_MASK    0xffffffff00000000
#define NCX_SLAB_MAP_SHIFT   32

#define NCX_SLAB_BUSY        0xffffffffffffffff

#endif


#if (NCX_DEBUG_MALLOC)

#define ncx_slab_junk(p, size)     ncx_memset(p, 0xA5, size)

#else

#define ncx_slab_junk(p, size)

#endif


static ncx_slab_page_t *ncx_slab_alloc_pages(ncx_slab_pool_t *pool,
    ncx_uint_t pages);
static void ncx_slab_free_pages(ncx_slab_pool_t *pool, ncx_slab_page_t *page,
    ncx_uint_t pages);
static bool ncx_slab_empty(ncx_slab_pool_t *pool, ncx_slab_page_t *page);

static ncx_uint_t  ncx_slab_max_size;//2048
static ncx_uint_t  ncx_slab_exact_size;//64
static ncx_uint_t  ncx_slab_exact_shift;//6
static ncx_uint_t  ncx_pagesize; //4K
static ncx_uint_t  ncx_pagesize_shift;//12
static ncx_uint_t  ncx_real_pages;  // 对齐后，在计算page的个数

void
ncx_slab_init(ncx_slab_pool_t *pool)
{
    u_char           *p;
    size_t            size;
    ncx_uint_t        i, n, pages;
    ncx_slab_page_t  *slots;

	/*pagesize*/
	ncx_pagesize = getpagesize();//4K
	for (n = ncx_pagesize, ncx_pagesize_shift = 0; 
			n >>= 1; ncx_pagesize_shift++) { /* void */ }

    /* STUB */
    if (ncx_slab_max_size == 0) {
        ncx_slab_max_size = ncx_pagesize / 2;//2K
        ncx_slab_exact_size = ncx_pagesize / (8 * sizeof(uintptr_t));//64  uintptr_t 类型的位图变量表示的页划分
        for (n = ncx_slab_exact_size; n >>= 1; ncx_slab_exact_shift++) {
            /* void */
        }
    }

    pool->min_size = 1 << pool->min_shift;//8byte

    p = (u_char *) pool + sizeof(ncx_slab_pool_t);//sizeof:80
    slots = (ncx_slab_page_t *) p;//sizeof(page):24

    n = ncx_pagesize_shift - pool->min_shift;
    for (i = 0; i < n; i++) {//9([0~8])
        slots[i].slab = 0;
        slots[i].next = &slots[i];
        slots[i].prev = 0;
    }

    p += n * sizeof(ncx_slab_page_t);

    size = pool->end - p;//pages[] + cache
    ncx_slab_junk(p, size);

    pages = (ncx_uint_t) (size / (ncx_pagesize + sizeof(ncx_slab_page_t)));

    ncx_memzero(p, pages * sizeof(ncx_slab_page_t));

    pool->pages = (ncx_slab_page_t *) p;

    pool->free.prev = 0;
    pool->free.next = (ncx_slab_page_t *) p;//可用的page
    //page数据第一个元素
    pool->pages->slab = pages;//994
    pool->pages->next = &pool->free;
    pool->pages->prev = (uintptr_t) &pool->free;

    pool->start = (u_char *)
                  ncx_align_ptr((uintptr_t) p + pages * sizeof(ncx_slab_page_t),
                                 ncx_pagesize);

	ncx_real_pages = (pool->end - pool->start) / ncx_pagesize;//994 地址对齐后还是994：可能会少一
	pool->pages->slab = ncx_real_pages;
}


void *
ncx_slab_alloc(ncx_slab_pool_t *pool, size_t size)
{
    void  *p;

    ncx_shmtx_lock(&pool->mutex);

    p = ncx_slab_alloc_locked(pool, size);

    ncx_shmtx_unlock(&pool->mutex);

    return p;
}


void *
ncx_slab_alloc_locked(ncx_slab_pool_t *pool, size_t size)
{
    size_t            s;
    uintptr_t         p, n, m, mask, *bitmap;
    ncx_uint_t        i, slot, shift, map;
    ncx_slab_page_t  *page, *prev, *slots;

    if (size >= ncx_slab_max_size) {

		debug("slab alloc: %zu", size);

        page = ncx_slab_alloc_pages(pool, (size >> ncx_pagesize_shift)
                                          + ((size % ncx_pagesize) ? 1 : 0));
        if (page) {
            p = (page - pool->pages) << ncx_pagesize_shift;
            p += (uintptr_t) pool->start;

        } else {
            p = 0;
        }

        goto done;
    }

    if (size > pool->min_size) {
        shift = 1;
        for (s = size - 1; s >>= 1; shift++) { /* void */ }
        slot = shift - pool->min_shift;

    } else {
        size = pool->min_size;
        shift = pool->min_shift;
        slot = 0;
    }

    slots = (ncx_slab_page_t *) ((u_char *) pool + sizeof(ncx_slab_pool_t));
    page = slots[slot].next;

    if (page->next != page) {

        if (shift < ncx_slab_exact_shift) {

            do {
                p = (page - pool->pages) << ncx_pagesize_shift;
                bitmap = (uintptr_t *) (pool->start + p);

                map = (1 << (ncx_pagesize_shift - shift))
                          / (sizeof(uintptr_t) * 8);//128/8*8=2  计算需要几个uintptr_t类型位图

                for (n = 0; n < map; n++) {

                    if (bitmap[n] != NCX_SLAB_BUSY) {

                        for (m = 1, i = 0; m; m <<= 1, i++) {
                            if ((bitmap[n] & m)) {
                                continue;
                            }

                            bitmap[n] |= m;

                            i = ((n * sizeof(uintptr_t) * 8) << shift)
                                + (i << shift);

                            if (bitmap[n] == NCX_SLAB_BUSY) {
                                for (n = n + 1; n < map; n++) {
                                     if (bitmap[n] != NCX_SLAB_BUSY) {
                                         p = (uintptr_t) bitmap + i;

                                         goto done;
                                     }
                                }

                                prev = (ncx_slab_page_t *)
                                            (page->prev & ~NCX_SLAB_PAGE_MASK);
                                prev->next = page->next;
                                page->next->prev = page->prev;

                                page->next = NULL;
                                page->prev = NCX_SLAB_SMALL;
                            }

                            p = (uintptr_t) bitmap + i;

                            goto done;
                        }
                    }
                }

                page = page->next;

            } while (page);

        } else if (shift == ncx_slab_exact_shift) {

            do {
                if (page->slab != NCX_SLAB_BUSY) {

                    for (m = 1, i = 0; m; m <<= 1, i++) {
                        if ((page->slab & m)) {
                            continue;
                        }

                        page->slab |= m;
  
                        if (page->slab == NCX_SLAB_BUSY) {
                            prev = (ncx_slab_page_t *)
                                            (page->prev & ~NCX_SLAB_PAGE_MASK);
                            prev->next = page->next;
                            page->next->prev = page->prev;

                            page->next = NULL;
                            page->prev = NCX_SLAB_EXACT;
                        }

                        p = (page - pool->pages) << ncx_pagesize_shift;
                        p += i << shift;
                        p += (uintptr_t) pool->start;

                        goto done;
                    }
                }

                page = page->next;

            } while (page);

        } else { /* shift > ncx_slab_exact_shift */

            n = ncx_pagesize_shift - (page->slab & NCX_SLAB_SHIFT_MASK);//已经占用移位->可移动位数
            n = 1 << n;
            n = ((uintptr_t) 1 << n) - 1;
            mask = n << NCX_SLAB_MAP_SHIFT;//0xffffffff00000000
 
            do {//高32位表示占用情况：0x100000000 表示，占用一个
                if ((page->slab & NCX_SLAB_MAP_MASK) != mask) {//slab&0xffffffff00000000   != 0xffffffff

                    //m为位图表示内存使用情况
                    for (m = (uintptr_t) 1 << NCX_SLAB_MAP_SHIFT, i = 0;//NCX_SLAB_MAP_SHIFT=32
                         m & mask;
                         m <<= 1, i++)
                    {
                        if ((page->slab & m)) {//判断当前位是否已经使用
                            continue;
                        }

                        page->slab |= m;

                        if ((page->slab & NCX_SLAB_MAP_MASK) == mask) {
                            prev = (ncx_slab_page_t *)
                                            (page->prev & ~NCX_SLAB_PAGE_MASK);
                            prev->next = page->next;
                            page->next->prev = page->prev;

                            page->next = NULL;
                            page->prev = NCX_SLAB_BIG;
                        }

                        p = (page - pool->pages) << ncx_pagesize_shift;
                        p += i << shift;
                        p += (uintptr_t) pool->start;

                        goto done;
                    }
                }

                page = page->next;

            } while (page);
        }
    }

    page = ncx_slab_alloc_pages(pool, 1);

    if (page) {
        if (shift < ncx_slab_exact_shift) {
            // 小于128时 
            p = (page - pool->pages) << ncx_pagesize_shift;//数据页对应的首地址
            bitmap = (uintptr_t *) (pool->start + p);//前8个字节

            s = 1 << shift;//申请size大小
            n = (1 << (ncx_pagesize_shift - shift)) / 8 / s;

            if (n == 0) {
                n = 1;
            }

            bitmap[0] = (2 << n) - 1;//第一个字节为3 :0011

            map = (1 << (ncx_pagesize_shift - shift)) / (sizeof(uintptr_t) * 8);//计算申请的size，占用内存块数、1<<7  128/64=2块

            for (i = 1; i < map; i++) {
                bitmap[i] = 0;
            }

            page->slab = shift;
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_SMALL;

            slots[slot].next = page;

            p = ((page - pool->pages) << ncx_pagesize_shift) + s * n;//偏移s*n=32*1=32字节
            p += (uintptr_t) pool->start;//p=p+start=32+startH

            goto done;

        } else if (shift == ncx_slab_exact_shift) {
            //  slab位图表示64块内存使用情况
            page->slab = 1;//第一块空间被占用
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_EXACT;

            slots[slot].next = page;

            p = (page - pool->pages) << ncx_pagesize_shift;
            p += (uintptr_t) pool->start;

            goto done;

        } else { /* shift > ncx_slab_exact_shift */
            // 低位表示存放数据的大小
            page->slab = ((uintptr_t) 1 << NCX_SLAB_MAP_SHIFT) | shift;//NCX_SLAB_MAP_SHIFT=32
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_BIG;

            slots[slot].next = page;

            p = (page - pool->pages) << ncx_pagesize_shift;
            p += (uintptr_t) pool->start;

            goto done;
        }
    }

    p = 0;

done:

    debug("slab alloc: %p", (void *)p);

    return (void *) p;
}


void
ncx_slab_free(ncx_slab_pool_t *pool, void *p)
{
    ncx_shmtx_lock(&pool->mutex);

    ncx_slab_free_locked(pool, p);

    ncx_shmtx_unlock(&pool->mutex);
}


void
ncx_slab_free_locked(ncx_slab_pool_t *pool, void *p)
{
    size_t            size;
    uintptr_t         slab, m, *bitmap;
    ncx_uint_t        n, type, slot, shift, map;
    ncx_slab_page_t  *slots, *page;

    debug("slab free: %p", p);

    if ((u_char *) p < pool->start || (u_char *) p > pool->end) {
        error("ncx_slab_free(): outside of pool");
        goto fail;
    }

    n = ((u_char *) p - pool->start) >> ncx_pagesize_shift;//size找下page表下表
    page = &pool->pages[n];
    slab = page->slab;
    type = page->prev & NCX_SLAB_PAGE_MASK;

    switch (type) {

    case NCX_SLAB_SMALL:

        shift = slab & NCX_SLAB_SHIFT_MASK;//0x000000000000000f
        size = 1 << shift;//计算出这块内存，申请时使用多大size

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        n = ((uintptr_t) p & (ncx_pagesize - 1)) >> shift;
        m = (uintptr_t) 1 << (n & (sizeof(uintptr_t) * 8 - 1));
        n /= (sizeof(uintptr_t) * 8);
        bitmap = (uintptr_t *) ((uintptr_t) p & ~(ncx_pagesize - 1));

        if (bitmap[n] & m) {

            if (page->next == NULL) {
                slots = (ncx_slab_page_t *)
                                   ((u_char *) pool + sizeof(ncx_slab_pool_t));
                slot = shift - pool->min_shift;

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_SMALL;
                page->next->prev = (uintptr_t) page | NCX_SLAB_SMALL;
            }

            bitmap[n] &= ~m;

            n = (1 << (ncx_pagesize_shift - shift)) / 8 / (1 << shift);

            if (n == 0) {
                n = 1;
            }

            if (bitmap[0] & ~(((uintptr_t) 1 << n) - 1)) {
                goto done;
            }

            map = (1 << (ncx_pagesize_shift - shift)) / (sizeof(uintptr_t) * 8);

            for (n = 1; n < map; n++) {
                if (bitmap[n]) {
                    goto done;
                }
            }

            ncx_slab_free_pages(pool, page, 1);

            goto done;
        }

        goto chunk_already_free;

    case NCX_SLAB_EXACT:

        m = (uintptr_t) 1 <<
                (((uintptr_t) p & (ncx_pagesize - 1)) >> ncx_slab_exact_shift);
        size = ncx_slab_exact_size;

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        if (slab & m) {
            if (slab == NCX_SLAB_BUSY) {
                slots = (ncx_slab_page_t *)
                                   ((u_char *) pool + sizeof(ncx_slab_pool_t));
                slot = ncx_slab_exact_shift - pool->min_shift;

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_EXACT;
                page->next->prev = (uintptr_t) page | NCX_SLAB_EXACT;
            }

            page->slab &= ~m;

            if (page->slab) {
                goto done;
            }

            ncx_slab_free_pages(pool, page, 1);

            goto done;
        }

        goto chunk_already_free;

    case NCX_SLAB_BIG:

        shift = slab & NCX_SLAB_SHIFT_MASK;
        size = 1 << shift;

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        m = (uintptr_t) 1 << ((((uintptr_t) p & (ncx_pagesize - 1)) >> shift)
                              + NCX_SLAB_MAP_SHIFT);

        if (slab & m) {

            if (page->next == NULL) {
                slots = (ncx_slab_page_t *)
                                   ((u_char *) pool + sizeof(ncx_slab_pool_t));
                slot = shift - pool->min_shift;

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NCX_SLAB_BIG;
                page->next->prev = (uintptr_t) page | NCX_SLAB_BIG;
            }

            page->slab &= ~m;

            if (page->slab & NCX_SLAB_MAP_MASK) {
                goto done;
            }

            ncx_slab_free_pages(pool, page, 1);

            goto done;
        }

        goto chunk_already_free;

    case NCX_SLAB_PAGE:

        if ((uintptr_t) p & (ncx_pagesize - 1)) {
            goto wrong_chunk;
        }

		if (slab == NCX_SLAB_PAGE_FREE) {
			alert("ncx_slab_free(): page is already free");
			goto fail;
        }

		if (slab == NCX_SLAB_PAGE_BUSY) {
			alert("ncx_slab_free(): pointer to wrong page");
			goto fail;
        }

        n = ((u_char *) p - pool->start) >> ncx_pagesize_shift;
        size = slab & ~NCX_SLAB_PAGE_START;

        ncx_slab_free_pages(pool, &pool->pages[n], size);

        ncx_slab_junk(p, size << ncx_pagesize_shift);

        return;
    }

    /* not reached */

    return;

done:

    ncx_slab_junk(p, size);

    return;

wrong_chunk:

	error("ncx_slab_free(): pointer to wrong chunk");

    goto fail;

chunk_already_free:

	error("ncx_slab_free(): chunk is already free");

fail:

    return;
}


static ncx_slab_page_t *
ncx_slab_alloc_pages(ncx_slab_pool_t *pool, ncx_uint_t pages)
{
    ncx_slab_page_t  *page, *p;
	
    for (page = pool->free.next; page != &pool->free; page = page->next) {
	
        if (page->slab >= pages) {

            if (page->slab > pages) {//从第二个开始
                page[pages].slab = page->slab - pages;
                page[pages].next = page->next;
                page[pages].prev = page->prev;

                p = (ncx_slab_page_t *) page->prev;
                p->next = &page[pages];
                page->next->prev = (uintptr_t) &page[pages];

            } else {
                p = (ncx_slab_page_t *) page->prev;
                p->next = page->next;
                page->next->prev = page->prev;
            }

            page->slab = pages | NCX_SLAB_PAGE_START;//0x8000000000000000
            page->next = NULL;
            page->prev = NCX_SLAB_PAGE;//0

            if (--pages == 0) {
                return page;
            }

            for (p = page + 1; pages; pages--) {
                p->slab = NCX_SLAB_PAGE_BUSY;//0xffffffffffffffff
                p->next = NULL;
                p->prev = NCX_SLAB_PAGE;//0
                p++;
            }

            return page;
        }
	}

    error("ncx_slab_alloc() failed: no memory");

    return NULL;
}

static void
ncx_slab_free_pages(ncx_slab_pool_t *pool, ncx_slab_page_t *page,
    ncx_uint_t pages)
{
    ncx_slab_page_t  *prev, *next;

	if (pages > 1) {
		ncx_memzero(&page[1], (pages - 1)* sizeof(ncx_slab_page_t));
	}  

    if (page->next) {
        prev = (ncx_slab_page_t *) (page->prev & ~NCX_SLAB_PAGE_MASK);
        prev->next = page->next;
        page->next->prev = page->prev;
    }

	page->slab = pages;
	page->prev = (uintptr_t) &pool->free;  
	page->next = pool->free.next;
	page->next->prev = (uintptr_t) page;

	pool->free.next = page;

#ifdef PAGE_MERGE
	if (pool->pages != page) {
		prev = page - 1;
		if (ncx_slab_empty(pool, prev)) {
			for (; prev >= pool->pages; prev--) {
				if (prev->slab != 0) 
				{
					pool->free.next = page->next;
					page->next->prev = (uintptr_t) &pool->free;

					prev->slab += pages;
					ncx_memzero(page, sizeof(ncx_slab_page_t));

					page = prev;

					break;
				}
			}
		}
	}

	if ((page - pool->pages + page->slab) < ncx_real_pages) {
		next = page + page->slab;
		if (ncx_slab_empty(pool, next)) 
		{
			prev = (ncx_slab_page_t *) (next->prev);
			prev->next = next->next;
			next->next->prev = next->prev;

			page->slab += next->slab;
			ncx_memzero(next, sizeof(ncx_slab_page_t));
		}	
	}

#endif
}

void
ncx_slab_dummy_init(ncx_slab_pool_t *pool)
{
    ncx_uint_t n;

	ncx_pagesize = getpagesize();
	for (n = ncx_pagesize, ncx_pagesize_shift = 0; 
			n >>= 1; ncx_pagesize_shift++) { /* void */ }

    if (ncx_slab_max_size == 0) {
        ncx_slab_max_size = ncx_pagesize / 2;
        ncx_slab_exact_size = ncx_pagesize / (8 * sizeof(uintptr_t));
        for (n = ncx_slab_exact_size; n >>= 1; ncx_slab_exact_shift++) {
            /* void */
        }
    }
}

void
ncx_slab_stat(ncx_slab_pool_t *pool, ncx_slab_stat_t *stat)
{
	uintptr_t 			m, n, mask, slab;
	uintptr_t 			*bitmap;
	ncx_uint_t 			i, j, map, type, obj_size;
	ncx_slab_page_t 	*page;

	ncx_memzero(stat, sizeof(ncx_slab_stat_t));

	page = pool->pages;
 	stat->pages = (pool->end - pool->start) / ncx_pagesize;;

	for (i = 0; i < stat->pages; i++)
	{
		slab = page->slab;
		type = page->prev & NCX_SLAB_PAGE_MASK;

		switch (type) {

			case NCX_SLAB_SMALL:
	
				n = (page - pool->pages) << ncx_pagesize_shift;
                bitmap = (uintptr_t *) (pool->start + n);

				obj_size = 1 << slab;
                map = (1 << (ncx_pagesize_shift - slab))
                          / (sizeof(uintptr_t) * 8);

				for (j = 0; j < map; j++) {
					for (m = 1 ; m; m <<= 1) {
						if ((bitmap[j] & m)) {
							stat->used_size += obj_size;
							stat->b_small   += obj_size;
						}

					}		
				}
	
				stat->p_small++;

				break;

			case NCX_SLAB_EXACT:

				if (slab == NCX_SLAB_BUSY) {
					stat->used_size += sizeof(uintptr_t) * 8 * ncx_slab_exact_size;
					stat->b_exact   += sizeof(uintptr_t) * 8 * ncx_slab_exact_size;
				}
				else {
					for (m = 1; m; m <<= 1) {
						if (slab & m) {
							stat->used_size += ncx_slab_exact_size;
							stat->b_exact    += ncx_slab_exact_size;
						}
					}
				}

				stat->p_exact++;

				break;

			case NCX_SLAB_BIG:

				j = ncx_pagesize_shift - (slab & NCX_SLAB_SHIFT_MASK);
				j = 1 << j;
				j = ((uintptr_t) 1 << j) - 1;
				mask = j << NCX_SLAB_MAP_SHIFT;
				obj_size = 1 << (slab & NCX_SLAB_SHIFT_MASK);

				for (m = (uintptr_t) 1 << NCX_SLAB_MAP_SHIFT; m & mask; m <<= 1)
				{
					if ((page->slab & m)) {
						stat->used_size += obj_size;
						stat->b_big     += obj_size;
					}
				}

				stat->p_big++;

				break;

			case NCX_SLAB_PAGE:

				if (page->prev == NCX_SLAB_PAGE) {		
					slab 			=  slab & ~NCX_SLAB_PAGE_START;
					stat->used_size += slab * ncx_pagesize;
					stat->b_page    += slab * ncx_pagesize;
					stat->p_page    += slab;

					i += (slab - 1);

					break;
				}

			default:

				if (slab  > stat->max_free_pages) {
					stat->max_free_pages = page->slab;
				}

				stat->free_page += slab;

				i += (slab - 1);

				break;
		}

		page = pool->pages + i + 1;
	}

	stat->pool_size = pool->end - pool->start;
	stat->used_pct = stat->used_size * 100 / stat->pool_size;

	info("pool_size : %zu bytes",	stat->pool_size);
	info("used_size : %zu bytes",	stat->used_size);
	info("used_pct  : %zu%%\n",		stat->used_pct);

	info("total page count : %zu",	stat->pages);
	info("free page count  : %zu\n",	stat->free_page);
		
	info("small slab use page : %zu,\tbytes : %zu",	stat->p_small, stat->b_small);	
	info("exact slab use page : %zu,\tbytes : %zu",	stat->p_exact, stat->b_exact);
	info("big   slab use page : %zu,\tbytes : %zu",	stat->p_big,   stat->b_big);	
	info("page slab use page  : %zu,\tbytes : %zu\n",	stat->p_page,  stat->b_page);				

	info("max free pages : %zu\n",		stat->max_free_pages);
}

static bool 
ncx_slab_empty(ncx_slab_pool_t *pool, ncx_slab_page_t *page)
{
	ncx_slab_page_t *prev;
	
	if (page->slab == 0) {
		return true;
	}

	//page->prev == PAGE | SMALL | EXACT | BIG
	if (page->next == NULL ) {
		return false;
	}

	prev = (ncx_slab_page_t *)(page->prev & ~NCX_SLAB_PAGE_MASK);   
	while (prev >= pool->pages) { 
		prev = (ncx_slab_page_t *)(prev->prev & ~NCX_SLAB_PAGE_MASK);   
	};

	if (prev == &pool->free) {
		return true;
	}

	return false;
}
