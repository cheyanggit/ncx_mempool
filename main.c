#include "ncx_slab.h"

int main(int argc, char **argv)
{
	char *p;
	size_t 	pool_size = 4096000;  //4M 
	ncx_slab_stat_t stat;
	u_char 	*space;
	space = (u_char *)malloc(pool_size);

	ncx_slab_pool_t *sp;
	sp = (ncx_slab_pool_t*) space;

	sp->addr = space;
	sp->min_shift = 3;
	sp->end = space + pool_size;

	ncx_slab_init(sp);

	int i;
	int count = 65;//1000000;
	for (i = 0; i < count; i++) 
	{   
		//p = ncx_slab_alloc(sp, 128 + i); 
		//p = ncx_slab_alloc(sp, 64 ); // 64字节
		//p = ncx_slab_alloc(sp, 128 ); //128
		p = ncx_slab_alloc(sp, 32 ); 

		if (p == NULL) 
		{   
			printf("%d\n", i); 
			return -1; 
		}   
		//ncx_slab_free(sp, p); 
	}   
	ncx_slab_stat(sp, &stat);

	printf("##########################################################################\n");
	for (i = 0; i < 2500; i++) 
	{   
		p = ncx_slab_alloc(sp, 30 + i); 
		if (p == NULL) 
		{   
			printf("%d\n", i); 
			return -1; 
		}   
		
		if (i % 3 == 0) 
		{
			ncx_slab_free(sp, p);
		}
	}   
	ncx_slab_stat(sp, &stat);

	free(space);

	return 0;
}
