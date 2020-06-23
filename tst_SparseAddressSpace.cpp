#define CATCH_CONFIG_MAIN
#include "external/Catch2/single_include/catch2/catch.hpp"

#include <numeric>

#include "SparseAddressSpace.h"

static constexpr int s_minsegsize = 5;
using SAS = SparseAddressSpace<uint32_t>;
using Seg = SAS::Segment;
using SegSPtr = std::shared_ptr<Seg>;

SegSPtr createSegment(uint32_t start, size_t size, int value) {
    auto s = std::make_shared<Seg>();
    s->data = std::vector<uint8_t>(size, value);
    s->start = start;
    return s;
}

void addSegment(SAS& sas, uint32_t start, size_t size, int value) {
    sas.insertSegment(*createSegment(start, size, value));
}

void verifySegment(SAS::SegWPtr seg, uint32_t start, std::vector<std::pair<int, int>> expected) {
    REQUIRE(!seg.expired());
    REQUIRE(seg.lock()->start == start);

    auto& data = seg.lock()->data;
    size_t i = 0;
    while (!expected.empty()) {
        std::pair<int, int> verifying = expected[0];
        expected.erase(expected.begin());

        int cnt = 0;
        while (cnt++ < verifying.second) {
            if (i >= data.size()) {
                std::string failString = "Segment smaller than expected";
                FAIL(failString);
            }
            if (data[i] != verifying.first) {
                std::string failString = "Segment verification failed. i: " + std::to_string(i) +
                                         "\texpected: " + std::to_string(verifying.first) +
                                         "\tbut got: " + std::to_string(data[i]);
                FAIL(failString);
            }
            i++;
        }
    }
}

SAS::SegWPtr getExpectedSingleSegment(SAS& sas) {
    auto segs = sas.segments();
    REQUIRE(segs.size() == 1);
    return segs[0];
}

SAS::SegWPtr getSegmentAtAddr(SAS& sas, uint32_t addr) {
    SAS::SegWPtr ptr;
    for (const auto& seg : sas.segments()) {
        if (seg.lock()->start == addr) {
            ptr = seg.lock();
            break;
        }
    }
    REQUIRE(!ptr.expired());
    return ptr;
}

TEST_CASE("Test top") {
    static constexpr int s1_val = 1;
    static constexpr int s1_size = 10;
    static constexpr int s1_start = 100;
    static constexpr int s2_val = 2;
    static constexpr int s3_val = 3;
    static constexpr uint32_t deadbeef = 0xDEADBEEF;

    SAS sas(s_minsegsize);
    addSegment(sas, s1_start, s1_size, s1_val);

    getExpectedSingleSegment(sas);

    SECTION("Coalescing") {
        SECTION("Fully contained coalescing") {
            const int s2_size = s1_size + 2;
            const auto s2_start = s1_start - 1;
            addSegment(sas, s2_start, s2_size, s2_val);

            // Only a single segment should be left and only 2's should be present (s1 has been fully overwritten)
            auto seg = getExpectedSingleSegment(sas);

            verifySegment(seg, s2_start, {{s2_val, s2_size}});
        }

        SECTION("Lower coalescing") {
            const uint32_t s2_start = s1_start + s1_size / 2;
            addSegment(sas, s2_start, s1_size, s2_val);

            // Only a single segment should be present due to coalescing. Segment starts with 1's and ends with 2's
            auto seg = getExpectedSingleSegment(sas);

            verifySegment(seg, s1_start, {{s1_val, s1_size / 2}, {s2_val, s1_size}});
        }

        SECTION("Upper coalescing") {
            const uint32_t s2_start = s1_start - s1_size / 2;
            addSegment(sas, s2_start, s1_size, s2_val);

            // Only a single segment should be present due to coalescing. Segment starts with 1's and ends with 2's
            auto seg = getExpectedSingleSegment(sas);

            verifySegment(seg, s2_start, {{s2_val, s1_size}, {s1_val, s1_size / 2}});
        }

        SECTION("Adjacent coalescing") {
            const uint32_t s2_start = s1_start - s1_size;
            addSegment(sas, s2_start, s1_size, s2_val);

            const uint32_t s3_start = s1_start + s1_size;
            addSegment(sas, s3_start, s1_size, s3_val);

            auto seg = getExpectedSingleSegment(sas);

            verifySegment(seg, s2_start, {{s2_val, s1_size}, {s1_val, s1_size}, {s3_val, s1_size}});
        }
    }

    SECTION("Read/write initialized") {
        SECTION("Read/write within segment") {
            const uint32_t addr = s1_start + s1_size / 2;
            sas.writeByte(addr, s2_val);
            auto seg = getExpectedSingleSegment(sas);

            REQUIRE(sas.readByte(addr) == s2_val);
            verifySegment(seg, s1_start, {{s1_val, s1_size / 2}, {s2_val, 1}, {s1_val, s1_size / 2 - 1}});
        }

        SECTION("Read/write non-byte") {
            const uint32_t addr = s1_start + s1_size / 2;
            sas.writeValue(addr, deadbeef);
            auto seg = getExpectedSingleSegment(sas);

            REQUIRE(sas.readValue<uint32_t>(addr) == deadbeef);

            // Verify surrounding bytes
            verifySegment(seg, s1_start,
                          {{s1_val, s1_size / 2},
                           {(deadbeef & 0xFF), 1},
                           {((deadbeef >> 8) & 0xFF), 1},
                           {((deadbeef >> 16) & 0xFF), 1},
                           {((deadbeef >> 24) & 0xFF), 1},
                           {s1_val, static_cast<int>(s1_size / 2 - sizeof(deadbeef))}});
        }
    }

    SECTION("Read/write uninitialized") {
        // Create additional lower and upper sections to populate the address space
        addSegment(sas, s1_start / 4, s1_size / 4, 4);
        addSegment(sas, s1_start * 9 / 4, s1_size / 4, 5);

        SECTION("Uninitialized value-access between segments") {
            const uint32_t s2_start = s1_start + 2 * s1_size;
            addSegment(sas, s2_start, s1_size, s2_val);

            const uint32_t addr = static_cast<uint32_t>(s1_start + s1_size * 1.5);
            sas.writeValue(addr, s1_val);
            REQUIRE(sas.readValue<uint32_t>(addr) == s1_val);

            /* Having minimum segment size 5, we expect the new segment to have merged together with segment s2, as
             * illustrated by the following:
             * 'v' is our write pointer. We initially write at 115, which allocates a new segment of size 5 from
             * 113-117. Subsequent iterations writes the following bytes of our 4-byte value. We see that in iteration
             * 4, the write pointer accesses address 118, which prompts a new segment to be created.
             * This new segment will subsequently be coalesced together with the initial 113-117 segment as well as the
             * 120-... segment (s2).
             *
             *             addr: 110 *----*----*----* 120
             * Iteration 1:             --v--  |---s2
             * Iteration 2:             ---v-  |---s2
             * Iteration 3:             ----v  |---s2
             * Iteration 4:             -----v-|---s2
             */

            const uint32_t segstart = addr - s_minsegsize / 2;
            auto seg = getSegmentAtAddr(sas, segstart);

            verifySegment(seg, segstart,
                          {{0, 2},
                           {s1_val, 1},
                           {((s1_val >> 8) & 0xFF), 1},
                           {((s1_val >> 16) & 0xFF), 1},
                           {((s1_val >> 24) & 0xFF), 1},
                           {0, 1},
                           {s2_val, s1_size}});
        }
    }
}

TEST_CASE("Initialization test") {
    static constexpr int s1_val = 1;
    static constexpr int s1_size = 10;
    static constexpr int s1_start = 10;
    static constexpr int s2_val = 2;
    static constexpr int s2_size = s1_size;
    static constexpr int s2_start = s1_start + 2 * s1_size;
    static constexpr int s3_val = 3;

    SAS sas(s_minsegsize);
    auto s1 = createSegment(s1_start, s1_size, s1_val);
    auto s2 = createSegment(s2_start, s2_size, s2_val);

    sas.addInitSegment(*s1);
    sas.addInitSegment(*s2);

    // Verify that no segments have been written to the active SAS memory
    REQUIRE(sas.segments().size() == 0);

    // Reset the SAS, writing initialization segments to active memory
    sas.reset();

    // verify that init segments are present in the SAS
    auto seg1 = getSegmentAtAddr(sas, s1_start);
    verifySegment(seg1, s1_start, {{s1_val, s1_size}});
    auto seg2 = getSegmentAtAddr(sas, s2_start);
    verifySegment(seg2, s2_start, {{s2_val, s2_size}});

    // Overwrite SAS without inferring new segments at the start of s1 and end of s2 (but a new segment is inferred in
    // between the two segments).
    const uint32_t s3_start = s1_start;
    const uint32_t s3_end = s2_start + s2_size;
    const int s3_size = s3_end - s3_start;
    uint32_t s3_wrptr = s3_start;
    while (s3_wrptr < s3_end) {
        sas.writeByte(s3_wrptr++, s3_val);
    }

    // Verify that SAS has been overwritten and now only has a single segment
    auto seg3 = getExpectedSingleSegment(sas);
    verifySegment(seg3, s3_start, {{s3_val, s3_size}});

    // Verify that s1 and s2 are unmodified
    verifySegment(s1, s1_start, {{s1_val, s1_size}});
    verifySegment(s2, s2_start, {{s2_val, s2_size}});

    // Reset SAS and verify that initial segments are now again present in SAS
    sas.reset();
    seg1 = getSegmentAtAddr(sas, s1_start);
    verifySegment(seg1, s1_start, {{s1_val, s1_size}});
    seg2 = getSegmentAtAddr(sas, s2_start);
    verifySegment(seg2, s2_start, {{s2_val, s2_size}});
}

TEST_CASE("Fuzz test") {
    // Fuzz-writes a large array, writing segments of the array in a random manner. Then, the sparse address space is
    // sequentially read through to very that the array was written as expected. This is performed with SASs of
    // different minimum segment sizes.

    const int datasize = static_cast<int>(GENERATE(std::pow(2, 1), std::pow(2, 3), std::pow(2, 5), std::pow(2, 7),
                                                   std::pow(2, 10), std::pow(2, 15), std::pow(2, 17)));

    // Min SS is based on the current data size. This is done to ensure that we are not testing very small minimum
    // segments with very large datasets, which will result in a heavily fragmented address space that is slow to
    // access.
    int minss = static_cast<int>(datasize / 100);
    minss = minss < 3 ? 3 : minss;
    minss = minss % 2 == 0 ? minss + 1 : minss;

    SECTION("Fuzzing") {
        SAS sas(minss);
        std::vector<uint8_t> data(datasize);
        std::generate(data.begin(), data.end(), std::rand);

        std::vector<int> idxsToWrite(datasize);
        std::iota(idxsToWrite.begin(), idxsToWrite.end(), 0);

        // Randomly write all data
        while (!idxsToWrite.empty()) {
            const int wrIdxToWrite = std::rand() % idxsToWrite.size();
            const int idx = idxsToWrite[wrIdxToWrite];
            sas.writeByte(idx, data[idx]);
            idxsToWrite.erase(idxsToWrite.begin() + wrIdxToWrite);
        }

        // there should only be a single segment due to all randomly created segments having been coalesced
        auto seg = getExpectedSingleSegment(sas);

        // Sequentially read the data
        for (int i = 0; i < datasize; i++) {
            const uint32_t sasvalue = sas.readByte(i);
            const uint32_t refvalue = data[i];

            if (sasvalue != refvalue) {
                FAIL("err");
            }

            REQUIRE(sasvalue == refvalue);
        }
    }
}
