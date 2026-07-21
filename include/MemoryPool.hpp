#pragma once
#include <vector>
#include <cstddef>
#include <iostream>

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
            return new T(std::forward<Args>(args)...);
        }

        T* ptr = free_list.back();
        free_list.pop_back();

        // Construct object in-place using placement new
        return new (ptr) T(std::forward<Args>(args)...);
    }

    // Return object to pool free list for reuse
    void deallocate(T* ptr) {
        if (!ptr) return;

        // Call destructor explicitly
        ptr->~T();

        // Return memory location to free list
        free_list.push_back(ptr);
    }

    size_t available() const {
        return free_list.size();
    }

private:
    std::vector<T> pool;
    std::vector<T*> free_list;
};