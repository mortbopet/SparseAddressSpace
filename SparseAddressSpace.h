#include <vector>
#include <unordered_set>
#include <memory>

#include "external/intervaltree/IntervalTree.h"

template <typename T_addr, typename T_v, size_t minSegSize = 4 /** Minimum segment size, in bytes */>
struct SparseAddressSpace {
    struct Segment {
        T_addr start;
        std::vector<uint8_t> data;

        bool contains(const Segment& other) const {
            return start <= other.start && (start + data.size()) >= (other.start + other.data.size());
        }

        bool operator=(const Segment& other) const {
            return start == other.start && data == other.data;
        }
    };

    using SegmentPtr = std::shared_ptr<Segment>;
    using SegmentWPtr = std::weak_ptr<Segment>;
    using T_interval = Interval<size_t, SegmentPtr>;
    using Range = std::pair<size_t, size_t>;
    SparseAddressSpace() {}

    SparseAddressSpace(const T_addr startAddr, uint8_t* data, size_t n) { addInitArray(startAddr, data, n); }
    SparseAddressSpace(const Segment& seg) { addInitArray(seg); }

    void write(T_addr address, uint8_t value) {
        if (contains(address)) {
            std::vector<T_interval> results;
            data.findContained(address, address, results);
            auto& seg = results[0];
            const size_t idx = address - seg->start;
            seg->data[idx] = value;
        }
    }

    template <bool byteIndexed = true>
    T_v read(T_addr address, unsigned width = 4) {
        if constexpr (!byteIndexed)
            address <<= 2;

        T_v value = 0;
        for (unsigned i = 0; i < width; i++)
            value |= data[address++] << (i * CHAR_BIT);

        return value;
    }

    template <bool byteIndexed = true>
    T_v readMemConst(T_addr address, unsigned width = 4) const {
        if constexpr (!byteIndexed)
            address <<= 2;

        T_v value = 0;
        for (unsigned i = 0; i < width; i++) {
            value |= contains(address) ? data.at(address) << (i * CHAR_BIT) : 0;
            address++;
        }

        return value;
    }

    bool contains(uint32_t address) const {
        std::vector<T_interval> results;
        data.findContained(address, address, results);
        return results.size() > 0;
    }

    /**
     * @brief addInitializationMemory
     * The specified program will be added as a memory segment which will be loaded into this memory once it is reset.
     */
    void addInitArray(const T_addr start, uint8_t* data, size_t n) {
        auto seg = std::make_shared<Segment>();
        seg->start = start;
        seg->data = std::vector(data, data + n);
        addInitArray(seg);
    }

    void addInitArray(const SparseAddressSpace& other) { initData.insertSegment(other); }

    void clearInitArrays() { initData.clear(); }

    void reset() {
        data.clear();

        // Deep copy all segments in the initialization data to the current data
        initData.visit_all([=](const auto& interval) {
            auto segCopy = std::make_unique<Segment>(interval->data);
            insertSegment(segCopy);
        });
    }

    /**
     * @brief insertSegment
     * Inserts memory segment @p segment at the specified starting address.
     * If the segment overlaps any other memory segments, these will be coalesced, with overlapping memory values being
     * taken from the newly inserted segment.
     * We only check for overlaps at the start and stop address. Any segments contained within the newly inserted
     * segment will be deleted.
     */
    void insertSegment(SegmentPtr& segment) {
        /** Struct wrapper around an interval pointer to ensure that std::set does not try to overload resolve with an iterator */
        struct IntervalPtr {
            T_interval* ptr;
        };
        std::unordered_set<IntervalPtr> segmentsToKeep;
        data.visit_all([&](const auto& interval) { segmentsToKeep.insert(&interval); });

        // Locate segments which are fully contained within the new segment. These contained segments shall be removed.
        std::vector<T_interval> contained = data.findContained(segment->start, segment->start + segment->data.size());
        for (auto& i : contained) {
            segmentsToKeep.erase({&i});
        }

        // Coalesce any overlapping upper and lower segments
        const std::vector<T_addr> edges = {segment->start, static_cast<T_addr>(segment->start + segment->data.size())};
        for (T_addr edgeAddress : edges) {
            std::vector<T_interval> overlaps = data.findContained(edgeAddress, edgeAddress);
            for (auto& i : overlaps) {
                segmentsToKeep.erase({&i});
                coalesce(i.value, segment);
            }
        }

        std::vector<T_interval> segmentsToKeepVec(segmentsToKeep.begin(), segmentsToKeep.end());

        // Rebuild the interval tree with the new set of (coalesced) intervals
        data = IntervalTree<size_t, SegmentPtr>(segmentsToKeepVec);
    }

    std::vector<SegmentWPtr> segments() const {
        std::vector<SegmentWPtr> segs;
        data.visit_all([&](const auto& interval) { segs.emplace_back(interval.value); });
        return segs;
    }

    /**
     * @brief coalesce
     * Coalesce two segments, using values of @p s2 for any overlapping addresses between @p s1 and @p s2.
     */
    SegmentPtr coalesce(SegmentPtr s1, SegmentPtr s2) {
        if (s2->contains(*s1)) {
            return s2;
        }

        // Figure out how much to coalesce from s1 lower and upper
        const int coalesce_n_bytes = s2->start - s1->start;

        // Coalesce lower
        if (coalesce_n_bytes > 0) {
            s2->data.insert(s2->data.begin(), s1->data.begin(), s1->data.begin() + std::abs(coalesce_n_bytes));
        }

        // Coalesce upper
        if (coalesce_n_bytes < 0) {
            s2->data.insert(s2->data.end(), s1->data.end() - std::abs(coalesce_n_bytes), s1->data.end());
        }

        return s2;
    }

    std::vector<SparseAddressSpace> initData;

    IntervalTree<size_t, SegmentPtr> data;
};
