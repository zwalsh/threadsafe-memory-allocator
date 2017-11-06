#include <sys/mman.h>
#include <sys/types.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "optmalloc.h"

typedef struct free_cell {
	struct free_cell* next;
} free_cell;

typedef struct header {
	size_t size;
} header;

typedef struct extra_cell {
	struct extra_cell* next;
} extra_cell;


const size_t PAGE_SIZE = 4096;

typedef struct bin {
	size_t size;
	free_cell* first;
	void* unalloc;
} bin;

typedef struct page_header {
	size_t size;
	int thread;
} page_header;

__thread bin* arena;

typedef struct cache {
	long thread;
	free_cell* first;
	pthread_mutex_t mutex;
} cache;

typedef struct cache_list {
	cache* c;
	struct cache_list* next;
} cache_list;

cache_list* caches = NULL;
__thread extra_cell* extra_memory = NULL;
__thread int thread_num = -1;

pthread_mutex_t current_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
int current_thread = 1;

int PAGES_TO_ALLOCATE = 8;


void
check_rv(int rv)
{
	if (rv == -1) {
		perror("Whoops");
	}
}

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

void*
allocate_pages(size_t num_pages)
{
	void* ptr = mmap(0, PAGE_SIZE * num_pages, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	check_rv(*((int *) ptr));
	return ptr;
}

/**
 * Deallocates the pages pointed to by the given header.
 * The header must point to multiple pages of memory.
 */
void
deallocate_pages(header* h)
{
	assert(h->size > PAGE_SIZE);

	int rv = munmap(h, h->size);
	check_rv(rv);
}

void
initialize_bin(bin* b, size_t size)
{
	b->size = size;
	b->first = 0;
}

int
size_for_bin_number(int i)
{
	assert(i >= 0);
	if (i == 0) {
		return 24;
	}
	return pow(2, (i + 4)); // Now 1->32 and each successive bin will contain the next
}


void
initialize_arena() 
{
	// set arena variable
	arena = (bin*) allocate_pages(1);
	
	// configure a bin for 24, then powers of 2 up to 4096
	int bin_index = 0;
	size_t bin_size = size_for_bin_number(bin_index);
	while (bin_size <= PAGE_SIZE) {
		initialize_bin((arena + bin_index), bin_size);
		bin_index++;
		bin_size = size_for_bin_number(bin_index);
	}

	cache_list* cl = (cache_list*) (arena + bin_index);

	cache* c = (cache*) (cl + 1);
	
	cl->c = c;
	cl->next = caches;
	caches = cl;


	c->thread = thread_num;
	c->first = NULL;
	pthread_mutex_init(&(c->mutex), NULL);
}


bin*
pick_bin(size_t size) {
	if (size > PAGE_SIZE) {
		printf("Cannot use bins for sizes bigger than a page!n");
		exit(1);
	}

	bin* current = arena;
	while(current->size < size) {
		current = current + 1;
	}
	
	return current;
}

void*
get_page_start(void* ptr)
{
	uintptr_t addr = (uintptr_t) ptr;
	int offset_in_page = addr % PAGE_SIZE;
	return ptr - offset_in_page;
}


void
add_one_page_to_extra(void* start) {
	if (extra_memory == NULL) {
		extra_memory = (extra_cell*) start;
		extra_memory->next = NULL;
	} else {
		extra_cell* extra = (extra_cell*)start;
		extra->next = extra_memory;
		extra_memory = extra;
	}
}
	

void
add_extra_memory(void* pages_start, int num_to_alloc)  
{
	for (int ii = 0; ii < num_to_alloc; ++ii) {
		add_one_page_to_extra(pages_start);
		pages_start += PAGE_SIZE;
	}
}

void* 
get_page_from_extra() 
{	

	if (extra_memory == NULL) {
		add_extra_memory(allocate_pages(PAGES_TO_ALLOCATE), PAGES_TO_ALLOCATE);
	}		
		
	page_header* return_value = (page_header*)extra_memory;
	extra_memory = extra_memory->next;
	return return_value;
}

void*
get_chunk(bin* b) {
	if (b->first == NULL) {
		page_header* pg = get_page_from_extra();
		pg->size = b->size;
		pg->thread = thread_num;
		//set first
		b->first = (free_cell*) (pg + 1);
		b->first->next = NULL;
		//set unalloc
		b->unalloc = (void*) (b->first) + b->size;
	}
	
	free_cell* cell_to_return = b->first;
	b->first = b->first->next;
	if (b->first == NULL && b->unalloc != NULL) {
		//handle unalloc pointer
		b->first = b->unalloc;
		void* page_start = get_page_start(b->unalloc);
		void* next_unalloc = b->unalloc + b->size;
		if (next_unalloc > page_start + PAGE_SIZE - b->size) {
			b->unalloc = NULL;
		} else {
			b->unalloc = next_unalloc;
		}
	}

	cell_to_return->next = 0;
	return cell_to_return;
}

void*
opt_malloc(size_t size)
{	
	if (thread_num == -1) {
		pthread_mutex_lock(&current_thread_mutex);
		thread_num = current_thread;
		current_thread++;
		pthread_mutex_unlock(&current_thread_mutex);
	}
	if (arena == 0) {
		initialize_arena();
	}
	
	if (size >= PAGE_SIZE) {
		size += sizeof(page_header);
		int num_pages = div_up(size, PAGE_SIZE);
		page_header* ph = (page_header*) allocate_pages(num_pages);
		ph->size = size;
		ph->thread = thread_num;
		return (void*) (ph + 1);
	}

	bin* correct_bin = pick_bin(size);

	void* return_chunk = get_chunk(correct_bin);
	return return_chunk;
}


cache*
find_cache(long tn) 
{
	cache_list* current_l = caches;
	cache* current_c = current_l->c;
	while(current_c->thread != tn) {
		current_l = current_l->next;
		if (current_l == NULL) {
			exit(1);
		}
		current_c = current_l->c;
	}
	return current_c;
}


void
add_to_bin(free_cell* current, bin* correct_bin) {
	

	free_cell* temp = correct_bin->first;
	correct_bin->first = current;
	current->next = temp;
}


void
clear_cache(cache* c)
{	
	while(c->first != NULL) {
		free_cell* current = c->first;
		c->first = current->next;
		page_header* ph =(page_header*) get_page_start(current);
		size_t size_chunk = ph->size;
		bin* correct_bin = pick_bin(size_chunk);
		add_to_bin(current, correct_bin);
	}				

}

void
opt_free(void* item)
{
	page_header* page_start = (page_header*) get_page_start(item);
	int thread_number = page_start->thread;
	long size = page_start->size;	
	if (size > PAGE_SIZE) {
		header* pages = (header*) (item - sizeof(page_header));
		deallocate_pages(pages);
		return;
	}
	
	cache* correct_cache = find_cache(thread_num);

	free_cell* fc = (free_cell*) item;
	
	pthread_mutex_lock(&correct_cache->mutex);
	fc->next = correct_cache->first;
	correct_cache->first = fc;

	if (thread_number == thread_num) {
		clear_cache(correct_cache);
	}
	pthread_mutex_unlock(&correct_cache->mutex);
}

void*
opt_realloc(void* prev, size_t size)
{
	void* new_mem = opt_malloc(size);
	page_header* pg = (page_header*) get_page_start(prev);
	size_t old_size = pg->size;
	memcpy(new_mem, prev, old_size);
	opt_free(prev);
	return new_mem;
}
