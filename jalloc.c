#include <stdio.h>
#include <windows.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#include "jalloc.h"

#define HEAP_SIZE (1024 * 1024) // 1 MB

#define ALIGNMENT 16

#define MIN_PAYLOAD_SIZE ALIGNMENT
#define MIN_FREE_SIZE (sizeof(Chunk) + sizeof(Footer) + sizeof(FreeNode) + MIN_PAYLOAD_SIZE)

#define CHUNK_FREE 0x1
#define FLAGS_MASK 0xF
#define SIZE_MASK (~(size_t)FLAGS_MASK)

typedef struct Chunk{
    size_t size_flags; // size in upper bits, flags in lower bits
} Chunk;

typedef struct Footer{
    size_t size;
} Footer;

typedef struct FreeNode {
    struct FreeNode *next;
    struct FreeNode *prev;
} FreeNode;

Chunk* first_chunk = NULL;
Chunk* epilogue = NULL;
FreeNode* free_head = NULL;

/*

[Prologue] [Actual Chunks] [Epilogue]   ---> Heap Layout

epilogue and prologue are always indicated as allocated with size 0, used for edge checks
prologue has footer to make prev_chunk work, epilogue doesn't need footer since it's always at the end of the heap

[Header] [Payload] [Footer]  ---> Chunk Layout

Header and Footer both contain size of chunk, so we can easily navigate between chunks 

*/

static size_t chunk_size_(Chunk* chunk){
    return chunk->size_flags & SIZE_MASK;
}

static bool chunk_is_free_(Chunk* chunk){
    return chunk->size_flags & CHUNK_FREE;
}

static void chunk_set_size_(Chunk* chunk, size_t size){
    assert((size & FLAGS_MASK) == 0);

    chunk->size_flags = (chunk->size_flags & FLAGS_MASK) | size;
}

static void chunk_set_free_(Chunk* chunk){
    chunk->size_flags |= CHUNK_FREE;
}

static void chunk_set_used_(Chunk* chunk){
    chunk->size_flags &= ~CHUNK_FREE;
}

/*

Bit-packed size_flags as size is aligned to 16 bytes, wo lowest 4 bits are empty
Store free or allocated info in lower bits to save overhead

flagmask extracts lower 4 bits with & 0xF, sizemask extracts upper bits upto 64th bit with & ~0xF

*/

static size_t align_size_(size_t size){ //for alignment purposes 
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static Footer* get_footer_(Chunk *chunk){
    return (Footer *)((char *)(chunk + 1) + chunk_size_(chunk));
}

static void insert_free_node_(FreeNode* node){
    node->next = free_head;
    node->prev = NULL;

    if (free_head){
        free_head->prev = node;
    }

    free_head = node;
}

static void remove_free_node_(FreeNode* node){
    if (node->prev){
        node->prev->next = node->next;
    }
    else{
        free_head = node->next;
    }

    if (node->next){
        node->next->prev = node->prev;
    }
}

static void replace_free_node_(FreeNode* old, FreeNode* new){
    new->prev = old->prev;
    new->next = old->next;

    if (new->prev){
        new->prev->next = new;
    }
    else{
        free_head = new;
    }

    if (new->next){
        new->next->prev = new;
    }
}

static Chunk* next_chunk_(Chunk* chunk){
    return (Chunk*)((char*)get_footer_(chunk) + sizeof(Footer));
}

static Chunk* prev_chunk_(Chunk* chunk){
    Footer* prev_footer = (Footer*)((char*)chunk - sizeof(Footer));

    return (Chunk*)((char*)chunk - sizeof(Chunk) - sizeof(Footer) - prev_footer->size);
}

static Chunk* coalesce_(Chunk* chunk){
    Chunk* next = next_chunk_(chunk);
    size_t chunk_size = chunk_size_(chunk);
    size_t next_size = chunk_size_(next);

    if(chunk_is_free_(next)){
        remove_free_node_((FreeNode*)(next + 1));

        chunk_size += sizeof(Chunk) + sizeof(Footer) + next_size;
        chunk_set_size_(chunk, chunk_size);
    }

    Footer* footer = get_footer_(chunk);
    footer->size = chunk_size;

    Chunk* prev = prev_chunk_(chunk);
    size_t prev_size = chunk_size_(prev);

    if(chunk_is_free_(prev)){
        remove_free_node_((FreeNode*)(prev + 1));

        prev_size += sizeof(Chunk) + sizeof(Footer) + chunk_size;
        chunk_set_size_(prev, prev_size);

        footer = get_footer_(prev);
        footer->size = prev_size;

        chunk = prev;
    }

    return chunk;
}

int heap_init(){
    if (first_chunk){
        return 0;
    }

    void* raw = (Chunk*)VirtualAlloc(NULL, HEAP_SIZE + ALIGNMENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // allocate extra for alignment so that we can align the first chunk to 16 bytes

    if (raw == NULL){
        return -1;
    }

    uintptr_t aligned = ((uintptr_t)raw + (ALIGNMENT - 1)) & ~(uintptr_t)(ALIGNMENT - 1); // align the first chunk to 16 bytes, this is what the extra mem was for

    Chunk* prologue = (Chunk*)(aligned + sizeof(Chunk));

    chunk_set_size_(prologue, 0);
    chunk_set_used_(prologue);

    Footer* prologue_footer = get_footer_(prologue);
    prologue_footer->size = 0;

    Chunk* first = (Chunk*)((char*)prologue_footer + sizeof(Footer));

    first->size_flags = 0;
    chunk_set_free_(first);

    size_t raw_size = HEAP_SIZE - 3*sizeof(Chunk) - 2*sizeof(Footer); // 3*chunk for epilogue+prologue+heap, 2*footer for prologue and heap
    size_t first_size = raw_size & ~(size_t)FLAGS_MASK;  // align to 16
    chunk_set_size_(first, first_size); 

    Footer* first_footer = get_footer_(first);
    first_footer->size = chunk_size_(first);

    epilogue = (Chunk*)((char*)first_footer + sizeof(Footer));

    chunk_set_size_(epilogue, 0);
    chunk_set_used_(epilogue);

    first_chunk = first;


    free_head = (FreeNode*)(first_chunk + 1);
    free_head->next = NULL;
    free_head->prev = NULL;

    return 0;
}

void* heap_alloc(size_t size) {

    if(!first_chunk){
        if (heap_init() != 0){
            return NULL;
        }
    }
    
    size = align_size_(size);

    FreeNode* current = free_head;

    while(current != NULL){
        Chunk* chunk = ((Chunk*)current) - 1;

        if (chunk_is_free_(chunk) && chunk_size_(chunk) >= size){

            if (chunk_size_(chunk) >= size + MIN_FREE_SIZE){

                Chunk* new_chunk = (Chunk*)((char*)(chunk + 1) + size + sizeof(Footer));

                new_chunk->size_flags = 0;
                size_t old_size = chunk_size_(chunk);

                size_t new_chunk_size = old_size - size - sizeof(Chunk) - sizeof(Footer);

                chunk_set_size_(new_chunk, new_chunk_size);
                
                chunk_set_free_(new_chunk);

                Footer* new_footer = get_footer_(new_chunk);
                new_footer->size = chunk_size_(new_chunk);

                FreeNode* new_node = (FreeNode*)(new_chunk + 1);
                
                replace_free_node_(current, new_node);
                chunk_set_size_(chunk, size);
                chunk_set_used_(chunk);

                Footer* footer = get_footer_(chunk);
                footer->size = chunk_size_(chunk);

                return (void*)(chunk + 1);
            }

            else{
                remove_free_node_(current);
                chunk_set_used_(chunk);
                
                return (void*)(chunk + 1);
            }
        }
        
        current = current->next;
    }

    return NULL;

}

void heap_free(void* ptr) {
    if(ptr == NULL){
        return;
    }

    Chunk* chunk = ((Chunk*)ptr) - 1;

    if(chunk_is_free_(chunk)){
        printf("Double free detected\n");
        return;
    }
    chunk_set_free_(chunk);

    chunk=coalesce_(chunk);

    FreeNode* node = (FreeNode*)(chunk + 1);
    insert_free_node_(node);

}


// for testing


