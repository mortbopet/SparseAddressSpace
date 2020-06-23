#pragma once

#include <memory>
#include <set>
#include <vector>

#include <limits.h>

#include "external/intervaltree/IntervalTree.h"

#ifdef USE_SAS_NAMESPACE
namespace sas {
#endif

template <typename T_addr>
class SparseAddressSpace {
public:
    /** @brief LargeInt
     * Used in cases when checking and adjusting under/overflow within the T_addr range.
     */
    using LargeInt = long long;
    static_assert(sizeof(LargeInt) > sizeof(T_addr),
                  "Address type must be smaller than the internal large integer value.");
    constexpr static LargeInt c_maxAddr = std::numeric_limits<T_addr>::max();

    struct Segment;
    using SegSPtr = std::shared_ptr<Segment>;
    using SegWPtr = std::weak_ptr<Segment>;
    using T_interval = Interval<LargeInt, SegSPtr>;
    using IntervalVector = std::vector<T_interval>;
    using Range = std::pair<LargeInt, LargeInt>;
    using SASData = IntervalTree<LargeInt, SegSPtr>;
    using SAS = SparseAddressSpace<T_addr>;

    /**
     * @brief The Segment struct
     * Represents a segment of contiguous memory within the address space.
     */
    struct Segment : public std::enable_shared_from_this<Segment> {
        Segment() {}

        /**
         * @brief start: address of the first byte in this segment
         */
        T_addr start;
        /**
         * @brief end: address of the last byte in this segment
         */
        inline LargeInt end() const { return static_cast<LargeInt>(start) + data.size() - 1; }
        inline bool contains(const Segment& other) const { return start <= other.start && end() >= other.end(); }
        inline bool contains(const T_addr addr) const { return start <= addr && addr <= end(); }

        /**
         * @brief toInterval
         * @returns an interval object of the given segment. Note that the end of the interval will point to the address
         * after the end() address. This is done to aid in coalescing adjacent blocks.
         */
        T_interval toInterval() {
            // Edge case: if this segment reaches the top of the address space, we must truncate the interval within the
            // range of T_addr. This is allowed due to the impossibility of coalescing segments out of bounds of the
            // address space.
            LargeInt realEnd = end() + 1;
            if (realEnd > c_maxAddr) {
                realEnd = c_maxAddr;
            }
            assert(realEnd >= start);
            return T_interval(start, realEnd, toSPtr());
        }
        SegSPtr toSPtr() { return this->shared_from_this(); }
        bool operator==(const Segment& other) const { return start == other.start && data == other.data; }

        Segment& operator=(const Segment& other) {
            start = other.start;
            data = other.data;
            return *this;
        }
        std::vector<uint8_t> data;
    };

    SparseAddressSpace(const unsigned minSegSize = 5) : m_minSegSize(minSegSize) {
        assert(m_minSegSize % 2 == 1 && "m_minSegSize must be an uneven value");
        assert(m_minSegSize >= 3 && "m_minSegSize must be at least 3");
    }

    void writeByte(T_addr byteAddress, uint8_t value) {
        SegSPtr segment = segmentForAddress(byteAddress);

        // Perform write
        const int wridx = byteAddress - segment->start;
        assert(wridx >= 0 && wridx < segment->data.size());
        segment->data[wridx] = value;
    }

    template <typename T_v>
    void writeValue(T_addr byteAddress, T_v value, size_t nbytes) {
        if (nbytes > sizeof(value)) {
            throw std::runtime_error("Trying to write more bytes than what is contained in @p value");
        }
        for (unsigned i = 0; i < nbytes; i++) {
            writeByte(byteAddress++, value);
            value >>= CHAR_BIT;
        }
    }

    template <typename T_v>
    void writeValue(T_addr byteAddress, T_v value) {
        writeValue(byteAddress, value, sizeof(T_v));
    }

    uint8_t readByte(T_addr address) const {
        SegSPtr segment = segmentForAddress(address);

        // Perform read
        const int rdidx = address - segment->start;
        assert(rdidx >= 0 && rdidx < segment->data.size());
        return segment->data[rdidx];
    }

    template <typename T_v>
    T_v readValue(T_addr address) const {
        T_v value = 0;
        for (unsigned i = 0; i < sizeof(T_v); i++)
            value |= readByte(address++) << (i * CHAR_BIT);

        return value;
    }

    SegSPtr contains(uint32_t address) const {
        auto overlapping = data.findOverlapping(address, address);
        SegSPtr seg;
        if (overlapping.size() == 0) {
            return seg;
        }
        assert(overlapping.size() == 1);

        // Query the overlapping segment for whether contains the address. This is to avoid an off-by-1 error
        // wherein the interval of a segment is inclusive of the address of the first byte after the last byte in
        // the segment.
        seg = overlapping[0].value;
        return seg->contains(address) ? seg : SegSPtr();
    }

    SAS& getInitSas() {
        if (!m_initData) {
            m_initData = std::make_unique<SAS>();
        }
        return *m_initData;
    }

    void clear() {
        data = SASData();
        if (m_initData) {
            m_initData->clear();
        }
    }

    void reset() {
        data = SASData();

        // Deep copy all segments in the initialization data to the current data
        if (m_initData) {
            m_initData->data.visit_all([=](const auto& interval) {
                /*
                Segment segCopy = *interval.value;
                // Initialize the shared_ptr required for std::shared_from_this
                SegSPtr segCopyPtr = std::make_shared<Segment>(segCopy);
                */
                SegSPtr segCopyPtr = std::make_shared<Segment>(*interval.value);
                insertSegment(*segCopyPtr);
            });
        }
    }

    /**
     * @brief insertSegment
     * Inserts memory segment @p segment at the specified starting address.
     * If the segment overlaps any other memory segments, these will be coalesced, with overlapping
     * memory values being taken from the newly inserted segment. We only check for overlaps at the
     * start and stop address. Any segments contained within the newly inserted segment will be
     * deleted.
     */
    void insertSegment(Segment& segment) {
        if (segment.data.size() == 0) {
            // Nothing to do
            return;
        }

        /** Struct wrapper around an interval pointer to ensure that std::set does not try to
         * overload resolve with an iterator*/
        std::set<SegSPtr> segmentsToKeep;
        data.visit_all([&](auto& interval) { segmentsToKeep.insert(interval.value); });

        // Locate segments which are fully contained within the new segment. These contained
        // segments shall be removed.
        std::vector<T_interval> contained = data.findContained(segment.start, segment.end());
        for (auto& i : contained) {
            segmentsToKeep.erase(i.value);
        }

        // Coalesce any overlapping upper and lower segments into the new segment. Address segment->end() + 1 ensures
        // coalescing of adjacent blocks
        const std::vector<LargeInt> edges = {segment.start, segment.end() + 1};
        for (LargeInt edgeAddress : edges) {
            std::vector<T_interval> overlaps = data.findOverlapping(edgeAddress, edgeAddress);
            for (auto& i : overlaps) {
                segmentsToKeep.erase(i.value);
                coalesce(*i.value, segment);
            }
        }

        // convert segments to keep into format required by IntervalTree
        std::vector<T_interval> segmentsToKeepVec;
        for (const auto& seg : segmentsToKeep) {
            segmentsToKeepVec.push_back(seg->toInterval());
        }

        // Insert (coalesced) new segment into segments to keep
        segmentsToKeepVec.push_back(segment.toInterval());

        // Rebuild the interval tree with the new set of (coalesced) intervals. std::move is used due to the r-value
        // reference constraint of the IntervalTree constructor
        data = SASData(std::move(segmentsToKeepVec));
        setMRUSeg(segment.toSPtr());
    }

    void insertSegment(const T_addr startaddr, const std::vector<uint8_t>& data) {
        auto s = std::make_shared<Segment>();
        s->data = data;
        s->start = startaddr;
        insertSegment(*s);
    }

    void insertSegment(const T_addr startaddr, const uint8_t* data, size_t n) {
        insertSegment(startaddr, std::vector<uint8_t>(data, data + n));
    }

    std::vector<SegWPtr> segments() const {
        std::vector<SegWPtr> segs;
        data.visit_all([&](const auto& interval) { segs.emplace_back(interval.value); });
        return segs;
    }

private:
    /**
     * @brief segmentForAddress
     * @returns a segment containing the requested byte address @param addr. If no segment is found, a new segment is
     * created. segmentForAddress may create new segments if a segment is missing.
     *
     * As such, the physical state of the SAS may be modified in the function, however the logical state (which is an
     * unrestricted address space) is maintained - hence, the function is marked const.
     */
    SegSPtr segmentForAddress(T_addr addr) const {
        // Physical changes to the SAS are performed through a non-const pointer to this
        auto* thisNonConst = const_cast<SAS*>(this);

        SegSPtr seg;
        // Initially, check if MRU segment is our target segment, to speed up spatial locality accesses. Else, traverse
        // the sparse array
        if (m_mruSegment && m_mruSegment->contains(addr)) {
            // MRU access
            seg = m_mruSegment;
        } else if ((seg = contains(addr))) {
            return seg;
        } else {
            // No segment contains the requested address, create new segment and retry
            thisNonConst->createMissingSegment(addr);
            return segmentForAddress(addr);
        }

        thisNonConst->setMRUSeg(seg);
        return seg;
    }

    void createMissingSegment(T_addr addr) {
        assert(!contains(addr));
        std::vector<T_interval> intervals;
        // Find near segments to the missing address
        data.visit_all([&](auto& interval) { intervals.emplace_back(interval); });

        // Find closest upper and lower segments to the address
        const T_interval* lower = nullptr;
        const T_interval* upper = nullptr;

        for (const auto& interval : intervals) {
            if (interval.stop <= addr) {
                if (!lower) {
                    lower = &interval;
                } else if (interval.stop > lower->stop) {
                    lower = &interval;
                }
            }

            if (interval.start > addr) {
                if (!upper) {
                    upper = &interval;
                } else if (interval.start < upper->start) {
                    upper = &interval;
                }
            }
        }

        // Create a segment centered at the requested address with size m_minSegSize. If such a new segment
        // overlaps with the closest segments to the new segment, the new segment will adjusted accordingly (either
        // truncated or shifted wrt. the center address). We ensure that the bounds of the new segment is adjusted to
        // facilitate coalescing when inserted.
        LargeInt newstart = static_cast<LargeInt>(addr) - m_minSegSize / 2;
        const int adjustStart = newstart < 0 ? -newstart : 0;
        newstart += adjustStart;
        LargeInt newstop = static_cast<LargeInt>(addr) + m_minSegSize / 2 + 1 + adjustStart;

        if (lower && static_cast<T_addr>(lower->stop) >= newstart) {
            const auto truncatedBytes = static_cast<T_addr>(lower->stop) - newstart;
            newstart = lower->stop;

            // Add the truncated bytes to the other end of the new segment
            newstop += truncatedBytes;
        }

        if (upper && static_cast<T_addr>(upper->start) <= newstop) {
            newstop = static_cast<T_addr>(upper->start);
        }
        if (newstop > c_maxAddr) {
            // Truncate within address space. Newstop always points to the first address after the last byte of the new
            // segment. In this case, this address is outside of the T_addr address space, however, the below adjustment
            // of newstop ensures that we are able to allocate a segment up to and including the last address in the
            // address space.
            newstop = c_maxAddr + 1;
        }
        const int segsize = newstop - newstart;
        assert(segsize != 0);
        insertSegment(static_cast<T_addr>(newstart), std::vector<uint8_t>(segsize, 0));
    }

    inline void setMRUSeg(SegSPtr ptr) {
        if (m_mruSegment != ptr) {
            m_mruSegment = ptr;
        }
    }

    /**
     * @brief coalesce
     * Coalesce two segments, using values of @p s2 for any overlapping addresses between @p s1 and
     * @p s2.
     */
    Segment& coalesce(Segment& s1, Segment& s2) {
        if (s2.contains(s1)) {
            return s2;
        }

        // Coalesce lower
        const int coalesce_lower_bytes = s2.start - s1.start;
        if (coalesce_lower_bytes > 0) {
            s2.data.insert(s2.data.begin(), s1.data.begin(), s1.data.begin() + coalesce_lower_bytes);
            s2.start = s1.start;
        }

        // Coalesce upper
        const int coalesce_upper_bytes = s1.end() - s2.end();
        if (coalesce_upper_bytes > 0) {
            s2.data.insert(s2.data.end(), s1.data.end() - coalesce_upper_bytes, s1.data.end());
        }

        return s2;
    }

    /**
     * @brief m_initData
     * SAS representing the segments which will be written to this SAS upon datastructure reset.
     */
    std::unique_ptr<SAS> m_initData;

    /**
     * @brief data
     * Interval tree representing the currently active segments in the address space.
     */
    SASData data;

    /**
     * @brief m_mruSegment
     * Pointer to the most recently accessed segment in the address space. This segment will be checked on each
     * read/write to speed up accesses with spatial locality.
     */
    SegSPtr m_mruSegment;

    /**
     * @brief m_minSegSize
     * Minimum segment size, in bytes.
     * When an address which is not contained within any segment is accessed, a new segment is created around the
     * address. This segment will (assuming no conflicts with adjacent segments, see createMissingSegment) have
     * m_minSegSize width, centerred around the requested address.
     */
    const unsigned m_minSegSize;
};

#ifdef USE_SAS_NAMESPACE
}
#endif
