// Extensive correctness + performance tests for the WGVK allocator stack.
//
// Three layers are exercised:
//   - VirtualAllocator   : the inner bitmap allocator (3-level summary,
//                          ALLOCATOR_GRANULARITY = 64 bytes per block).
//   - WgvkDeviceMemoryPool / WgvkAllocator (public surface): exercised through
//                          wgvkAllocator_alloc / wgvkAllocator_free, gated on
//                          a real Vulkan-capable adapter being present.
//
// All inner tests are model-checked against an O(N) reference: a flat byte
// vector simulating block occupancy, validated after every op for invariants
// (no overlap, alignment, summary-bit consistency).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include <wgvk.h>
#include <wgvk_structs_impl.h>

// =====================================================================
// Internal allocator function prototypes. These are normally `static` in
// src/wgvk.c but the build system overrides WGVK_ALLOCATOR_INTERNAL_LINKAGE
// to be empty so we can link against them here.
// =====================================================================
extern "C" {
bool   allocator_create (VirtualAllocator* allocator, size_t size);
void   allocator_destroy(VirtualAllocator* allocator);
size_t allocator_alloc  (VirtualAllocator* allocator, size_t size, size_t alignment);
void   allocator_free   (VirtualAllocator* allocator, size_t offset, size_t size);
}

namespace {

// ---------------------------------------------------------------------
// Helpers / invariants
// ---------------------------------------------------------------------

constexpr size_t G = ALLOCATOR_GRANULARITY; // 64 bytes
constexpr size_t W = BITS_PER_WORD;         // 64 bits

static inline size_t blocks_for(size_t size) {
    return (size + G - 1) / G;
}

static inline size_t round_up_pow2(size_t a) {
    if (a <= 1) return 1;
    a--;
    a |= a >> 1;
    a |= a >> 2;
    a |= a >> 4;
    a |= a >> 8;
    a |= a >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    a |= a >> 32;
#endif
    return a + 1;
}

// Check the saturation invariants the allocator now relies on:
//   level1 bit (w%W) in word (w/W)  ==  (level2[w] == ~0ULL)
//   level0 bit (w%W) in word (w/W)  ==  (level1[w] == ~0ULL)
// Tail bits (positions beyond l2_word_count / l1_word_count) must always
// stay set to 1 because they are pre-marked at create time.
::testing::AssertionResult CheckSummaryInvariants(const VirtualAllocator& a) {
    for (size_t w = 0; w < a.l2_word_count; ++w) {
        bool saturated = (a.level2[w] == ~0ULL);
        size_t l1_word = w / W;
        size_t l1_bit  = w % W;
        if (l1_word >= a.l1_word_count) {
            return ::testing::AssertionFailure()
                << "L1 word " << l1_word << " out of range for L2 word " << w;
        }
        bool l1_is_set = ((a.level1[l1_word] >> l1_bit) & 1ULL) != 0;
        if (saturated != l1_is_set) {
            return ::testing::AssertionFailure()
                << "L2[" << w << "]=" << std::hex << a.level2[w] << std::dec
                << " saturated=" << saturated
                << " but L1 bit " << l1_word << ":" << l1_bit
                << " is " << l1_is_set;
        }
    }
    // Tail bits in L1 (positions >= l2_word_count) must be 1.
    for (size_t pos = a.l2_word_count; pos < a.l1_word_count * W; ++pos) {
        size_t l1_word = pos / W;
        size_t l1_bit  = pos % W;
        if (((a.level1[l1_word] >> l1_bit) & 1ULL) == 0) {
            return ::testing::AssertionFailure()
                << "L1 tail bit at position " << pos << " not set";
        }
    }

    for (size_t w = 0; w < a.l1_word_count; ++w) {
        bool saturated = (a.level1[w] == ~0ULL);
        size_t l0_word = w / W;
        size_t l0_bit  = w % W;
        if (l0_word >= a.l0_word_count) {
            return ::testing::AssertionFailure()
                << "L0 word " << l0_word << " out of range";
        }
        bool l0_is_set = ((a.level0[l0_word] >> l0_bit) & 1ULL) != 0;
        if (saturated != l0_is_set) {
            return ::testing::AssertionFailure()
                << "L1[" << w << "]=" << std::hex << a.level1[w] << std::dec
                << " saturated=" << saturated
                << " but L0 bit " << l0_word << ":" << l0_bit
                << " is " << l0_is_set;
        }
    }
    for (size_t pos = a.l1_word_count; pos < a.l0_word_count * W; ++pos) {
        size_t l0_word = pos / W;
        size_t l0_bit  = pos % W;
        if (((a.level0[l0_word] >> l0_bit) & 1ULL) == 0) {
            return ::testing::AssertionFailure()
                << "L0 tail bit at position " << pos << " not set";
        }
    }
    return ::testing::AssertionSuccess();
}

// Reference model: 1 byte per block, 1 = occupied. Used to cross-check
// observable allocator behaviour after each op.
struct RefModel {
    std::vector<uint8_t> occ;
    explicit RefModel(size_t total_blocks) : occ(total_blocks, 0) {}

    void mark(size_t off, size_t size, uint8_t v) {
        size_t start = off / G;
        size_t n     = blocks_for(size);
        for (size_t i = 0; i < n && start + i < occ.size(); ++i)
            occ[start + i] = v;
    }

    bool any_in(size_t start, size_t n) const {
        for (size_t i = 0; i < n && start + i < occ.size(); ++i)
            if (occ[start + i]) return true;
        return false;
    }

    size_t count_used() const {
        size_t s = 0;
        for (auto b : occ) s += (b != 0);
        return s;
    }
};

// Portable 64-bit popcount. MSVC has no __builtin_popcountll.
#if defined(_MSC_VER)
    #include <intrin.h>
    static inline unsigned wgvk_popcount64(uint64_t x) {
        return (unsigned)__popcnt64(x);
    }
#else
    static inline unsigned wgvk_popcount64(uint64_t x) {
        return (unsigned)__builtin_popcountll((unsigned long long)x);
    }
#endif

// Count the occupied valid blocks. Skips tail bits in the last L2 word
// since allocator_create now seeds those to 1 to make saturation logic
// uniform.
size_t BitmapPopcount(const VirtualAllocator& a) {
    size_t c = 0;
    size_t left = a.total_blocks;
    for (size_t w = 0; w < a.l2_word_count; ++w) {
        size_t valid = (left >= W) ? W : left;
        uint64_t mask = (valid == W) ? ~0ULL : ((1ULL << valid) - 1ULL);
        c += static_cast<size_t>(wgvk_popcount64(a.level2[w] & mask));
        left -= valid;
    }
    return c;
}

// Test fixture for the inner allocator. Default size: 64 KiB (1024 blocks).
class VirtualAllocatorTest : public ::testing::Test {
protected:
    VirtualAllocator a{};
    void Init(size_t size) {
        ASSERT_TRUE(allocator_create(&a, size));
        ASSERT_NE(a.level0, nullptr);
        ASSERT_NE(a.level1, nullptr);
        ASSERT_NE(a.level2, nullptr);
        ASSERT_EQ(a.size_in_bytes, size);
        ASSERT_EQ(a.total_blocks, size / G);
    }
    void TearDown() override {
        allocator_destroy(&a);
        // After destroy, struct must be zeroed.
        EXPECT_EQ(a.level0, nullptr);
        EXPECT_EQ(a.level1, nullptr);
        EXPECT_EQ(a.level2, nullptr);
        EXPECT_EQ(a.size_in_bytes, 0u);
        EXPECT_EQ(a.total_blocks, 0u);
    }
};

// =====================================================================
// 1. Lifecycle
// =====================================================================

TEST_F(VirtualAllocatorTest, Lifecycle_TinySize) {
    Init(G); // exactly 1 block
    EXPECT_EQ(a.total_blocks, 1u);
    EXPECT_EQ(a.l2_word_count, 1u);
    EXPECT_EQ(a.l1_word_count, 1u);
    EXPECT_EQ(a.l0_word_count, 1u);
    // Tail bits (positions 1..63 in each level) are pre-set to 1 by
    // allocator_create; only the single valid bit must start cleared.
    EXPECT_EQ(a.level2[0] & 1ULL, 0u);
    EXPECT_EQ(a.level1[0] & 1ULL, 0u);
    EXPECT_EQ(a.level0[0] & 1ULL, 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    // The single valid block must be allocatable + freeable.
    size_t off = allocator_alloc(&a, 1, 1);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(allocator_alloc(&a, 1, 1), OUT_OF_SPACE);
    allocator_free(&a, 0, G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
}

TEST_F(VirtualAllocatorTest, Lifecycle_OneWordWorth) {
    Init(64 * G); // 64 blocks
    EXPECT_EQ(a.total_blocks, 64u);
    EXPECT_EQ(a.l2_word_count, 1u);
}

TEST_F(VirtualAllocatorTest, Lifecycle_LargeSize) {
    Init(64u * 1024 * 1024); // 64 MiB
    EXPECT_EQ(a.total_blocks, (64u * 1024 * 1024) / G);
    EXPECT_EQ(a.l2_word_count,
              (a.total_blocks + W - 1) / W);
    EXPECT_EQ(a.l1_word_count,
              (a.l2_word_count + W - 1) / W);
    EXPECT_EQ(a.l0_word_count,
              (a.l1_word_count + W - 1) / W);
}

TEST_F(VirtualAllocatorTest, Lifecycle_DestroyOnZeroedStructIsSafe) {
    // allocator_destroy must tolerate a zero-initialised struct.
    VirtualAllocator z{};
    allocator_destroy(&z); // must not crash
    EXPECT_EQ(z.level0, nullptr);
}

TEST_F(VirtualAllocatorTest, Lifecycle_DestroyOnNullIsSafe) {
    allocator_destroy(nullptr); // must not crash
}

// =====================================================================
// 2. Edge case sizes
// =====================================================================

TEST_F(VirtualAllocatorTest, Edge_AllocZeroReturnsZeroOffset) {
    Init(64 * G);
    // The implementation contract returns offset 0 for a zero-byte alloc
    // without touching state.
    size_t off = allocator_alloc(&a, 0, 64);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(BitmapPopcount(a), 0u);
}

TEST_F(VirtualAllocatorTest, Edge_AllocSizeOneConsumesSingleBlock) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, 1, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(BitmapPopcount(a), 1u);
}

TEST_F(VirtualAllocatorTest, Edge_AllocOneGranuleConsumesSingleBlock) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(BitmapPopcount(a), 1u);
}

TEST_F(VirtualAllocatorTest, Edge_AllocGranulePlusOneConsumesTwoBlocks) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, G + 1, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(BitmapPopcount(a), 2u);
}

TEST_F(VirtualAllocatorTest, Edge_AllocLargerThanCapacityFails) {
    Init(64 * G);
    EXPECT_EQ(allocator_alloc(&a, 64 * G + 1, 1), OUT_OF_SPACE);
    EXPECT_EQ(BitmapPopcount(a), 0u);
}

TEST_F(VirtualAllocatorTest, Edge_AllocExactlyCapacitySucceeds) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, 64 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(BitmapPopcount(a), 64u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
}

TEST_F(VirtualAllocatorTest, Edge_AllocAfterFullFails) {
    Init(64 * G);
    ASSERT_NE(allocator_alloc(&a, 64 * G, 1), OUT_OF_SPACE);
    EXPECT_EQ(allocator_alloc(&a, 1, 1), OUT_OF_SPACE);
}

TEST_F(VirtualAllocatorTest, Edge_FreeZeroIsNoOp) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    auto before = a.level2[0];
    allocator_free(&a, 0, 0);
    EXPECT_EQ(a.level2[0], before);
}

// =====================================================================
// 3. Alignment
// =====================================================================

TEST_F(VirtualAllocatorTest, Alignment_BelowGranularityRaisedToGranularity) {
    Init(8 * G);
    // alignment 1 must be raised to G internally; offset 0 satisfies both.
    size_t off1 = allocator_alloc(&a, 1, 1);
    ASSERT_NE(off1, OUT_OF_SPACE);
    EXPECT_EQ(off1 % G, 0u);
}

TEST_F(VirtualAllocatorTest, Alignment_NonPowerOfTwoRoundsUp) {
    Init(64 * G);
    // alignment 100 -> 128
    size_t off1 = allocator_alloc(&a, 1, 1);
    ASSERT_EQ(off1, 0u);
    size_t off2 = allocator_alloc(&a, 1, 100);
    ASSERT_NE(off2, OUT_OF_SPACE);
    EXPECT_EQ(off2 % 128, 0u);
}

TEST_F(VirtualAllocatorTest, Alignment_Power2Alignments) {
    Init(1024 * G);
    for (size_t align : {(size_t)64, (size_t)128, (size_t)256, (size_t)512,
                         (size_t)1024, (size_t)2048, (size_t)4096}) {
        size_t off = allocator_alloc(&a, 1, align);
        ASSERT_NE(off, OUT_OF_SPACE) << "align=" << align;
        EXPECT_EQ(off % align, 0u) << "align=" << align;
    }
}

TEST_F(VirtualAllocatorTest, Alignment_LargerThanCapacityFails) {
    Init(64 * G);
    // Offset 0 satisfies any alignment, so we must occupy block 0 first.
    size_t a0 = allocator_alloc(&a, G, 1);
    ASSERT_EQ(a0, 0u);
    // Now an alignment of 1 MiB cannot be satisfied anywhere inside a
    // 4 KiB allocator: there is no other 1 MiB-aligned offset in range.
    EXPECT_EQ(allocator_alloc(&a, 1, 1024 * 1024), OUT_OF_SPACE);
}

TEST_F(VirtualAllocatorTest, Alignment_AlignmentEqualsCapacity) {
    Init(64 * G);
    // Single offset 0 satisfies an alignment equal to capacity.
    size_t off = allocator_alloc(&a, 1, 64 * G);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
}

TEST_F(VirtualAllocatorTest, Alignment_HighestPow2WithinRange) {
    Init(8 * G);
    size_t off = allocator_alloc(&a, G, 8 * G);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(off, 0u);
}

TEST_F(VirtualAllocatorTest, Alignment_BlockAfterUnalignedConsumesAlignmentSlot) {
    Init(8 * G);
    // First two single-block allocs at offset 0 and G.
    size_t a1 = allocator_alloc(&a, 1, 1);
    size_t a2 = allocator_alloc(&a, 1, 1);
    ASSERT_EQ(a1, 0u);
    ASSERT_EQ(a2, G);
    // Alignment 4*G forces the next alloc to skip to offset 4*G.
    size_t a3 = allocator_alloc(&a, 1, 4 * G);
    ASSERT_NE(a3, OUT_OF_SPACE);
    EXPECT_EQ(a3, 4 * G);
}

// =====================================================================
// 4. Free behaviour
// =====================================================================

TEST_F(VirtualAllocatorTest, Free_AllowsReuse) {
    Init(64 * G);
    size_t a1 = allocator_alloc(&a, G, 1);
    ASSERT_EQ(a1, 0u);
    allocator_free(&a, a1, G);
    size_t a2 = allocator_alloc(&a, G, 1);
    EXPECT_EQ(a2, 0u);
}

TEST_F(VirtualAllocatorTest, Free_ClearsBitsAfterRelease) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, 4 * G, 1);
    ASSERT_EQ(BitmapPopcount(a), 4u);
    allocator_free(&a, off, 4 * G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
    // The single L2 word was never saturated (only 4 bits got set), so its
    // L1 saturation bit was never set in the first place.
    EXPECT_EQ(a.level1[0] & 1ULL, 0u);
    EXPECT_EQ(a.level0[0] & 1ULL, 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
}

TEST_F(VirtualAllocatorTest, Free_KeepsOtherAllocationsIntact) {
    Init(64 * G);
    size_t a1 = allocator_alloc(&a, 2 * G, 1);
    size_t a2 = allocator_alloc(&a, 2 * G, 1);
    size_t a3 = allocator_alloc(&a, 2 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    ASSERT_NE(a2, OUT_OF_SPACE);
    ASSERT_NE(a3, OUT_OF_SPACE);
    allocator_free(&a, a2, 2 * G);

    // 4 blocks remain occupied (the first and third).
    EXPECT_EQ(BitmapPopcount(a), 4u);

    // The freed range fits a same-sized allocation again.
    size_t a4 = allocator_alloc(&a, 2 * G, 1);
    EXPECT_EQ(a4, a2);
}

TEST_F(VirtualAllocatorTest, Free_MultiWordSpanCollapsesSummary) {
    Init(4096 * G); // 4096 blocks => 64 L2 words => 1 L1 word
    // Allocate 70 blocks: spans more than one L2 word.
    size_t off = allocator_alloc(&a, 70 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    allocator_free(&a, off, 70 * G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
    // After draining: every L2 word zero, every valid L1 saturation bit
    // zero, every valid L0 saturation bit zero. (L0 bit 0 is the only
    // valid one here; bits 1..63 are tail bits left at 1 by init.)
    for (size_t i = 0; i < a.l2_word_count; ++i) EXPECT_EQ(a.level2[i], 0u);
    EXPECT_EQ(a.level1[0], 0u);
    EXPECT_EQ(a.level0[0] & 1ULL, 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
}

// =====================================================================
// 5. Multi-word & multi-level spans
// =====================================================================

TEST_F(VirtualAllocatorTest, Span_AllocSpanningMultipleL2Words) {
    Init(256 * G); // 4 L2 words, 1 L1 word
    size_t off = allocator_alloc(&a, 200 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(BitmapPopcount(a), 200u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
}

TEST_F(VirtualAllocatorTest, Span_AllocSpanningMultipleL1Words) {
    // Need >= 2 L1 words: l1_word_count > 1 => l2_word_count > 64
    // => total_blocks > 64*64=4096 blocks => size > 4096*G = 256 KiB.
    Init(8192 * G); // 8192 blocks, 128 L2 words, 2 L1 words
    size_t off = allocator_alloc(&a, 5000 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(BitmapPopcount(a), 5000u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    EXPECT_NE(a.level0[0], 0u);
}

TEST_F(VirtualAllocatorTest, Span_FillEntireAllocator_ManySmall) {
    Init(64 * G);
    std::vector<size_t> offs;
    for (int i = 0; i < 64; ++i) {
        size_t o = allocator_alloc(&a, 1, 1);
        ASSERT_NE(o, OUT_OF_SPACE) << "i=" << i;
        offs.push_back(o);
    }
    // No two offsets repeat, all unique block indices.
    std::set<size_t> uniq(offs.begin(), offs.end());
    EXPECT_EQ(uniq.size(), 64u);
    EXPECT_EQ(allocator_alloc(&a, 1, 1), OUT_OF_SPACE);
}

TEST_F(VirtualAllocatorTest, Span_FillEntireAllocator_OneBig) {
    Init(64 * G);
    size_t off = allocator_alloc(&a, 64 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    EXPECT_EQ(BitmapPopcount(a), 64u);
}

// =====================================================================
// 6. Fragmentation
// =====================================================================

TEST_F(VirtualAllocatorTest, Frag_HoleTooSmallFails) {
    Init(64 * G);
    // [4][4][4][...]
    size_t a1 = allocator_alloc(&a, 4 * G, 1);
    size_t a2 = allocator_alloc(&a, 4 * G, 1);
    size_t a3 = allocator_alloc(&a, 4 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    ASSERT_NE(a2, OUT_OF_SPACE);
    ASSERT_NE(a3, OUT_OF_SPACE);
    // Free the middle => 4-block hole.
    allocator_free(&a, a2, 4 * G);
    // 5 blocks won't fit in a 4-block hole. Plenty of space afterwards
    // though, so it must succeed at the tail.
    size_t a4 = allocator_alloc(&a, 5 * G, 1);
    ASSERT_NE(a4, OUT_OF_SPACE);
    EXPECT_GE(a4, a3 + 4 * G);
}

TEST_F(VirtualAllocatorTest, Frag_HoleExactlyFits) {
    Init(64 * G);
    size_t a1 = allocator_alloc(&a, 4 * G, 1);
    size_t a2 = allocator_alloc(&a, 4 * G, 1);
    size_t a3 = allocator_alloc(&a, 4 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    ASSERT_NE(a3, OUT_OF_SPACE);
    allocator_free(&a, a2, 4 * G);
    size_t a4 = allocator_alloc(&a, 4 * G, 1);
    EXPECT_EQ(a4, a2);
}

TEST_F(VirtualAllocatorTest, Frag_FirstFit) {
    Init(64 * G);
    // [oc][..][oc][...] then alloc 1 block: must take the first hole.
    size_t a1 = allocator_alloc(&a, G, 1);
    size_t a2 = allocator_alloc(&a, 2 * G, 1);
    size_t a3 = allocator_alloc(&a, G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    ASSERT_NE(a2, OUT_OF_SPACE);
    ASSERT_NE(a3, OUT_OF_SPACE);
    allocator_free(&a, a2, 2 * G);
    size_t a4 = allocator_alloc(&a, G, 1);
    EXPECT_EQ(a4, a2);
}

TEST_F(VirtualAllocatorTest, Frag_AlternatingPattern) {
    Init(64 * G);
    std::vector<size_t> kept, freed;
    for (int i = 0; i < 64; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE);
        if (i & 1) freed.push_back(o); else kept.push_back(o);
    }
    EXPECT_EQ(BitmapPopcount(a), 64u);
    for (size_t o : freed) allocator_free(&a, o, G);

    // Half the allocator is free again.
    EXPECT_EQ(BitmapPopcount(a), 32u);

    // Refill them — must succeed for 32 single-block allocations.
    for (size_t i = 0; i < freed.size(); ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE) << "refill i=" << i;
    }
    EXPECT_EQ(BitmapPopcount(a), 64u);
    EXPECT_EQ(allocator_alloc(&a, G, 1), OUT_OF_SPACE);
}

TEST_F(VirtualAllocatorTest, Frag_BigAllocFailsThroughFragmentation) {
    Init(64 * G);
    // Mark every 4th block by an alloc that leaves 3-block holes.
    std::vector<size_t> markers;
    for (int i = 0; i < 16; ++i) {
        size_t o = allocator_alloc(&a, G, 1);          // 1 block alloc
        size_t s = allocator_alloc(&a, 3 * G, 1);      // 3 block alloc
        ASSERT_NE(o, OUT_OF_SPACE);
        ASSERT_NE(s, OUT_OF_SPACE);
        markers.push_back(o);
        markers.push_back(s);
    }
    EXPECT_EQ(BitmapPopcount(a), 64u);
    // Free only the 1-block markers. Pattern: [F][3OC][F][3OC]...
    for (size_t i = 0; i < markers.size(); i += 2) {
        allocator_free(&a, markers[i], G);
    }
    EXPECT_EQ(BitmapPopcount(a), 48u);
    // No room for a 2-block contiguous alloc.
    EXPECT_EQ(allocator_alloc(&a, 2 * G, 1), OUT_OF_SPACE);
    // 1-block allocs fit fine.
    size_t small = allocator_alloc(&a, G, 1);
    EXPECT_NE(small, OUT_OF_SPACE);
}

// =====================================================================
// 7. Determinism / repeatability
// =====================================================================

TEST_F(VirtualAllocatorTest, Determinism_SameSequenceSameOffsets) {
    auto run = [](std::vector<size_t>& out) {
        VirtualAllocator t{};
        ASSERT_TRUE(allocator_create(&t, 64 * G));
        for (int i = 0; i < 16; ++i) {
            out.push_back(allocator_alloc(&t, (i + 1) * G / 4 + G, 1));
        }
        allocator_destroy(&t);
    };
    std::vector<size_t> o1, o2;
    run(o1);
    run(o2);
    EXPECT_EQ(o1, o2);
}

// =====================================================================
// 8. Whole-allocator invariant under random workloads
// =====================================================================

TEST(VirtualAllocatorRandom, ChurnInvariantsHold) {
    constexpr size_t kSize = 1u << 20; // 1 MiB
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, kSize));
    RefModel ref(kSize / G);

    struct Live { size_t off, size; };
    std::vector<Live> live;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int>     coin(0, 99);
    std::uniform_int_distribution<size_t>  size_dist(1, 256); // bytes
    std::uniform_int_distribution<size_t>  align_dist(0, 12);

    for (int it = 0; it < 5000; ++it) {
        bool do_free = !live.empty() && coin(rng) < 40;
        if (do_free) {
            std::uniform_int_distribution<size_t> idx(0, live.size() - 1);
            size_t i = idx(rng);
            allocator_free(&a, live[i].off, live[i].size);
            ref.mark(live[i].off, live[i].size, 0);
            live[i] = live.back();
            live.pop_back();
        } else {
            size_t sz    = size_dist(rng) * G;
            size_t align = (size_t)1 << align_dist(rng);
            size_t off   = allocator_alloc(&a, sz, align);
            if (off == OUT_OF_SPACE) continue;

            // Alignment respected (after raising to G and rounding up to pow2).
            size_t expected_align = std::max<size_t>(G, round_up_pow2(align));
            ASSERT_EQ(off % expected_align, 0u)
                << "align=" << align << " expected_align=" << expected_align
                << " off=" << off;

            // No overlap with existing allocations.
            size_t blk = off / G;
            size_t n   = blocks_for(sz);
            ASSERT_FALSE(ref.any_in(blk, n))
                << "overlap at off=" << off << " size=" << sz;
            ref.mark(off, sz, 1);
            live.push_back({off, sz});
        }
        // Check summary invariants every 50 iterations to keep test fast.
        if ((it & 49) == 0) {
            ASSERT_TRUE(CheckSummaryInvariants(a));
        }
    }

    // Drain.
    for (auto& l : live) {
        allocator_free(&a, l.off, l.size);
        ref.mark(l.off, l.size, 0);
    }
    EXPECT_EQ(BitmapPopcount(a), 0u);
    EXPECT_EQ(ref.count_used(), 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    allocator_destroy(&a);
}

TEST(VirtualAllocatorRandom, ChurnInvariantsHold_LargeAlignments) {
    constexpr size_t kSize = 4u * 1024 * 1024;
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, kSize));
    RefModel ref(kSize / G);

    struct Live { size_t off, size; };
    std::vector<Live> live;

    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int>     coin(0, 99);
    std::uniform_int_distribution<size_t>  size_dist(1, 4096);
    std::uniform_int_distribution<size_t>  align_pow(0, 16); // up to 64 KiB

    for (int it = 0; it < 4000; ++it) {
        bool do_free = !live.empty() && coin(rng) < 45;
        if (do_free) {
            std::uniform_int_distribution<size_t> idx(0, live.size() - 1);
            size_t i = idx(rng);
            allocator_free(&a, live[i].off, live[i].size);
            ref.mark(live[i].off, live[i].size, 0);
            live[i] = live.back();
            live.pop_back();
        } else {
            size_t sz    = size_dist(rng);
            size_t align = (size_t)1 << align_pow(rng);
            size_t off   = allocator_alloc(&a, sz, align);
            if (off == OUT_OF_SPACE) continue;
            size_t expected_align = std::max<size_t>(G, round_up_pow2(align));
            ASSERT_EQ(off % expected_align, 0u);
            size_t blk = off / G;
            size_t n   = blocks_for(sz);
            ASSERT_FALSE(ref.any_in(blk, n));
            ref.mark(off, sz, 1);
            live.push_back({off, sz});
        }
    }
    for (auto& l : live) {
        allocator_free(&a, l.off, l.size);
        ref.mark(l.off, l.size, 0);
    }
    EXPECT_EQ(BitmapPopcount(a), 0u);
    allocator_destroy(&a);
}

// =====================================================================
// 9. Reverse / random free order
// =====================================================================

TEST(VirtualAllocatorOrder, ReverseFreeRefillsBackwards) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 64 * G));
    std::vector<size_t> offs;
    for (int i = 0; i < 64; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE);
        offs.push_back(o);
    }
    // Free in reverse.
    for (auto it = offs.rbegin(); it != offs.rend(); ++it)
        allocator_free(&a, *it, G);
    EXPECT_EQ(BitmapPopcount(a), 0u);

    // After draining, first-fit must return offset 0 again.
    EXPECT_EQ(allocator_alloc(&a, G, 1), 0u);
    allocator_destroy(&a);
}

TEST(VirtualAllocatorOrder, RandomFreeOrderDrains) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 1024 * G));
    std::vector<size_t> offs;
    for (int i = 0; i < 256; ++i) {
        size_t o = allocator_alloc(&a, 4 * G, 1);
        ASSERT_NE(o, OUT_OF_SPACE);
        offs.push_back(o);
    }
    std::mt19937 rng(123);
    std::shuffle(offs.begin(), offs.end(), rng);
    for (auto o : offs) allocator_free(&a, o, 4 * G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    allocator_destroy(&a);
}

// =====================================================================
// 10. Boundary tests against last_start_block
// =====================================================================

TEST(VirtualAllocatorBoundary, AllocAtTailBlock) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 64 * G));
    // Fill all but the last block.
    size_t a1 = allocator_alloc(&a, 63 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    // The remaining slot is exactly 1 block, at offset 63 * G.
    size_t a2 = allocator_alloc(&a, G, 1);
    EXPECT_EQ(a2, 63 * G);
    EXPECT_EQ(allocator_alloc(&a, G, 1), OUT_OF_SPACE);
    allocator_destroy(&a);
}

TEST(VirtualAllocatorBoundary, MultiBlockAtTail) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 64 * G));
    size_t a1 = allocator_alloc(&a, 60 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    size_t a2 = allocator_alloc(&a, 4 * G, 1);
    ASSERT_NE(a2, OUT_OF_SPACE);
    EXPECT_EQ(a2, 60 * G);
    EXPECT_EQ(allocator_alloc(&a, G, 1), OUT_OF_SPACE);
    allocator_destroy(&a);
}

TEST(VirtualAllocatorBoundary, MultiWordBoundaryAlloc) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 192 * G)); // 3 L2 words
    // Allocate enough to leave just 65 free at the end (crossing word boundary).
    size_t a1 = allocator_alloc(&a, 127 * G, 1);
    ASSERT_NE(a1, OUT_OF_SPACE);
    size_t a2 = allocator_alloc(&a, 65 * G, 1);
    ASSERT_NE(a2, OUT_OF_SPACE);
    EXPECT_EQ(a2, 127 * G);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    allocator_destroy(&a);
}

// =====================================================================
// 11. Stress / scaling
// =====================================================================

TEST(VirtualAllocatorStress, ManyAllocsSmall) {
    VirtualAllocator a{};
    constexpr size_t blocks = 64 * 1024; // 64 K blocks
    ASSERT_TRUE(allocator_create(&a, blocks * G));
    std::vector<size_t> offs;
    offs.reserve(blocks);
    for (size_t i = 0; i < blocks; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE) << "i=" << i;
        offs.push_back(o);
    }
    EXPECT_EQ(allocator_alloc(&a, G, 1), OUT_OF_SPACE);
    EXPECT_EQ(BitmapPopcount(a), blocks);

    // Drain in same order; verify all bits clear.
    for (size_t o : offs) allocator_free(&a, o, G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
    EXPECT_TRUE(CheckSummaryInvariants(a));
    allocator_destroy(&a);
}

TEST(VirtualAllocatorStress, ScatteredFreeRebuildsAvailability) {
    VirtualAllocator a{};
    constexpr size_t blocks = 1024;
    ASSERT_TRUE(allocator_create(&a, blocks * G));

    std::vector<size_t> offs;
    for (size_t i = 0; i < blocks; ++i)
        offs.push_back(allocator_alloc(&a, G, 1));

    // Free every prime-index allocation (sparse scatter).
    auto is_prime = [](size_t n) {
        if (n < 2) return false;
        for (size_t k = 2; k * k <= n; ++k) if (n % k == 0) return false;
        return true;
    };
    size_t freed = 0;
    for (size_t i = 0; i < offs.size(); ++i) {
        if (is_prime(i)) { allocator_free(&a, offs[i], G); freed++; }
    }
    EXPECT_EQ(BitmapPopcount(a), blocks - freed);
    // Every freed slot should be re-allocatable to a 1-block alloc.
    for (size_t i = 0; i < freed; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE) << "refill i=" << i;
    }
    EXPECT_EQ(BitmapPopcount(a), blocks);
    EXPECT_EQ(allocator_alloc(&a, G, 1), OUT_OF_SPACE);
    allocator_destroy(&a);
}

// =====================================================================
// 12. Performance / throughput (microbenchmarks). These are not
//      strict bounds - they just print numbers and assert "completes".
// =====================================================================

class AllocatorPerf : public ::testing::Test {};

TEST_F(AllocatorPerf, Perf_SmallAllocFree_LIFO) {
    VirtualAllocator a{};
    constexpr size_t blocks = 64 * 1024;
    ASSERT_TRUE(allocator_create(&a, blocks * G));
    constexpr int iterations = 200000;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<size_t> stack;
    stack.reserve(1024);
    for (int i = 0; i < iterations; ++i) {
        if (stack.size() < 512) {
            size_t o = allocator_alloc(&a, G, 1);
            if (o != OUT_OF_SPACE) stack.push_back(o);
        } else {
            allocator_free(&a, stack.back(), G);
            stack.pop_back();
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::printf("[perf] LIFO 1-block: %d ops in %lld us  (%.2f ns/op)\n",
                iterations, (long long)us, us * 1000.0 / iterations);
    while (!stack.empty()) { allocator_free(&a, stack.back(), G); stack.pop_back(); }
    allocator_destroy(&a);
}

TEST_F(AllocatorPerf, Perf_SequentialFillThenDrain) {
    VirtualAllocator a{};
    constexpr size_t blocks = 64 * 1024;
    ASSERT_TRUE(allocator_create(&a, blocks * G));
    std::vector<size_t> offs;
    offs.reserve(blocks);

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < blocks; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        ASSERT_NE(o, OUT_OF_SPACE);
        offs.push_back(o);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (auto o : offs) allocator_free(&a, o, G);
    auto t2 = std::chrono::steady_clock::now();
    auto us_alloc = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto us_free  = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    std::printf("[perf] fill %zu blocks: alloc=%lld us (%.2f ns/op) free=%lld us (%.2f ns/op)\n",
                (size_t)blocks, (long long)us_alloc,
                us_alloc * 1000.0 / blocks,
                (long long)us_free,
                us_free * 1000.0 / blocks);
    allocator_destroy(&a);
}

TEST_F(AllocatorPerf, Perf_FragmentedFirstFit) {
    // First-fit walks from offset 0 each call; this is the worst case for
    // the scan loop. Measure that it stays bounded for a heavily fragmented
    // allocator.
    VirtualAllocator a{};
    constexpr size_t blocks = 4096;
    ASSERT_TRUE(allocator_create(&a, blocks * G));

    // Build a fragmented state: alloc 1, alloc 2, free the 1s.
    std::vector<size_t> ones;
    std::vector<size_t> twos;
    for (size_t i = 0; i < blocks / 3; ++i) {
        size_t a1 = allocator_alloc(&a, G, 1);
        size_t a2 = allocator_alloc(&a, 2 * G, 1);
        if (a1 == OUT_OF_SPACE || a2 == OUT_OF_SPACE) break;
        ones.push_back(a1);
        twos.push_back(a2);
    }
    for (auto o : ones) allocator_free(&a, o, G);

    constexpr int iterations = 20000;
    auto t0 = std::chrono::steady_clock::now();
    int hits = 0;
    for (int i = 0; i < iterations; ++i) {
        size_t o = allocator_alloc(&a, G, 1);
        if (o != OUT_OF_SPACE) {
            hits++;
            allocator_free(&a, o, G);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::printf("[perf] fragmented first-fit: %d ops in %lld us (%.2f ns/op, %d hits)\n",
                iterations, (long long)us, us * 1000.0 / iterations, hits);
    for (auto o : twos) allocator_free(&a, o, 2 * G);
    allocator_destroy(&a);
}

TEST_F(AllocatorPerf, Perf_RandomMix) {
    constexpr size_t kSize = 8u * 1024 * 1024;
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, kSize));

    struct Live { size_t off, size; };
    std::vector<Live> live;
    live.reserve(8192);

    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<int>    coin(0, 99);
    std::uniform_int_distribution<size_t> size_dist(1, 1024); // bytes

    constexpr int iterations = 50000;
    auto t0 = std::chrono::steady_clock::now();
    int allocs = 0, frees = 0;
    for (int i = 0; i < iterations; ++i) {
        if (!live.empty() && coin(rng) < 40) {
            std::uniform_int_distribution<size_t> idx(0, live.size() - 1);
            size_t k = idx(rng);
            allocator_free(&a, live[k].off, live[k].size);
            live[k] = live.back();
            live.pop_back();
            frees++;
        } else {
            size_t sz = size_dist(rng);
            size_t off = allocator_alloc(&a, sz, 1);
            if (off != OUT_OF_SPACE) {
                live.push_back({off, sz});
                allocs++;
            }
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::printf("[perf] random mix: %d ops in %lld us (%.2f ns/op, %d alloc / %d free)\n",
                iterations, (long long)us, us * 1000.0 / iterations,
                allocs, frees);
    for (auto& l : live) allocator_free(&a, l.off, l.size);
    allocator_destroy(&a);
}

// =====================================================================
// 13. Full-suite invariants in a separate fixture (TEST, not _F)
// =====================================================================

TEST(VirtualAllocatorMisc, ZeroSizeAllocatorIsCallable) {
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 0));
    EXPECT_EQ(a.total_blocks, 0u);
    // Any non-zero alloc must fail gracefully.
    EXPECT_EQ(allocator_alloc(&a, 1, 1), OUT_OF_SPACE);
    // Zero-byte alloc still returns 0 (no state change).
    EXPECT_EQ(allocator_alloc(&a, 0, 1), 0u);
    allocator_destroy(&a);
}

TEST(VirtualAllocatorMisc, PartialFreeIgnoresOutOfRange) {
    // free(...) clamps via the `current_block >= total_blocks` check; an
    // out-of-range offset/size combination must not corrupt memory.
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 64 * G));
    size_t off = allocator_alloc(&a, 4 * G, 1);
    ASSERT_NE(off, OUT_OF_SPACE);
    // Free with a size that overshoots — the loop breaks at the boundary.
    allocator_free(&a, off, 1024 * G);
    EXPECT_EQ(BitmapPopcount(a), 0u);
    allocator_destroy(&a);
}

TEST(VirtualAllocatorMisc, AllocAlignmentBetweenWords) {
    // Force an alignment that lands in the middle of an L2 word.
    VirtualAllocator a{};
    ASSERT_TRUE(allocator_create(&a, 256 * G));
    // Block 1 gets occupied so block 0 is unavailable for an align-block-2.
    size_t a1 = allocator_alloc(&a, G, 1);
    ASSERT_EQ(a1, 0u);
    // Now request a 1-block alloc aligned to 32 blocks: must skip to block 32.
    size_t a2 = allocator_alloc(&a, G, 32 * G);
    ASSERT_NE(a2, OUT_OF_SPACE);
    EXPECT_EQ(a2, 32 * G);
    allocator_destroy(&a);
}

// =====================================================================
// 14. Public surface: WgvkAllocator_* end-to-end
//      Skipped automatically when no Vulkan adapter is available.
// =====================================================================

class WgvkAllocatorDeviceTest : public ::testing::Test {
protected:
    WGPUInstance instance = nullptr;
    WGPUAdapter  adapter  = nullptr;
    WGPUDevice   device   = nullptr;

    void SetUp() override {
        WGPUInstanceDescriptor desc{};
        instance = wgpuCreateInstance(&desc);
        if (!instance) GTEST_SKIP() << "Cannot create WGPUInstance";

        WGPURequestAdapterOptions opts{};
        opts.powerPreference = WGPUPowerPreference_HighPerformance;
        struct Ctx { WGPUAdapter a = nullptr; bool done = false; } ctx;
        WGPURequestAdapterCallbackInfo cbi{};
        cbi.callback = [](WGPURequestAdapterStatus s, WGPUAdapter a,
                          WGPUStringView, void* u, void*) {
            auto* c = static_cast<Ctx*>(u);
            if (s == WGPURequestAdapterStatus_Success) c->a = a;
            c->done = true;
        };
        cbi.userdata1 = &ctx;
        WGPUFuture f = wgpuInstanceRequestAdapter(instance, &opts, cbi);
        WGPUFutureWaitInfo wi{f, 0};
        for (int tries = 0; tries < 5 && !ctx.done; ++tries)
            wgpuInstanceWaitAny(instance, 1, &wi, 1000000000);
        if (!ctx.done || !ctx.a) {
            GTEST_SKIP() << "No Vulkan adapter available; skipping device-backed allocator tests";
        }
        adapter = ctx.a;

        struct DCtx { WGPUDevice d = nullptr; bool done = false; } dctx;
        WGPUDeviceDescriptor ddesc{};
        WGPURequestDeviceCallbackInfo dcbi{};
        dcbi.callback = [](WGPURequestDeviceStatus s, WGPUDevice d,
                           WGPUStringView, void* u, void*) {
            auto* c = static_cast<DCtx*>(u);
            if (s == WGPURequestDeviceStatus_Success) c->d = d;
            c->done = true;
        };
        dcbi.userdata1 = &dctx;
        WGPUFuture df = wgpuAdapterRequestDevice(adapter, &ddesc, dcbi);
        WGPUFutureWaitInfo dwi{df, 0};
        for (int tries = 0; tries < 5 && !dctx.done; ++tries)
            wgpuInstanceWaitAny(instance, 1, &dwi, 1000000000);
        if (!dctx.done || !dctx.d) {
            GTEST_SKIP() << "Failed to obtain WGPUDevice";
        }
        device = dctx.d;
    }

    void TearDown() override {
        if (device)   wgpuDeviceRelease(device);
        if (adapter)  wgpuAdapterRelease(adapter);
        if (instance) wgpuInstanceRelease(instance);
    }

    // Build a synthetic VkMemoryRequirements that matches what most adapters
    // expose for buffer-like allocations.
    VkMemoryRequirements MakeReq(VkDeviceSize size, VkDeviceSize align) const {
        VkMemoryRequirements r{};
        r.size = size;
        r.alignment = align;
        // All visible memory types — let the allocator pick.
        r.memoryTypeBits = 0xFFFFFFFFu;
        return r;
    }
};

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_SimpleAllocFree) {
    WgvkAllocator& alloc = device->builtinAllocator;

    auto req = MakeReq(4096, 16);
    wgvkAllocation a1{};
    ASSERT_TRUE(wgvkAllocator_alloc(&alloc, &req, 0, &a1));
    EXPECT_NE(a1.memory, VK_NULL_HANDLE);
    EXPECT_NE(a1.pool, nullptr);
    EXPECT_EQ(a1.size, (size_t)4096);
    wgvkAllocator_free(&a1);
}

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_RepeatedAllocsReuseChunks) {
    WgvkAllocator& alloc = device->builtinAllocator;
    constexpr int N = 512;
    std::vector<wgvkAllocation> live(N);
    auto req = MakeReq(64 * 1024, 256);
    int ok = 0;
    for (int i = 0; i < N; ++i) {
        if (wgvkAllocator_alloc(&alloc, &req, 0, &live[ok])) ok++;
    }
    ASSERT_GT(ok, 0);
    // The pool count must be small; chunk sizes grow geometrically so a
    // 32 MiB working set should fit in a handful of chunks per pool.
    uint32_t total_chunks = 0;
    for (uint32_t p = 0; p < alloc.pool_count; ++p)
        total_chunks += alloc.pools[p].chunk_count;
    EXPECT_LT(total_chunks, 32u);
    for (int i = 0; i < ok; ++i) wgvkAllocator_free(&live[i]);
}

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_AlignmentBelowGranularityRespected) {
    WgvkAllocator& alloc = device->builtinAllocator;
    auto req = MakeReq(1, 1); // ridiculous, allocator must round up.
    wgvkAllocation a1{};
    ASSERT_TRUE(wgvkAllocator_alloc(&alloc, &req, 0, &a1));
    // Internal contract: offset is at least 64-aligned (G).
    EXPECT_EQ(a1.offset % ALLOCATOR_GRANULARITY, 0u);
    wgvkAllocator_free(&a1);
}

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_ImpossibleMemoryTypeFails) {
    WgvkAllocator& alloc = device->builtinAllocator;
    VkMemoryRequirements r{};
    r.size = 1024;
    r.alignment = 1;
    r.memoryTypeBits = 0; // no type acceptable -> must fail
    wgvkAllocation a1{};
    EXPECT_FALSE(wgvkAllocator_alloc(&alloc, &r, 0, &a1));
}

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_LargeAllocationGrowsChunk) {
    WgvkAllocator& alloc = device->builtinAllocator;
    auto req = MakeReq(MIN_CHUNK_SIZE, 1024);
    wgvkAllocation a1{};
    ASSERT_TRUE(wgvkAllocator_alloc(&alloc, &req, 0, &a1));
    EXPECT_GE(a1.pool->chunks[a1.chunk_index].allocator.size_in_bytes,
              MIN_CHUNK_SIZE);
    wgvkAllocator_free(&a1);
}

TEST_F(WgvkAllocatorDeviceTest, PublicAPI_FreeAllowsReuse) {
    WgvkAllocator& alloc = device->builtinAllocator;
    auto req = MakeReq(8 * 1024, 256);

    wgvkAllocation a1{}, a2{};
    ASSERT_TRUE(wgvkAllocator_alloc(&alloc, &req, 0, &a1));
    auto first_off = a1.offset;
    auto first_mem = a1.memory;
    wgvkAllocator_free(&a1);
    ASSERT_TRUE(wgvkAllocator_alloc(&alloc, &req, 0, &a2));
    // After freeing, a same-size+alignment alloc should land in the same
    // pool/chunk and (most likely) at the same offset.
    EXPECT_EQ(a2.memory, first_mem);
    EXPECT_EQ(a2.offset, first_off);
    wgvkAllocator_free(&a2);
}

} // namespace
