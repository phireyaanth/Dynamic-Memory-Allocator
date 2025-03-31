🧠 Dynamic Memory Allocator – CSE 320 HW3
This project is a custom dynamic memory allocator written in C for x86-64 Linux systems, built as part of the CSE 320 course at Stony Brook University.

✨ Features
🔹 Segregated Free Lists: Blocks are organized by size classes for fast first-fit allocation.

🔹 Quick Lists: Small, frequently freed blocks are cached for rapid reuse.

🔹 Immediate Coalescing: Adjacent free blocks are merged instantly upon deallocation.

🔹 Block Splitting: Larger blocks are split to minimize wasted space—no splinters allowed.

🔹 16-byte Alignment: Ensures proper alignment for all allocations.

🔹 Header/Footer Obfuscation: Uses a MAGIC constant with XOR to detect invalid accesses.

🔹 Prologue/Epilogue Blocks: Eliminates edge-case logic during heap traversal.

🔹 Circular, Doubly Linked Free Lists: Maintained in LIFO order for faster insertion/removal.

🔹 Internal Fragmentation Tracking: Measures how efficiently memory is utilized.

📊 Metrics
sf_fragmentation(): Reports current internal fragmentation.

sf_utilization(): Tracks peak memory utilization over time.

🧪 Testing
✅ Criterion Unit Tests for malloc, free, realloc, coalescing, alignment, quick list flushing, and edge-case handling.

🛠 Custom tests written in sfmm_tests.c to ensure correctness and performance under stress.

🚫 Restrictions
❌ No use of malloc, free, realloc, or similar standard library functions.

✅ All memory is managed using sf_mem_grow()—simulates sbrk for heap extension.

🔐 Must pass with obfuscated MAGIC values (e.g., 0x0 vs randomized).
