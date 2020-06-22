#pragma once

#include <memory>
#include <set>
#include <vector>

#include <limits.h>

#include "external/intervaltree/IntervalTree.h"

template <typename T_addr, size_t t_minSegSize = 5>
class SparseAddressSpace {
public:
    struct Segment;
    using SegSPtr = std::shared_ptr<Segment>;
    using SegWPtr = std::weak_ptr<Segment>;
    using T_interval = Interval<size_t, SegSPtr>;
    using IntervalVector = std::vector<T_interval>;
    using Range = std::pair<size_t, size_t>;

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
        inline T_addr end() const { return start + data.size() - 1; }
        inline bool contains(const Segment& other) const { return start <= other.start && end() >= other.end(); }
        inline bool contains(const T_addr addr) const { return start <= addr && addr <= end(); }

        /**
         * @brief toInterval
         * @returns an interval object of the given segment. Note that the end of the interval will point to the address
         * after the end() address. This is done to aid in coalescing adjacent blocks.
         */
        T_interval toInterval() { return T_interval(start, end() + 1, this->shared_from_this()); }
        bool operator==(const Segment& other) const { return start == other.start && data == other.data; }
        std::vector<uint8_t> data;
    };

    SparseAddressSpace(const unsigned minSegSize = 5) : m_minSegSize(minSegSize) {
        assert(t_minSegSize % 2 == 1 && "t_minSegSize must be an uneven value");
        assert(t_minSegSize >= 3 && "t_minSegSize must be at least 3");
    }

    void writeByte(T_addr byteAddress, uint8_t value) {
        SegSPtr segment = segmentForAddress(byteAddress);

        // Perform write
        const int wridx = byteAddress - segment->start;
        assert(wridx >= 0 && wridx < segment->data.size());
        segment->data[wridx] = value;
    }

    template <typename T_v>
    void writeValue(T_addr byteAddress, T_v value) {
        const size_t bytes = sizeof(T_v);
        for (unsigned i = 0; i < bytes; i++) {
            writeByte(byteAddress++, value);
            value >>= CHAR_BIT;
        }
    }

    uint8_t readByte(T_addr address) {
        SegSPtr segment = segmentForAddress(address);

        // Perform read
        const int rdidx = address - segment->start;
        assert(rdidx >= 0 && rdidx < segment->data.size());
        return segment->data[rdidx];
    }

    template <typename T_v>
    T_v readValue(T_addr address) {
        T_v value = 0;
        for (unsigned i = 0; i < sizeof(T_v); i++)
            value |= readByte(address++) << (i * CHAR_BIT);

        return value;
    }

    SegSPtr contains(uint32_t address) const {
        auto& overlapping = data.findOverlapping(address, address);

        if (overlapping.size() == 0) {
            return false;
        }

        assert(overlapping.size() == 1);

        // Query the overlapping segment for whether contains the address. This is to avoid an off-by-1 error wherein
        // the interval of a segment is inclusive of the address of the first byte after the last byte in the segment.
        SegSPtr seg = overlapping[0].value;
        if (seg->contains(address)) {
            return seg;
        }
        return SegSPtr();
    }

    void addInitSegment(const Segment& other) {
        if (!m_initData) {
            m_initData = std::make_unique<SparseAddressSpace<T_addr>>();
        }
        m_initData.insertSegment(other);
    }

    void clearInitArrays() { m_initData.clear(); }

    void reset() {
        data.clear();

        // Deep copy all segments in the initialization data to the current data
        m_initData.visit_all([=](const auto& interval) {
            auto segCopy = std::make_unique<Segment>(interval->data);
            insertSegment(segCopy);
        });
    }

    /**
     * @brief insertSegment
     * Inserts memory segment @p segment at the specified starting address.
     * If the segment overlaps any other memory segments, these will be coalesced, with overlapping
     * memory values being taken from the newly inserted segment. We only check for overlaps at the
     * start and stop address. Any segments contained within the newly inserted segment will be
     * deleted.
     */
    void insertSegment(SegSPtr& segment) {
        /** Struct wrapper around an interval pointer to ensure that std::set does not try to
         * overload resolve with an iterator*/
        std::set<SegSPtr> segmentsToKeep;
        data.visit_all([&](auto& interval) { segmentsToKeep.insert(interval.value); });

        // Locate segments which are fully contained within the new segment. These contained
        // segments shall be removed.
        std::vector<T_interval> contained = data.findContained(segment->start, segment->end());
        for (auto& i : contained) {
            segmentsToKeep.erase(i.value);
        }

        // Coalesce any overlapping upper and lower segments into the new segment. Address segment->end() + 1 ensures
        // coalescing of adjacent blocks
        const std::vector<T_addr> edges = {segment->start, static_cast<T_addr>(segment->end() + 1)};
        for (T_addr edgeAddress : edges) {
            std::vector<T_interval> overlaps = data.findOverlapping(edgeAddress, edgeAddress);
            for (auto& i : overlaps) {
                segmentsToKeep.erase(i.value);
                coalesce(i.value, segment);
            }
        }

        // convert segments to keep into format required by IntervalTree
        std::vector<T_interval> segmentsToKeepVec;
        for (const auto& seg : segmentsToKeep) {
            segmentsToKeepVec.push_back(seg->toInterval());
        }

        // Insert (coalesced) new segment into segments to keep
        segmentsToKeepVec.push_back(segment->toInterval());

        // Rebuild the interval tree with the new set of (coalesced) intervals. std::move is used due to the r-value
        // reference constraint of the IntervalTree constructor
        data = IntervalTree<size_t, SegSPtr>(std::move(segmentsToKeepVec));
        setMRUSeg(segment);
    }

    void insertSegment(const T_addr start, uint8_t* data, size_t n) {
        Segment seg;
        seg.start = start;
        seg.data = std::vector(data, data + n);
        insertSegment(seg);
    }

    std::vector<SegWPtr> segments() const {
        std::vector<SegWPtr> segs;
        data.visit_all([&](const auto& interval) { segs.emplace_back(interval.value); });
        return segs;
    }

    /**
     * @brief coalesce
     * Coalesce two segments, using values of @p s2 for any overlapping addresses between @p s1 and
     * @p s2.
     */
    SegSPtr coalesce(SegSPtr s1, SegSPtr s2) {
        if (s2->contains(*s1)) {
            return s2;
        }

        // Coalesce lower
        const int coalesce_lower_bytes = s2->start - s1->start;
        if (coalesce_lower_bytes > 0) {
            s2->data.insert(s2->data.begin(), s1->data.begin(), s1->data.begin() + coalesce_lower_bytes);
            s2->start = s1->start;
        }

        // Coalesce upper
        const int coalesce_upper_bytes = s1->end() - s2->end();
        if (coalesce_upper_bytes > 0) {
            s2->data.insert(s2->data.end(), s1->data.end() - coalesce_upper_bytes, s1->data.end());
        }

        return s2;
    }

private:
    /**
     * @brief segmentForAddress
     * @returns a segment containing the requested byte address @param addr. If no segment is found, a new segment is
     * created.
     */
    SegSPtr segmentForAddress(T_addr addr) {
        SegSPtr seg;
        // Initially, check if MRU segment is our target segment, to speed up spatial locality accesses. Else, traverse
        // the sparse array
        if (m_mruSegment && m_mruSegment->contains(addr)) {
            // MRU access
            seg = m_mruSegment;
        } else if (seg = contains(addr)) {
            return seg;
        } else {
            // No segment contains the requested address, create new segment and retry
            createMissingSegment(addr);
            return segmentForAddress(addr);
        }

        setMRUSeg(seg);
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

        // Create a segment centered at the requested address with size t_minSegSize. If such a new segment
        // overlaps with the closest segments to the new segment, the new segment will adjusted accordingly (either
        // truncated or shifted wrt. the center address). We ensure that the bounds of the new segment is adjusted to
        // facilitate coalescing when inserted.
        long long newstart = static_cast<long long>(addr) - m_minSegSize / 2;
        newstart = newstart < 0 ? 0 : newstart;
        long long newstop = addr + m_minSegSize / 2 + 1;

        if (lower && lower->stop >= newstart) {
            const int truncatedBytes = lower->stop - newstart;
            newstart = lower->stop;

            // Add the truncated bytes to the other end of the new segment
            newstop += truncatedBytes;
        }

        if (upper && upper->start <= newstop) {
            newstop = upper->start;
        }

        // Create the new segment
        auto s = std::make_shared<Segment>();
        const int segsize = newstop - newstart;
        s->data = std::vector<uint8_t>(segsize, 0);
        s->start = newstart;

        insertSegment(s);
    }

    void setMRUSeg(SegSPtr ptr) {
        if (m_mruSegment != ptr) {
            m_mruSegment = ptr;
        }
    }

    /**
     * @brief m_initData
     * Set of SAS structures representing the segments which will be written to this SAS upon datastructure reset.
     */
    std::unique_ptr<SparseAddressSpace<T_addr>> m_initData;

    /**
     * @brief data
     * Interval tree representing the currently active segments in the address space.
     */
    IntervalTree<size_t, SegSPtr> data;

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
