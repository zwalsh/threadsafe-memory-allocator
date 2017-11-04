#include "optmalloc.h"
#include <stdio.h>
#include <pthread.h>

void*
thread_start(void* arg)
{
	int* arr = opt_malloc(sizeof(int) * 10);
	for (int i = 0; i < 10; ++i) {
		arr[i] = pthread_self();
	}
	for (int i = 0; i < 10; ++i) {
		printf("%d.\n", arr[i]);
	}
}

int
main()
{
	pthread_t* threads = malloc(5 * sizeof(pthread_t));
	for (int i = 0; i < 5; ++i) {
		pthread_create(threads + i, 0, &thread_start, 0);
	}

	for (int i = 0; i < 5; ++i) {
		pthread_join(threads[i], 0);
	}
}
