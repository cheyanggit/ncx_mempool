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

static ncx_uint_t  ncx_slab_max_size;//2048    slab的一次最大分配空间，默认为pagesize/2
/*对于64位与32位系统，nginx里面默认的值是不一样的，我们看到数字可能会更好理解一点，所以我们就以32位来看，用实际的数字来说话！
这个时依赖slab的分配算法.它的值是这样来的.4096/32，2048是slab页大小，而32是一个int的位数，最后的值是128。
why? 我们在分配时,在一页中，我们可以将这一页分成多个块,而某个块需要标记是否被分配,而一页空间正好被分成32个128字节大小的块，于是我们可以用一个int的32位表示这块的使用情况，
而此时,我们是使用ngx_slab_page_s结构体中的slab成员来表示块的使用情况的。另外，在分配大于128与小于128时，表示块的占用情况有所有同
*/
static ncx_uint_t  ncx_slab_exact_size;//64     slab精确分配大小，这个是一个分界点，通常是4096/32，为什么会这样，我们后面会有介绍
static ncx_uint_t  ncx_slab_exact_shift;//6     slab精确分配大小对应的移位数
static ncx_uint_t  ncx_pagesize; //4K        // 页大小
static ncx_uint_t  ncx_pagesize_shift;//12  // 页大小对应的移位数
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
        // 最大分配空间为页大小的一半
        ncx_slab_max_size = ncx_pagesize / 2;//2K
        // 精确分配大小，8为一个字节的位数，sizeof(uintptr_t)为一个uintptr_t的字节，我们后面会根据这个size来判断使用不同的分配算法 
        ncx_slab_exact_size = ncx_pagesize / (8 * sizeof(uintptr_t));//64  uintptr_t 类型的位图变量表示的页划分
        // 计算出此精确分配的移位数
        for (n = ncx_slab_exact_size; n >>= 1; ncx_slab_exact_shift++) {
            /* void */
        }
    }

    pool->min_size = 1 << pool->min_shift;//8byte

    // p 指向slot数组
    p = (u_char *) pool + sizeof(ncx_slab_pool_t);//sizeof:80

    // 某一个大小范围内的页，放到一起，具有相同的移位数
    slots = (ncx_slab_page_t *) p;//sizeof(page):24

    // 最大移位数，减去最小移位数，得到需要的slot数量  
    // 默认为8
    n = ncx_pagesize_shift - pool->min_shift;
    // 初始化各个slot
    for (i = 0; i < n; i++) {//9([0~8])
        slots[i].slab = 0;
        slots[i].next = &slots[i];
        slots[i].prev = 0;
    }

    // 指向页数组
    p += n * sizeof(ncx_slab_page_t);

    size = pool->end - p;//pages[] + cache

    // 将开始的size个字节设置为0
    ncx_slab_junk(p, size);

    // 计算出当前内存空间可以放下多少个页，此时的计算没有进行对齐，在后面会进行调整
    pages = (ncx_uint_t) (size / (ncx_pagesize + sizeof(ncx_slab_page_t)));

    ncx_memzero(p, pages * sizeof(ncx_slab_page_t));

    pool->pages = (ncx_slab_page_t *) p;

    pool->free.prev = 0;
    pool->free.next = (ncx_slab_page_t *) p;//可用的page
    //page数据第一个元素
    pool->pages->slab = pages;//994
    pool->pages->next = &pool->free;
    pool->pages->prev = (uintptr_t) &pool->free;

    // 计算出对齐后的返回内存的地址
    pool->start = (u_char *)
                  ncx_align_ptr((uintptr_t) p + pages * sizeof(ncx_slab_page_t),
                                 ncx_pagesize);

    // 说明之前是没有对齐过的，由于对齐之后，最后那一页，有可能不够一页，所以要去掉那一块
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

    // 如果超出slab最大可分配大小，即大于2048，则我们需要计算出需要的page数，  
    // 然后从空闲页中分配出连续的几个可用页
    if (size >= ncx_slab_max_size) {

		debug("slab alloc: %zu", size);

        // 计算需要的页数，然后分配指针页数
        page = ncx_slab_alloc_pages(pool, (size >> ncx_pagesize_shift)
                                          + ((size % ncx_pagesize) ? 1 : 0));
        if (page) {
            // 由返回page在页数组中的偏移量，计算出实际数组地址的偏移量
            p = (page - pool->pages) << ncx_pagesize_shift;
            // 计算出实际的数据地址
            p += (uintptr_t) pool->start;

        } else {
            p = 0;
        }

        goto done;
    }

    // 如果小于2048，则启用slab分配算法进行分配

     // 计算出此size的移位数以及此size对应的slot以及移位数
    if (size > pool->min_size) {
        shift = 1;
        // 计算移位数
        for (s = size - 1; s >>= 1; shift++) { /* void */ }
        // 由移位数得到slot
        slot = shift - pool->min_shift;

    } else {
         // 小于最小可分配大小的都放到一个slot里面
        size = pool->min_size;
        shift = pool->min_shift;
        // 因为小于最小分配的，所以就放在第一个slot里面 
        slot = 0;
    }

    slots = (ncx_slab_page_t *) ((u_char *) pool + sizeof(ncx_slab_pool_t));
    // 得到当前slot所占用的页
    page = slots[slot].next;

    // 找到一个可用空间
    if (page->next != page) {
        // 分配大小小于128字节时的算法，看不懂的童鞋可以先看等于128字节的情况  
        // 当分配空间小于128字节时，我们不可能用一个int来表示这些块的占用情况  
        // 此时，我们就需要几个int了，即一个bitmap数组  
        // 我们此时没有使用page->slab，而是使用页数据空间的开始几个int空间来表示了  
        // 看代码 
        if (shift < ncx_slab_exact_shift) {//小于精确分配

            do {
                // 得到页数据部分
                p = (page - pool->pages) << ncx_pagesize_shift;

                // 页的开始几个int大小的空间来存放位图数据
                bitmap = (uintptr_t *) (pool->start + p);
                
                //32=》2位 16=》4位  8=》8位
                // 当前页，在当前size下可分成map*32个块  
                // 我们需要map个int来表示这些块空间
                map = (1 << (ncx_pagesize_shift - shift))
                          / (sizeof(uintptr_t) * 8);//128/8*8=2  计算需要几个uintptr_t类型位图

                for (n = 0; n < map; n++) {

                    if (bitmap[n] != NCX_SLAB_BUSY) {

                        for (m = 1, i = 0; m; m <<= 1, i++) {
                            if ((bitmap[n] & m)) {
                                // 当前位表示的块已被使用了
                                continue;
                            }

                            // 设置已占用
                            bitmap[n] |= m;

                            i = ((n * sizeof(uintptr_t) * 8) << shift)
                                + (i << shift);

                            // 如果当前bitmap所表示的空间已都被占用，就查找下一个bitmap
                            if (bitmap[n] == NCX_SLAB_BUSY) {
                                for (n = n + 1; n < map; n++) {
                                    // 找到下一个还剩下空间的bitmap
                                     if (bitmap[n] != NCX_SLAB_BUSY) {
                                         p = (uintptr_t) bitmap + i;

                                         goto done;
                                     }
                                }
                                // 剩下所有的bitmap都被占用了，表明当前的页已完全被使用了，把当前页从链表中删除 
                                prev = (ncx_slab_page_t *)
                                            (page->prev & ~NCX_SLAB_PAGE_MASK);
                                prev->next = page->next;
                                page->next->prev = page->prev;

                                page->next = NULL;
                                // 小内存分配 
                                page->prev = NCX_SLAB_SMALL;
                            }

                            p = (uintptr_t) bitmap + i;

                            goto done;
                        }
                    }
                }

                page = page->next;

            } while (page);

        } else if (shift == ncx_slab_exact_shift) {//精确分配
                // 如果分配大小正好是128字节，则一页可以分成32个块，我们可以用一个int来表示这些个块的使用情况  
            // 这里我们使用page->slab来表示这些块的使用情况，当所有块被占用后，该值就变成了0xffffffff，即NGX_SLAB_BUSY  
            // 表示该块都被占用了

            do {
                // 当前页可用
                if (page->slab != NCX_SLAB_BUSY) {

                    for (m = 1, i = 0; m; m <<= 1, i++) {
                        // 如果当前位被使用了，就继续查找下一块
                        if ((page->slab & m)) {
                            continue;
                        }

                        // 设置当前为已被使用
                        page->slab |= m;
                        // 最后一块也被使用了，就表示此页已使用完
                        if (page->slab == NCX_SLAB_BUSY) {
                            // 将当前页从链表中移除
                            prev = (ncx_slab_page_t *)
                                            (page->prev & ~NCX_SLAB_PAGE_MASK);
                            prev->next = page->next;
                            page->next->prev = page->prev;

                            page->next = NULL;
                            // 标识使用类型，精确
                            page->prev = NCX_SLAB_EXACT;
                        }

                        p = (page - pool->pages) << ncx_pagesize_shift;
                        p += i << shift;
                        p += (uintptr_t) pool->start;

                        goto done;
                    }
                }
                // 查找下一页 
                page = page->next;

            } while (page);

        } else { /* shift > ncx_slab_exact_shift */
            // 当需要分配的空间大于128或64时，我们可以用一个int的位来表示这些空间  64位机器是64
            //所以我们依然采用跟等于128时类似的情况，用page->slab来表示  
            // 但由于 大于128的情况比较多，移位数分别为8、9、10、11这些情况  
            // 对于一个页，我们如何来知道这个页的分配大小呢？  
            // 而我们知道，最小我们只需要使用16位即可表示这些空间了，即分配大小为256~512时  
            // 那么我们采用高16位来表示这些空间的占用情况  
            // 而最低位，我们也利用起来，表示此页的分配大小，即保存移位数  
            // 比如我们分配256，当分配第一个空间时，此时的page->slab位图情况是：0x0001008  
            // 那分配下一空间就是0x0003008了，当为0xffff008时，就分配完了  
            // 看代码  


            // page->slab & NGX_SLAB_SHIFT_MASK 即得到最低一位的值，其实就是当前页的分配大小的移位数  
            // ngx_pagesize_shift减掉后，就是在一页中标记这些块所需要的移位数，也就是块数对应的移位数 
            n = ncx_pagesize_shift - (page->slab & NCX_SLAB_SHIFT_MASK);//已经占用移位->可移动位数
            // 得到一个页面所能放下的块数
            n = 1 << n;
            // 得到表示这些块数都用完的bitmap，用现在是低16位的
            n = ((uintptr_t) 1 << n) - 1;
            // 将低16位转换成高16位，因为我们是用高16位来表示空间地址的占用情况的
            mask = n << NCX_SLAB_MAP_SHIFT;//0xffffffff00000000
 
            do {//高32位表示占用情况：0x100000000 表示，占用一个
                // 判断高16位是否全被占用了
                if ((page->slab & NCX_SLAB_MAP_MASK) != mask) {//slab&0xffffffff00000000   != 0xffffffff

                    //m为位图表示内存使用情况
                    // NGX_SLAB_MAP_SHIFT 为移位偏移， 得到0x10000
                    for (m = (uintptr_t) 1 << NCX_SLAB_MAP_SHIFT, i = 0;//NCX_SLAB_MAP_SHIFT=32
                         m & mask;
                         m <<= 1, i++)
                    {   // 当前块是否被占用 
                        if ((page->slab & m)) {//判断当前位是否已经使用
                            continue;
                        }
                        // 将当前位设置成1
                        page->slab |= m;
                        // 当前页是否完全被占用完
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
    // 如果当前slab对应的page中没有空间可分配了，则重新从空闲page中分配一个页  
    page = ncx_slab_alloc_pages(pool, 1);

    if (page) {
        if (shift < ncx_slab_exact_shift) {
            // 精确分配，小于64时 
            p = (page - pool->pages) << ncx_pagesize_shift;//数据页对应的首地址
            bitmap = (uintptr_t *) (pool->start + p);//前8个字节
            // 需要的空间大小
            s = 1 << shift;//申请size大小
            n = (1 << (ncx_pagesize_shift - shift)) / 8 / s;

            if (n == 0) {
                n = 1;
            }

            bitmap[0] = (2 << n) - 1;//第一个字节为3 :0011
            // 需要使用的uintptr_t数组个数
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
