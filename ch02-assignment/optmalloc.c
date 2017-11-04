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

/*
  typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
  } hm_stats;
*/

typedef struct free_cell {
	struct free_cell* next;
} free_cell;

typedef struct header {
	size_t size;
} header;


const size_t PAGE_SIZE = 4096;
const bool DEBUG = false;
static hm_stats stats; // This initializes the stats to 0.

typedef struct bin {
	size_t size;
	free_cell* first;
	void* unalloc;
//	pthread_mutex_t mutex;
} bin;

typedef struct page_header {
	size_t size;
	long thread;
} page_header;

pthread_key_t arena_key;
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

void
check_rv(int rv)
{
	if (rv == -1) {
		perror("Whoops");
	}
}

long
free_list_length_from(free_cell* cell, long acc)
{
	if (cell == 0) {
		return acc;
	}
	return free_list_length_from(cell->next, acc + 1);
}

void
hprintstats()
{
    stats.free_length = 0;
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
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
	stats.pages_mapped += num_pages;
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
	stats.pages_unmapped += div_up(h->size, PAGE_SIZE);
	int rv = munmap(h, h->size);
	check_rv(rv);
}

void
initialize_bin(bin* b, size_t size)
{
	if (DEBUG) {
		printf("init_bin(): thread: %lu, bin_size: %lu.\n", pthread_self(), size);
	}
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
	if (DEBUG) {
		printf("init_arena(): thread: %lu.\n", pthread_self());
	}
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


	c->thread = pthread_self();
	c->first = NULL;
	pthread_mutex_init(&(c->mutex), NULL);
}


bin*
pick_bin(size_t size) {
	if(DEBUG) {
//		printf("pick bin: thread: %lu, size %lu.\n", pthread_self(), size);
	}

	if (size > PAGE_SIZE) {
		printf("Cannot use bins for sizes bigger than a page!\n");
	}

	bin* current = arena;
	while(current->size < size) {
		current = current + 1;
	}


	if(DEBUG) {
//		printf("pick bin: thread: %lu, bin of size %lu.\n", pthread_self(), current->size);
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

void*
get_chunk(bin* b) {
	if (DEBUG) {
		printf("get chunk: thread: %lu, bin size: %lu.\n", pthread_self(), b->size);
	}

	if (b->first == NULL) {	
		page_header* pg = (page_header*) allocate_pages(1);
		pg->size = b->size;
		pg->thread = pthread_self();
		//set first
		b->first = (free_cell*) (pg + 1);
		b->first->next = NULL;
	//	printf("Pg: %p, first: %p, size: %lu.\n", pg, b->first, sizeof(page_header));
		//set unalloc
		b->unalloc = (void*) (b->first) + b->size;
	}
	
	if (b->first == 0x10) {
		printf("b->first is 0x10!\n");
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

	if (DEBUG) {
		printf("Got chunk: %p.\n", cell_to_return);
	}
	cell_to_return->next = 0;
	return cell_to_return;
}

void*
opt_malloc(size_t size)
{
	if (DEBUG) {
		printf("malloc(): thread: %lu, size: %lu.\n", pthread_self(), size);
	}
	stats.chunks_allocated += 1;
	if (arena_key == 0) {
		int rv = pthread_key_create(&arena_key, NULL);
		check_rv(rv);
	}
	
	if (arena == 0) {
		initialize_arena();
	}
	
	if (size >= PAGE_SIZE) {
		size += sizeof(page_header);
		int num_pages = div_up(size, PAGE_SIZE);
		page_header* ph = (page_header*) allocate_pages(num_pages);
		ph->size = size;
		ph->thread = pthread_self();
		return (void*) (ph + 1);
	}

	bin* correct_bin = pick_bin(size);

	void* return_chunk = get_chunk(correct_bin);	
	if (return_chunk == 0x10) {
		printf("Got chunk 0x10.\n");
		exit(1);
	}	
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
	if (DEBUG) {
		printf("clear_cache(): for thread: %lu.\n", c->thread);
	}	
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
	stats.chunks_freed += 1;
	page_header* page_start = (page_header*) get_page_start(item);
	long thread_num = page_start->thread;
	long size = page_start->size;	
	if (DEBUG) {
		printf("free(): %p, from thread %lu, in thread %lu.\n", item, thread_num, pthread_self());
	}
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

	if (thread_num == pthread_self()) {
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
