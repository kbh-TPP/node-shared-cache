#include<uv.h>
#include<stdio.h>
#include<string.h>
#include<errno.h>
#include "memcache.h"

#define    MAGIC_NUM    0xdeadbeef
#define    BLK_SIZE     1024

namespace cache {

typedef struct node_slave_s {
    uint32_t    slave_next;
    uint8_t        data[0];
} node_slave_t;

typedef struct node_s {
    uint32_t    slave_next;
    uint32_t    prev;
    uint32_t    next;
    uint32_t    hash_next;
    uint32_t    blocks;
    uint32_t    valLen;
    uint16_t    keyLen;
    uint16_t    hash;
    uint16_t    key[0];
} node_t;


typedef    struct cache_s {
    uint32_t    bitmap[65536]; // maximum memory size available is 65536*32*1K = 2G
    uint32_t    hashmap[65536];
    struct {
        uint32_t    magic;
        uint32_t    size;
        uint32_t    blocks_used;
        uint32_t    blocks_total; // maximum block count = 65536*32-513 = 2096639
        uint32_t    head;
        uint32_t    tail;
        uint32_t    nextBit; // next block to look for when allocating block

        uv_rwlock_t    lock;
    } info;

} cache_t;

void init(void* ptr, size_t size) {
    cache_t& cache = *static_cast<cache_t*>(ptr);
    if(cache.info.magic == MAGIC_NUM && cache.info.size == size) { // already initialized
        fprintf(stderr, "init cache: already initialized\n");
        return;
    }
    memset(&cache, 0, sizeof(cache_t));


    uv_rwlock_init(&cache.info.lock);
    uv_rwlock_wrlock(&cache.info.lock);
    
    cache.info.magic = MAGIC_NUM;
    cache.info.size = size;
    
    uv_rwlock_wrunlock(&cache.info.lock);
    
    uint32_t blocks = size >> 10;
    cache.info.blocks_total = blocks - 513;
    cache.info.blocks_used = 0;
    memset(cache.bitmap, 0xff, 64); // fill first 64 bytes of bitmap.
    cache.bitmap[16] = 1; // first block is used by cache.info
    cache.info.nextBit = 513;

    fprintf(stderr, "init cache: size %d, blocks %d, usage %d/%d\n", size, blocks, cache.info.blocks_used, cache.info.blocks_total);
}

inline void drop(cache_t& cache) {
    fprintf(stderr, "dropping least recently used...\n");
    // TODO
}

inline uint32_t selectOne(cache_t& cache) {
    uint32_t curr = cache.info.nextBit >> 5, i = cache.info.nextBit & 31;
    uint32_t bits = cache.bitmap[curr];
    while(bits == 0xffffffff) {
        i = 0;
        curr++;
        bits = cache.bitmap[curr];
    }
    // bits is not full 1
    for(; ; i++) {
        uint32_t mask = 1 << i;
        if(bits & mask) continue;
        cache.bitmap[curr] = bits | mask;
        uint32_t result = curr << 5 | i;
        if(result >= cache.info.blocks_total + 512) {
            cache.info.nextBit = 513;
        } else {
            cache.info.nextBit = result + 1;
        }
        return result;
    }
}

template<typename T>
inline T* address(void* base, uint32_t block) {
    return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + (block << 10));
}

inline uint32_t allocate(cache_t& cache, uint32_t count) {
    uint32_t target = cache.info.blocks_total - count;
    if(cache.info.blocks_used > target) { // not enough
        do {
            drop(cache);
        } while (cache.info.blocks_used > target);
    }
    // TODO
    uint32_t firstBlock = selectOne(cache);
    fprintf(stderr, "select %d blocks (first: %d)\n", count, firstBlock);
    cache.info.blocks_used += count;

    node_slave_t* node = address<node_slave_t>(&cache, firstBlock);
    while(--count) {
        uint32_t nextBlock = selectOne(cache);
        fprintf(stderr, "selected block  %d\n", nextBlock);
        node->slave_next = nextBlock;
        node = address<node_slave_t>(&cache, nextBlock);
    }
    return firstBlock;
}

inline uint32_t find(cache_t& cache, uint16_t hash, const uint16_t* key, size_t keyLen) {
    uint32_t curr = cache.hashmap[hash];
    node_t* pnode;
    fprintf(stderr, "cache::get finding match key (len:%d)\n", keyLen);
    for(; curr; curr = pnode->hash_next) {
        pnode = address<node_t>(&cache, curr);
        fprintf(stderr, "cache::get testing node %d (keyLen:%d)\n", curr, pnode->keyLen);
        if(pnode->keyLen == keyLen && !memcmp(pnode->key, key, keyLen << 1)) {
            // found it, curr is the block num and pnode is the pointer.
            fprintf(stderr, "cache::get matched on block %d\n", curr);
            break;
        }
        // because keyLen is less than 256, offset never reaches block end.
        // for(uint32_t i = 0; i < keyLen; i++, offset++) {
        //     if(offset == BLK_SIZE >> 1) { // reach block end
        //         ptr = address<uint16_t>(&cache, reinterpret_cast<node_slave_t*>(ptr)->slave_next);
        //         offset = sizeof(node_slave_t);
        //     }
        //     fprintf(stderr, "cache::get testing key char %d:%d\n", key[i], ptr[offset]);
        //     if(key[i] != ptr[offset]) { // key not match
        //         curr = pnode->hash_next;
        //         goto outer;
        //     }
        // }
    }
    if(curr && cache.info.tail != curr) { // found, bring to tail
        node_t& node = *pnode;
        address<node_t>(&cache, node.prev)->next = node.next;
        address<node_t>(&cache, node.next)->prev = node.prev;
        node.next = 0;
        node.prev = cache.info.tail;
        address<node_t>(&cache, cache.info.tail)->next = curr;
        cache.info.tail = curr;
    }
    return curr;
}

inline void release(cache_t& cache, uint32_t& block) {
    for(uint32_t next = block; next; next = address<node_slave_t>(&cache, next)->slave_next) {
        fprintf(stderr, "free block %d assert(bit:%d)\n", next, (cache.bitmap[next >> 5] >> (next & 31)) & 1);
        cache.bitmap[next >> 5] ^= 1 << next & 31;
    }
    block = 0;
}

inline void slave_next(cache_t& cache, node_slave_t*& node, uint32_t n) {
    for(uint32_t i = 1; i < n; i++) {
        node = address<node_slave_t>(&cache, node->slave_next);
    }
}

inline uint16_t hashsum(const uint16_t* key, size_t keyLen) {
    uint32_t hash = 0;
    for(size_t i = 0; i < keyLen; i++) {
        hash = hash * 31 + key[i];
    }
    return hash;
}

void get(void* ptr, const uint16_t* key, size_t keyLen, uint8_t*& retval, size_t& retvalLen) {
    fprintf(stderr, "cache::get: key len %d\n", keyLen);
    cache_t& cache = *static_cast<cache_t*>(ptr);

    uint16_t hash = hashsum(key, keyLen);
    uv_rwlock_rdlock(&cache.info.lock);
    uint32_t found = find(cache, hash, key, keyLen);
    if(found) {
        node_t* pnode = address<node_t>(&cache, found);
        size_t  valLen = retvalLen = pnode->valLen;
        uint8_t* val = retval = new uint8_t[valLen];

        node_slave_t* currentBlock = reinterpret_cast<node_slave_t*>(pnode);
        uint32_t offset = sizeof(node_t) + (keyLen << 1);
        uint32_t capacity = BLK_SIZE - offset;

        while(capacity < valLen) {
            fprintf(stderr, "copying val (%x+%d) %d bytes\n", currentBlock, offset, capacity);
            memcpy(val, reinterpret_cast<uint8_t*>(currentBlock) + offset, capacity);
            val += capacity;
            valLen -= capacity;
            currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
            offset = sizeof(node_slave_t);
            capacity = BLK_SIZE - sizeof(node_slave_t);
        }
        if(valLen) { // capacity >= valLen
            fprintf(stderr, "copying remaining val (%x+%d) %d bytes\n", currentBlock, offset, valLen);
            memcpy(val, reinterpret_cast<uint8_t*>(currentBlock) + offset, valLen);
        }
    } else {
        retval = NULL;
    }

    uv_rwlock_rdunlock(&cache.info.lock);
}

int set(void* ptr, const uint16_t* key, size_t keyLen, const uint8_t* val, size_t valLen) {
    size_t totalLen = keyLen + valLen + sizeof(node_s) - 1;
    uint32_t blocksRequired = totalLen / 1023 + (totalLen % 1023 ? 1 : 0);
    fprintf(stderr, "cache::set: total len %d (%d blocks required)\n", totalLen, blocksRequired);

    cache_t& cache = *static_cast<cache_t*>(ptr);

    if(blocksRequired > cache.info.blocks_total) {
        errno = E2BIG;
        return -1;
    }

    uint16_t hash = hashsum(key, keyLen);
    uv_rwlock_wrlock(&cache.info.lock);

    // find if key is already exists
    uint32_t found = find(cache, hash, key, keyLen);
    node_t* selectedBlock;

    if(found) { // update
        selectedBlock = address<node_t>(&cache, found);
        node_t& node = *selectedBlock;
        if(node.blocks > blocksRequired) { // free extra blocks
            node_slave_t* pcurr = reinterpret_cast<node_slave_t*>(&node);
            slave_next(cache, pcurr, blocksRequired);
            // drop remaining blocks
            fprintf(stderr, "freeing %d blocks\n", node.blocks - blocksRequired);
            release(cache, pcurr->slave_next);
        } else if(node.blocks < blocksRequired) {
            node_slave_t* pcurr = reinterpret_cast<node_slave_t*>(&node);
            slave_next(cache, pcurr, node.blocks);
            // allocate more blocks
            fprintf(stderr, "acquiring extra %d blocks assert(last:%d == 0)\n", blocksRequired - node.blocks, pcurr->slave_next);
            pcurr->slave_next = allocate(cache, blocksRequired - node.blocks);
        }
        node.blocks = blocksRequired;
    } else { // insert
        // insert into hash table
        uint32_t firstBlock = allocate(cache, blocksRequired);
        selectedBlock = address<node_t>(&cache, firstBlock);
        node_t& node = *selectedBlock;
        node.blocks = blocksRequired;
        node.hash_next = cache.hashmap[hash]; // insert into linked list
        cache.hashmap[hash] = firstBlock;
        node.hash = hash;
        node.keyLen = keyLen;

        if(!cache.info.tail) {
            cache.info.head = cache.info.tail = firstBlock;
        } else {
            address<node_t>(&cache, cache.info.tail)->next = firstBlock;
            node.prev = cache.info.tail;
            cache.info.tail = firstBlock;

        }
    }
    selectedBlock->valLen = valLen;

    // copy key
    fprintf(stderr, "copying key (len:%d)\n", keyLen);
    if(!found) memcpy(selectedBlock->key, key, keyLen << 1);
    // keyLen <= 256 < capacity
    // while(capacity < keyLen) {
    //     if(!found) memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, key, capacity << 1);
    //     key += capacity;
    //     keyLen -= capacity;
    //     currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
    //     offset = sizeof(node_slave_t);
    //     capacity = (BLK_SIZE - sizeof(node_slave_t)) >> 1;
    // }
    // if(keyLen) { // capacity >= keyLen
    // }

    // copy values
    node_slave_t* currentBlock = reinterpret_cast<node_slave_t*>(selectedBlock);
    uint32_t offset = sizeof(node_t) + (keyLen << 1);
    uint32_t capacity = BLK_SIZE - offset;

    fprintf(stderr, "copying val (len:%d currentBlock:%x offset:%d)\n", valLen, currentBlock, offset);

    while(capacity < valLen) {
        fprintf(stderr, "copying val (%x+%d) %d bytes\n", currentBlock, offset, capacity);
        memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, val, capacity);
        val += capacity;
        valLen -= capacity;
        currentBlock = address<node_slave_t>(&cache, currentBlock->slave_next);
        offset = sizeof(node_slave_t);
        capacity = BLK_SIZE - sizeof(node_slave_t);
    }

    if(valLen) { // capacity >= valLen
        fprintf(stderr, "copying remaining val (%x+%d) %d bytes\n", currentBlock, offset, valLen);
        memcpy(reinterpret_cast<uint8_t*>(currentBlock) + offset, val, valLen);
    }

    uv_rwlock_wrunlock(&cache.info.lock);
    return 0;
}


void enumerate(void* ptr, EnumerateCallback& enumerator) {
    cache_t& cache = *static_cast<cache_t*>(ptr);
    uv_rwlock_rdlock(&cache.info.lock);
    uint32_t curr = cache.info.head;

    while(curr) {
        node_t& node = *address<node_t>(&cache, curr);
        enumerator.next(node.key, node.keyLen);
        curr = node.next;
    }

    uv_rwlock_rdunlock(&cache.info.lock);
}

}