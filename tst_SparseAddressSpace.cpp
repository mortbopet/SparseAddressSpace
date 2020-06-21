#define CATCH_CONFIG_MAIN
#include "external/Catch2/single_include/catch2/catch.hpp"

#include "SparseAddressSpace.h"


TEST_CASE( "Contained coalesce" ) {
    typedef SparseAddressSpace<uint32_t, uint32_t> SAS;
    typedef SAS::Segment Seg;

    static constexpr uint8_t s1_val = 1;
    static constexpr uint8_t s1_size = 10;
    static constexpr uint8_t s1_start = 10;

    static constexpr uint8_t s2_val = 2;
    static constexpr uint8_t s2_size = s1_size + 2;

    SAS sas;

    auto s1 = std::make_shared<Seg>();
    s1->data = std::vector<uint8_t>(s1_size, s1_val);
    s1->start = s1_start;

    auto s2 = std::make_shared<Seg>();
    s2->data = std::vector<uint8_t>(s2_size, s2_val);
    s2->start = s1_start - 1;

    sas.insertSegment(s1);
    sas.insertSegment(s2);

    // Only a single segment should be left and only 2's should be present
    auto segs = sas.segments();
    REQUIRE( segs.size() == 1);
}
