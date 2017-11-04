

#include <stdlib.h>
#include <unistd.h>

#include "xmalloc.h"
#include "optmalloc.h"

void*
xmalloc(size_t bytes)
{
	printf("malloc.\n");
    return opt_malloc(bytes);
}

void
xfree(void* ptr)
{
	printf("free.\n");
    opt_free(ptr);
	printf("done.\n");
}

void*
xrealloc(void* prev, size_t bytes)
{	
	printf("realloc.\n");
    return opt_realloc(prev, bytes);
}

