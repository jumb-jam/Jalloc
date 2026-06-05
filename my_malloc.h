#pragma once

#include <stddef.h>

int heap_init();

void* heap_alloc(size_t size);

void heap_free(void* ptr);