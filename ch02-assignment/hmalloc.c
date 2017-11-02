#include <sys/mman.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>

#include "hmalloc.h"

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
	size_t size;
	struct free_cell* next;
	struct free_cell* prev;
} free_cell;

typedef struct header {
	size_t size;
} header;


const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; 
static free_cell* free_list_head;

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

long
free_list_length()
{
	return free_list_length_from(free_list_head, 0);
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length();
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

free_cell*
add_memory()
{
	stats.pages_mapped += 1;
	free_cell* cell = (free_cell*) mmap(0, 
					PAGE_SIZE,
					PROT_READ|PROT_WRITE,
					MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	cell->size = PAGE_SIZE;
	cell->next = 0;
	cell->prev = 0;
	return cell;
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

/**
 * Inserts the cell to add into the free list before the current cell.
 */
void
insert_before(free_cell* current, free_cell* to_add)
{
	assert(current != 0 && to_add != 0);
	if (current->prev == 0) {
		// current cell is the free_list_head.
		free_list_head = to_add;
		to_add->next = current;
		to_add->prev = 0;
		current->prev = to_add;
		return;
	}
	free_cell* prior_cell = current->prev;
	prior_cell->next = to_add;
	to_add->prev = prior_cell;
	
	current->prev = to_add;
	to_add->next = current;
}

/**
 *  Inserts the cell to add into the free list after the current cell.
 */
void
insert_after(free_cell* current, free_cell* to_add)
{
	assert(current != 0 && to_add != 0);
	if (current->next == 0) {
		current->next = to_add;
		to_add->prev = current;
		to_add->next = 0;
		return;
	}
	free_cell* next_cell = current->next;
	next_cell->prev = to_add;
	to_add->next = next_cell;

	current->next = to_add;
	to_add->prev = current;
}

void
merge_cells(free_cell* c1, free_cell* c2)
{
	c1->size = c1->size + c2->size;
	c1->next = c2->next;
	if (c1->next != 0) {
		c1->next->prev = c1;
	}
}


bool
check_adjacent(free_cell* c1, free_cell* c2)
{
	return (((void*) c1) + c1->size) == c2;
}

/**
 * Coalesces contiguous free cells in the list, checking at the current
 * cell to see if it touches the previous and next chunks.
 */
void
coalesce_at_cell(free_cell* cell)
{
	free_cell* prev = cell->prev;
	if (prev != 0 && check_adjacent(prev, cell)) {
		merge_cells(prev, cell);
		cell = prev;
	}	
	free_cell* next = cell->next;
	if (next != 0 && check_adjacent(cell, next)) {
		merge_cells(cell, next);
	}
}

/**
 * Inserts the given chunk of memory into the free list.
 * Maintains sorted order by address.
 * Coalesces contiguous chunks of free memory.
 */
void
insert_chunk_into_list(header* h)
{
	size_t size = h->size;
	free_cell* cell = (free_cell*) h;
	cell->size = size;
	cell->prev = 0;
	cell->next = 0;
	if (free_list_head == 0) {
		free_list_head = cell;
		// don't have to coalesce here - the free list is empty.
		return;
	}
	
	free_cell* current = free_list_head;
	while (current < cell) {
		if (current->next == 0) {
			insert_after(current, cell);
			coalesce_at_cell(cell);
			return;
		}
		current = current->next;
	}
	insert_before(current, cell);
	coalesce_at_cell(cell);

}

/**
 * Returns the first cell of the given size,
 * obtaining new memory if necessary.
 */
free_cell*
first_cell_of_size(size_t size) 
{
	assert(size < PAGE_SIZE);

	if (free_list_head == 0) {
		free_list_head = add_memory();
		free_list_head->prev = 0;
		free_list_head->next = 0;
		return free_list_head;
	}

	free_cell* current = free_list_head;
	while (current != 0 && current->size < size) {
		current = current->next;
	}
	if (current == 0) {
		free_cell* more_mem = add_memory();
		insert_chunk_into_list((header*)more_mem);
		return more_mem;
	} else {
		return current;
	}
}

void
split_and_remove_cell(free_cell* cell, size_t size) 
{
	assert(cell != 0);
	if (cell->size - size > sizeof(free_cell)) {
		free_cell* split =(free_cell*) (((void*)cell) + size);
		split->size = cell->size - size;
		split->next = cell->next;
		split->prev = cell->prev;
		if (cell->prev != 0) {
			cell->prev->next = split;
		} else {
			free_list_head = split;
		}
		if (cell->next != 0) {
			cell->next->prev = split;
		}
		return;
	} 

	// Cell was too small to split
	

	if (cell->prev != 0 && cell->next != 0) { // in middle of list
		cell->prev->next = cell->next;
		cell->next->prev = cell->prev;
	} else if (cell->prev == 0 && cell->next != 0) { // at front of list
		free_list_head = cell->next;
		free_list_head->prev = 0;
	} else if (cell->prev != 0 && cell->next == 0) { // at end of list
		cell->prev->next = 0;
	} else { // only item in list
		free_list_head = 0;
	}
}

free_cell*
free_cell_at_address(void* addr)
{
	free_cell* current = free_list_head;
	while ((void*)current < addr) {
		if (current == 0) {
			return 0;
		}
		current = current->next;
	}
	if (current == addr) {
		return current;
	} else {
		return 0;
	}
}

void*
hmalloc(size_t size)
{
	pthread_mutex_lock(&mutex);
	stats.chunks_allocated += 1;
	size += sizeof(size_t);
	
	if (size < sizeof(free_cell)) {
		size = sizeof(free_cell);
	}

	if (size >= PAGE_SIZE) {
		size_t num_pages = div_up(size, PAGE_SIZE);
		header* h = (header*) allocate_pages(num_pages);
		h->size = size;
		return ((void*) h) + sizeof(size_t);
	}

	// obtain a cell of the necessary size
	free_cell* cell = first_cell_of_size(size);

	// remove this cell from the free list
	split_and_remove_cell(cell, size);
	// return the properly incremented pointer
	header* h = (header*) cell;
	h->size = size;
	pthread_mutex_unlock(&mutex);
	return ((void*) h) + sizeof(size_t);
}

void
hfree(void* item)
{
	pthread_mutex_lock(&mutex);
	stats.chunks_freed += 1;
	header* h = (header*) (item - sizeof(size_t));
	size_t size = h->size;
	

	if (size < PAGE_SIZE) {
		insert_chunk_into_list(h);
	} else {
		deallocate_pages(h);
	}
	pthread_mutex_unlock(&mutex);
}

void*
hrealloc(void* prev, size_t size)
{
	pthread_mutex_lock(&mutex);
	// if free after, expand...
	printf("Realloc.\n");
	size_t current_size = *((long*) (prev - sizeof(size_t)));
	void* next_cell = prev + current_size;
	
	free_cell* available =  free_cell_at_address(next_cell);
	
	if (available != 0 && available->size + current_size <= size) {
		split_and_remove_cell(available, size - current_size);
		available->size = 0;
		available->next = 0;
		available->prev = 0;
		*((long*) (prev - sizeof(size_t))) = size;
		pthread_mutex_unlock(&mutex);
		printf("Done.\n");
		return prev;
	}
	
	// else hmalloc and copy, then hfree
	void* new_mem = hmalloc(size);
	int rv = *((int*)memcpy(new_mem, prev, current_size));
	hfree(prev);	
	pthread_mutex_unlock(&mutex);
	printf("Done.\n");
	return new_mem;
}
