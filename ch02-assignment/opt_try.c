#include "optmalloc.h"
#include <stdio.h>
#include <pthread.h>

int** list;

void*
thread_one(void* arg)
{
	for (int ii = 0; ii < 100; ii++) {
		list[ii] = opt_malloc(100 * sizeof(int));
	}
	sleep(1);
	opt_free(list[99]);
}

void*
thread_two(void* arg)
{
	for (int ii = 0; ii < 99; ii++) {
		opt_free(list[ii]);
	}
}


int
main()
{
	list = opt_malloc(100 * sizeof(int*));	
	pthread_t* threads = (pthread_t*) opt_malloc(2 * sizeof(pthread_t));
	pthread_create(&threads[0], 0, thread_one, 0);
	sleep(1);
	pthread_create(&threads[1], 0, thread_two, 0);
	
	pthread_join(threads[0], 0);
	pthread_join(threads[1], 0);
}
