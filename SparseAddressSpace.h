#include <memory>
#include <set>
#include <vector>

#include "external/intervaltree/IntervalTree.h"

template <typename T_addr, typename T_v, size_t minSegSize = 4 /** Minimum segment size, in bytes */>
struct SparseAddressSpace {
    struct Segment;
    using SegSPtr = std::shared_ptr<Segment>;
    using SegWPtr = std::weak_ptr<Segment>;
    using T_interval = Interval<size_t, SegSPtr>;
    using Range = std::pair<size_t, size_t>;

    struct Segment {
        Segment() {}

        T_addr start;
        inline T_addr end() const { return start + data.size(); }
        bool contains(const Segment& other) const {
            return start <= other.start && (start + data.size()) >= (other.start + other.data.size());
        }
        T_interval toInterval() { return T_interval(start, end(), toSharedPtr()); }
        bool operator==(const Segment& other) const { return start == other.start && data == other.data; }
        std::vector<uint8_t> data;

        std::shared_ptr<Segment> toSharedPtr() {
            if (!m_ptr.expired()) {
                return std::shared_ptr(m_ptr);
            } else {
                // Local pointer is uninitialized...
                auto shptr = std::shared_ptr<Segment>(this);
                m_ptr = shptr;
                return shptr;
            }
        }

        // All references to this Segment is managed by a shared pointer pointing to m_ptr. m_ptr is a weak pointer, to
        // ensure that this object itself does not maintain a reference to itself.
        std::weak_ptr<Segment> m_ptr;
    };

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
     * The specified program will be added as a memory segment which will be loaded into this memory
     * once it is reset.
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

        // Coalesce any overlapping upper and lower segments into the new segment
        const std::vector<T_addr> edges = {segment->start, static_cast<T_addr>(segment->end())};
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
        segmentsToKeepVec.push_back({segment->start, segment->end(), segment});

        // Rebuild the interval tree with the new set of (coalesced) intervals. std::move is used due to the r-value
        // reference constraint of the IntervalTree constructor
        data = IntervalTree<size_t, SegSPtr>(std::move(segmentsToKeepVec));
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

        // Figure out how much to coalesce from s1 lower and upper
        const int coalesce_n_bytes = s2->start - s1->start;

        // Coalesce lower
        if (coalesce_n_bytes > 0) {
            s2->data.insert(s2->data.begin(), s1->data.begin(), s1->data.begin() + std::abs(coalesce_n_bytes));
            s2->start = s1->start;
        }

        // Coalesce upper
        if (coalesce_n_bytes < 0) {
            s2->data.insert(s2->data.end(), s1->data.end() - std::abs(coalesce_n_bytes), s1->data.end());
        }

        return s2;
    }

    std::vector<SparseAddressSpace> initData;

    IntervalTree<size_t, SegSPtr> data;
};
