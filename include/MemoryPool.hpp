#pragma once
#include <vector>
#include <cstddef>
#include <iostream>
#include <unordered_set>
#include <utility>
#include <new>

// Fixed-capacity object pool over RAW, uninitialized storage. Each slot is
// constructed via placement-new on allocate() and destroyed via an explicit
// ~T() on deallocate(). The backing store is raw bytes (not std::vector<T>),
// so the pool's own teardown does NOT run T's destructor again. The previous
// vector<T>-backed version default-constructed every slot and then re-destroyed
// slots already destroyed in deallocate() — a latent double-free for any
// non-trivially-destructible T (invisible only because Order is trivial).
template <typename T, size_t BlockSize = 100000>
class MemoryPool {
public:
    MemoryPool() {
        storage_ = new Slot[BlockSize];   // raw aligned bytes; no T constructed
        free_list.reserve(BlockSize);
        for (size_t i = 0; i < BlockSize; ++i)
            free_list.push_back(reinterpret_cast<T*>(&storage_[i]));
    }

    ~MemoryPool() {
        // Raw bytes: nothing is destructed here. Contract: the owner
        // deallocate()s every live object before the pool dies (OrderBook's
        // destructor releases all live orders via order_map).
        delete[] storage_;
    }

    MemoryPool(const MemoryPool&) = delete;             // owns raw storage
    MemoryPool& operator=(const MemoryPool&) = delete;

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
        return new (ptr) T(std::forward<Args>(args)...);   // placement new
    }

    void deallocate(T* ptr) {
        if (!ptr) return;
        auto it = heap_allocated.find(ptr);
        if (it != heap_allocated.end()) {
            delete ptr;                     // overflow object lived on the heap
            heap_allocated.erase(it);
            return;
        }
        ptr->~T();                          // destroy the pooled object once
        free_list.push_back(ptr);           // return the raw slot for reuse
    }

    size_t available() const { return free_list.size(); }
    size_t heap_overflow_count() const { return heap_allocated.size(); }

private:
    // Raw storage sized/aligned for one T, holding no live object until
    // placement-new'd. std::byte (not the C++23-deprecated aligned_storage).
    struct alignas(T) Slot { unsigned char data[sizeof(T)]; };

    Slot* storage_ = nullptr;
    std::vector<T*> free_list;
    std::unordered_set<T*> heap_allocated;
};