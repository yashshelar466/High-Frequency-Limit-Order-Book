#pragma once
#include <vector>
#include <cstddef>
#include <iostream>
#include <unordered_set>

template <typename T, size_t BlockSize = 100000>
class MemoryPool {
public:
    MemoryPool() {
        // Pre-allocate memory block
        pool.resize(BlockSize);
        free_list.reserve(BlockSize);

        // Populate free list with pointers to pre-allocated objects
        for (size_t i = 0; i < BlockSize; ++i) {
            free_list.push_back(&pool[i]);
        }
    }

    // Allocate an object from the pool without hitting OS heap
    template <typename... Args>
    T* allocate(Args&&... args) {
        if (free_list.empty()) {
            std::cerr << "MemoryPool exhausted! Falling back to raw heap." << std::endl;
            T* heap_ptr = new T(std::forward<Args>(args)...);
            heap_allocated.insert(heap_ptr);
            return heap_ptr;
        }

        T* ptr = free_list.back();
        free_list.pop_back();

        // Construct object in-place using placement new
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // Return object to pool free list for reuse, or to the real heap
    // if it was a fallback allocation.
    void deallocate(T* ptr) {
        if (!ptr) return;

        auto it = heap_allocated.find(ptr);
        if (it != heap_allocated.end()) {
            // This pointer never lived in `pool` — it must go through
            // delete, not back onto free_list, or free_list ends up
            // holding a dangling pointer outside the pool's storage.
            delete ptr;
            heap_allocated.erase(it);
            return;
        }

        // Call destructor explicitly
        ptr->~T();

        // Return memory location to free list
        free_list.push_back(ptr);
    }

    size_t available() const {
        return free_list.size();
    }

    // Number of live objects that overflowed into raw heap allocation.
    // Non-zero means BlockSize was too small for this run — worth
    // sizing the pool to the real order volume instead of leaving this at 0.
    size_t heap_overflow_count() const {
        return heap_allocated.size();
    }

private:
    std::vector<T> pool;
    std::vector<T*> free_list;
    std::unordered_set<T*> heap_allocated;
};