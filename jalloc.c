#include <stdio.h>
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#include "jalloc.h"

#define HEAP_SIZE (1024 * 1024) // 1 MB

#define ALIGNMENT 16

#define CHUNK_FREE 0x1
#define PREV_CHUNK_FREE 0x2
#define FLAGS_MASK 0xF
#define SIZE_MASK (~(size_t)FLAGS_MASK)

#define NUM_CLASSES 16
#define MIN_CLASS_SIZE 32

typedef struct Segment{
    void* base;
    size_t size;
    struct Segment* next;
} Segment;

typedef struct Chunk{
    size_t prev_size;  // payload size of the previous chunk
    size_t size_flags; // size in upper bits, flags in lower bits
} Chunk;

typedef struct FreeNode {
    struct FreeNode *next;
    struct FreeNode *prev;
} FreeNode;

Chunk* first_chunk = NULL;
Chunk* epilogue = NULL;
Segment* segments = NULL;
FreeNode* free_heads[NUM_CLASSES] = {0};
static SRWLOCK heap_lock = SRWLOCK_INIT;

/*

[Segment Metadata] [Prologue] [Actual Chunks] [Epilogue]   ---> Heap Layout

epilogue and prologue are always indicated as allocated with size 0, used for edge checks
each segment has its own prologue and epilogue, so coalescing does not cross segment boundaries

[Header] [Payload]  ---> Chunk Layout

Header contains this chunk's size and the previous chunk's size, so we can navigate both ways

*/

static size_t chunk_size_(Chunk* chunk){
    return chunk->size_flags & SIZE_MASK;
}

static bool chunk_is_free_(Chunk* chunk){
    return chunk->size_flags & CHUNK_FREE;
}

static bool prev_chunk_is_free_(Chunk* chunk){
    return chunk->size_flags & PREV_CHUNK_FREE;
}

static void chunk_set_size_(Chunk* chunk, size_t size){
    assert((size & FLAGS_MASK) == 0);

    chunk->size_flags = (chunk->size_flags & FLAGS_MASK) | size;
}

static void chunk_set_prev_size_(Chunk* chunk, size_t size){
    assert((size & FLAGS_MASK) == 0);

    chunk->prev_size = size;
}

static void chunk_set_free_(Chunk* chunk){
    chunk->size_flags |= CHUNK_FREE;
}

static void chunk_set_used_(Chunk* chunk){
    chunk->size_flags &= ~CHUNK_FREE;
}

static void chunk_set_prev_free_(Chunk* chunk){
    chunk->size_flags |= PREV_CHUNK_FREE;
}

static void chunk_set_prev_used_(Chunk* chunk){
    chunk->size_flags &= ~PREV_CHUNK_FREE;
}

/*

Bit-packed size_flags as size is aligned to 16 bytes, so lowest 4 bits are empty
Store free or allocated info in lower bits to save overhead

flagmask extracts lower 4 bits with & 0xF, sizemask extracts upper bits upto 64th bit with & ~0xF

*/

static size_t align_size_(size_t size){ //for alignment purposes 
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static size_t min_free_payload_size_(void){
    return align_size_(sizeof(FreeNode));
}

static size_t min_alloc_size_(void){
    return min_free_payload_size_();
}

static int size_class_(size_t size){
    int class = 0;
    size_t class_size = MIN_CLASS_SIZE;

    while (class < NUM_CLASSES - 1 && size > class_size){
        class++;
        class_size <<= 1;
    }

    return class;
}

static void insert_free_node_(FreeNode* node){
    Chunk* chunk = ((Chunk*)node) - 1;
    int class = size_class_(chunk_size_(chunk));

    node->next = free_heads[class];
    node->prev = NULL;

    if (free_heads[class]){
        free_heads[class]->prev = node;
    }

    free_heads[class] = node;
}

static void remove_free_node_(FreeNode* node){
    Chunk* chunk = ((Chunk*)node) - 1;
    int class = size_class_(chunk_size_(chunk));

    if (node->prev){
        node->prev->next = node->next;
    }
    else{
        free_heads[class] = node->next;
    }

    if (node->next){
        node->next->prev = node->prev;
    }
}

static Chunk* next_chunk_(Chunk* chunk){
    return (Chunk*)((char*)(chunk + 1) + chunk_size_(chunk));
}

static Chunk* prev_chunk_(Chunk* chunk){
    return (Chunk*)((char*)chunk - sizeof(Chunk) - chunk->prev_size);
}

static Chunk* create_segment_(size_t min_payload_size){
    size_t alloc_size = HEAP_SIZE;
    size_t required_size = sizeof(Segment) + ALIGNMENT + 3 * sizeof(Chunk) + min_payload_size;

    if (required_size > alloc_size){
        alloc_size = align_size_(required_size);
    }

    void* raw = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (raw == NULL){
        return NULL;
    }

    Segment* segment = (Segment*)raw;
    segment->base = raw;
    segment->size = alloc_size;
    segment->next = segments;
    segments = segment;

    uintptr_t aligned = ((uintptr_t)raw + sizeof(Segment) + (ALIGNMENT - 1)) & ~(uintptr_t)(ALIGNMENT - 1);
    Chunk* prologue = (Chunk*)aligned;

    prologue->prev_size = 0;
    prologue->size_flags = 0;
    chunk_set_size_(prologue, 0);
    chunk_set_used_(prologue);
    chunk_set_prev_used_(prologue);

    Chunk* first = prologue + 1;

    chunk_set_prev_size_(first, 0);
    first->size_flags = 0;
    chunk_set_free_(first);
    chunk_set_prev_used_(first);

    size_t raw_size = (size_t)((char*)raw + alloc_size - (char*)(first + 1) - sizeof(Chunk));
    size_t first_size = raw_size & SIZE_MASK;
    chunk_set_size_(first, first_size);

    Chunk* segment_epilogue = next_chunk_(first);

    chunk_set_prev_size_(segment_epilogue, first_size);
    segment_epilogue->size_flags = 0;
    chunk_set_size_(segment_epilogue, 0);
    chunk_set_used_(segment_epilogue);
    chunk_set_prev_free_(segment_epilogue);

    if (first_chunk == NULL){
        first_chunk = first;
    }
    epilogue = segment_epilogue;

    insert_free_node_((FreeNode*)(first + 1));

    return first;
}

static Chunk* coalesce_(Chunk* chunk){
    Chunk* next = next_chunk_(chunk);
    size_t chunk_size = chunk_size_(chunk);
    size_t next_size = chunk_size_(next);

    if(chunk_is_free_(next)){
        remove_free_node_((FreeNode*)(next + 1));

        chunk_size += sizeof(Chunk) + next_size;
        chunk_set_size_(chunk, chunk_size);
    }

    if(prev_chunk_is_free_(chunk)){
        Chunk* prev = prev_chunk_(chunk);
        size_t prev_size = chunk_size_(prev);
        remove_free_node_((FreeNode*)(prev + 1));

        prev_size += sizeof(Chunk) + chunk_size;
        chunk_set_size_(prev, prev_size);

        chunk = prev;
    }

    next = next_chunk_(chunk);
    chunk_set_prev_size_(next, chunk_size_(chunk));
    chunk_set_prev_free_(next);

    return chunk;
}

static int heap_init_unlocked_(void){
    if (first_chunk){
        return 0;
    }

    for (int i = 0; i < NUM_CLASSES; i++){
        free_heads[i] = NULL;
    }

    if (create_segment_(min_free_payload_size_()) == NULL){
        return -1;
    }

    return 0;
}

int heap_init(){
    AcquireSRWLockExclusive(&heap_lock);
    int result = heap_init_unlocked_();
    ReleaseSRWLockExclusive(&heap_lock);

    return result;
}

static void* heap_alloc_unlocked_(size_t size){
    size = align_size_(size);
    if (size < min_alloc_size_()){
        size = min_alloc_size_();
    }

    for (;;){
        int start_class = size_class_(size);

        for (int i = start_class; i < NUM_CLASSES; i++){
            FreeNode* current = free_heads[i];

            while(current != NULL){
                FreeNode* next_node = current->next;
                Chunk* chunk = ((Chunk*)current) - 1;

                if (chunk_is_free_(chunk) && chunk_size_(chunk) >= size){

                    if (chunk_size_(chunk) >= size + sizeof(Chunk) + min_free_payload_size_()){

                        Chunk* new_chunk = (Chunk*)((char*)(chunk + 1) + size);

                        chunk_set_prev_size_(new_chunk, size);
                        new_chunk->size_flags = 0;
                        size_t old_size = chunk_size_(chunk);

                        size_t new_chunk_size = old_size - size - sizeof(Chunk);

                        remove_free_node_(current);

                        chunk_set_size_(new_chunk, new_chunk_size);
                        
                        chunk_set_free_(new_chunk);
                        chunk_set_prev_used_(new_chunk);

                        FreeNode* new_node = (FreeNode*)(new_chunk + 1);
                        insert_free_node_(new_node);

                        chunk_set_size_(chunk, size);
                        chunk_set_used_(chunk);

                        Chunk* after_new = next_chunk_(new_chunk);
                        chunk_set_prev_size_(after_new, new_chunk_size);
                        chunk_set_prev_free_(after_new);

                        return (void*)(chunk + 1);
                    }

                    else{
                        Chunk* next = next_chunk_(chunk);
                        remove_free_node_(current);
                        chunk_set_used_(chunk);
                        chunk_set_prev_size_(next, chunk_size_(chunk));
                        chunk_set_prev_used_(next);
                        
                        return (void*)(chunk + 1);
                    }
                }
                
                current = next_node;
            }
        }

        if (create_segment_(size) == NULL){
            return NULL;
        }
    }
}

void* heap_alloc(size_t size) {
    AcquireSRWLockExclusive(&heap_lock);

    if (heap_init_unlocked_() != 0){
        ReleaseSRWLockExclusive(&heap_lock);
        return NULL;
    }

    void* ptr = heap_alloc_unlocked_(size);
    ReleaseSRWLockExclusive(&heap_lock);

    return ptr;
}

void heap_free(void* ptr) {
    if(ptr == NULL){
        return;
    }

    AcquireSRWLockExclusive(&heap_lock);

    Chunk* chunk = ((Chunk*)ptr) - 1;

    if(chunk_is_free_(chunk)){
        printf("Double free detected\n");
        ReleaseSRWLockExclusive(&heap_lock);
        return;
    }
    chunk_set_free_(chunk);

    Chunk* next = next_chunk_(chunk);
    chunk_set_prev_size_(next, chunk_size_(chunk));
    chunk_set_prev_free_(next);

    chunk=coalesce_(chunk);

    FreeNode* node = (FreeNode*)(chunk + 1);
    insert_free_node_(node);

    ReleaseSRWLockExclusive(&heap_lock);
}


// for testing


