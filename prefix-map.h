#ifndef PREFIX_MAP_H
#define PREFIX_MAP_H

#include "hashmap.h"

struct prefix_item {
	const char *name;
	size_t prefix_length;
};

/*
 * Given an array of names, find unique prefixes (i.e. the first <n> characters
 * that uniquely identify the names) and store the lengths of the unique
 * prefixes in the 'prefix_length' field of the elements of the given array..
 *
 * Typically, the `struct prefix_item` information is a field in the actual
 * item struct; For this reason, the `array` parameter is specified as an array
 * of pointers to the items.
 *
 * The `min_length`/`max_length` parameters define what length the unique
 * prefixes should have.
 *
 * If no unique prefix could be found for a given item, its `prefix_length`
 * will be set to 0.
 */
void find_unique_prefixes(struct prefix_item **array, size_t nr,
			  size_t min_length, size_t max_length);

#endif
