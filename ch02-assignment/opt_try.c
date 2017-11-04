#include "optmalloc.h"
#include <stdio.h>
#include <pthread.h>

void*
thread_one(void* arg)
{
	
}


int** list;

int
main()
{
	list = opt_malloc(100 * sizeof(int*));	
}
