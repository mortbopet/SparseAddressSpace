#define CATCH_CONFIG_MAIN
#include "external/Catch2/single_include/catch2/catch.hpp"

#include "SparseAddressSpace.h"

typedef SparseAddressSpace<uint32_t> SAS;
typedef SAS::Segment Seg;

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

TEST_CASE("Coalescing") {
    static constexpr int s1_val = 1;
    static constexpr int s1_size = 10;
    static constexpr int s1_start = 100;
    static constexpr int s2_val = 2;
    static constexpr int s3_val = 3;

    SAS sas;

    auto s1 = std::make_shared<Seg>();
    s1->data = std::vector<uint8_t>(s1_size, s1_val);
    s1->start = s1_start;

    sas.insertSegment(s1);
    getExpectedSingleSegment(sas);

    SECTION("Fully contained coalescing") {
        auto s2 = std::make_shared<Seg>();
        const int s2_size = s1_size + 2;
        s2->data = std::vector<uint8_t>(s2_size, s2_val);
        const auto s2_start = s1_start - 1;
        s2->start = s2_start;
        sas.insertSegment(s2);

        // Only a single segment should be left and only 2's should be present (s1 has been fully overwritten)
        auto seg = getExpectedSingleSegment(sas);

        verifySegment(seg, s2_start, {{s2_val, s2_size}});
    }

    SECTION("Lower coalescing") {
        auto s2 = std::make_shared<Seg>();
        s2->data = std::vector<uint8_t>(s1_size, s2_val);
        const uint32_t s2_start = s1_start + s1_size / 2;
        s2->start = s2_start;
        sas.insertSegment(s2);

        // Only a single segment should be present due to coalescing. Segment starts with 1's and ends with 2's
        auto seg = getExpectedSingleSegment(sas);

        verifySegment(seg, s1_start, {{s1_val, s1_size / 2}, {s2_val, s1_size}});
    }

    SECTION("Upper coalescing") {
        auto s2 = std::make_shared<Seg>();
        s2->data = std::vector<uint8_t>(s1_size, s2_val);
        const uint32_t s2_start = s1_start - s1_size / 2;
        s2->start = s2_start;
        sas.insertSegment(s2);

        // Only a single segment should be present due to coalescing. Segment starts with 1's and ends with 2's
        auto seg = getExpectedSingleSegment(sas);

        verifySegment(seg, s2_start, {{s2_val, s1_size}, {s1_val, s1_size / 2}});
    }

    SECTION("Adjacent coalescing") {
        auto s2 = std::make_shared<Seg>();
        s2->data = std::vector<uint8_t>(s1_size, s2_val);
        const uint32_t s2_start = s1_start - s1_size;
        s2->start = s2_start;
        sas.insertSegment(s2);

        auto s3 = std::make_shared<Seg>();
        s3->data = std::vector<uint8_t>(s1_size, s3_val);
        const uint32_t s3_start = s1_start + s1_size;
        s3->start = s3_start;
        sas.insertSegment(s3);

        auto seg = getExpectedSingleSegment(sas);

        verifySegment(seg, s2_start, {{s2_val, s1_size}, {s1_val, s1_size}, {s3_val, s1_size}});
    }
}

TEST_CASE("Read/write") {
    static constexpr int s1_val = 1;
    static constexpr int s1_size = 20;
    static constexpr int s1_start = 100;
    static constexpr int s2_val = 2;
    static constexpr int s3_val = 3;
    static constexpr uint32_t deadbeef = 0xDEADBEEF;

    SAS sas;

    auto s1 = std::make_shared<Seg>();
    s1->data = std::vector<uint8_t>(s1_size, s1_val);
    s1->start = s1_start;

    sas.insertSegment(s1);
    getExpectedSingleSegment(sas);

    SECTION("Read/write within segment") {
        sas.writeByte(s1_start + s1_size / 2, s2_val);
        auto seg = getExpectedSingleSegment(sas);

        verifySegment(seg, s1_start, {{s1_val, s1_size / 2}, {s2_val, 1}, {s1_val, s1_size / 2 - 1}});
    }

    SECTION("Read/write between segments (uncoalesced)") {}

    SECTION("Read/write between segments (upper coalesce)") {}

    SECTION("Read/write between segments (Adjacent coalesce from new segment)") {}

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
                       {s1_val, s1_size / 2 - sizeof(deadbeef)}});
    }
}
