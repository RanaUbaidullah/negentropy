// (C) 2023 Doug Hoyte. MIT license

#pragma once

#include <string.h>

#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include <stdexcept>



namespace negentropy {


const uint64_t MAX_U64 = std::numeric_limits<uint64_t>::max();
using err = std::runtime_error;


struct alignas(16) XorElem {
    uint64_t timestamp;
    uint64_t idSize;
    char id[32];

    XorElem() : timestamp(0), idSize(32) {
        memset(id, '\0', sizeof(id));
    }

    XorElem(uint64_t timestamp, std::string_view id_) : timestamp(timestamp), idSize(id_.size()) {
        if (idSize > 32) throw negentropy::err("id too big");
        memset(id, '\0', sizeof(id));
        memcpy(id, id_.data(), idSize);
    }

    XorElem(uint64_t timestamp, uint64_t idSize) : timestamp(timestamp), idSize(idSize) {
        if (idSize > 32) throw negentropy::err("id too big");
        memset(id, '\0', sizeof(id));
    }

    std::string_view getId() const {
        return std::string_view(id, idSize);
    }

    std::string_view getId(uint64_t subSize) const {
        return getId().substr(0, subSize);
    }

    XorElem& operator^=(const XorElem &other) {
        auto *p1 = static_cast<unsigned char *>(__builtin_assume_aligned(id, 16));
        auto *p2 = static_cast<unsigned char *>(__builtin_assume_aligned(other.id, 16));
        for (size_t i = 0; i < 32; i++) p1[i] ^= p2[i];
        return *this;
    }

    bool operator==(const XorElem &other) const {
        return timestamp == other.timestamp && getId() == other.getId();
    }
};

inline bool operator<(const XorElem &a, const XorElem &b) {
    return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.getId() < b.getId();
};


struct Negentropy {
    uint64_t idSize;

    struct BoundOutput {
        XorElem start;
        XorElem end;
        std::string payload;
    };

    std::vector<XorElem> items;
    bool sealed = false;
    bool isInitiator = false;
    uint64_t frameSizeLimit = 0;
    std::deque<BoundOutput> pendingOutputs;

    Negentropy(uint64_t idSize) : idSize(idSize) {
        if (idSize < 8 || idSize > 32) throw negentropy::err("idSize invalid");
    }

    void addItem(uint64_t createdAt, std::string_view id) {
        if (sealed) throw negentropy::err("already sealed");

        items.emplace_back(createdAt, id);
    }

    void seal() {
        if (sealed) throw negentropy::err("already sealed");

        std::reverse(items.begin(), items.end()); // typically pushed in approximately descending order so this may speed up the sort
        std::sort(items.begin(), items.end());
        sealed = true;
    }

    std::string initiate(uint64_t frameSizeLimit_ = 0) {
        if (!sealed) throw negentropy::err("not sealed");
        isInitiator = true;

        if (frameSizeLimit_ != 0 && frameSizeLimit_ < 1024) throw negentropy::err("frameSizeLimit too small");
        frameSizeLimit = frameSizeLimit_;

        splitRange(items.begin(), items.end(), XorElem(0, ""), XorElem(MAX_U64, ""), pendingOutputs);

        return buildOutput();
    }

    std::string reconcile(std::string_view query) {
        if (isInitiator) throw negentropy::err("initiator not asking for have/need IDs");
        std::vector<std::string> haveIds, needIds;
        reconcileAux(query, haveIds, needIds);
        return buildOutput();
    }

    std::string reconcile(std::string_view query, std::vector<std::string> &haveIds, std::vector<std::string> &needIds) {
        if (!isInitiator) throw negentropy::err("non-initiator asking for have/need IDs");
        reconcileAux(query, haveIds, needIds);
        return buildOutput();
    }

  private:
    void reconcileAux(std::string_view query, std::vector<std::string> &haveIds, std::vector<std::string> &needIds) {
        if (!sealed) throw negentropy::err("not sealed");

        auto prevBound = XorElem(0, "");
        auto prevIndex = items.begin();
        uint64_t lastTimestampIn = 0;
        std::deque<BoundOutput> outputs;

        while (query.size()) {
            auto currBound = decodeBound(query, lastTimestampIn);
            auto mode = decodeVarInt(query); // 0 = Skip, 1 = Fingerprint, 2 = IdList, 3 = IdListResponse

            auto lower = prevIndex;
            auto upper = std::upper_bound(prevIndex, items.end(), currBound);

            if (mode == 0) { // Skip
                // Do nothing
            } else if (mode == 1) { // Fingerprint
                XorElem theirXorSet(0, getBytes(query, idSize));

                XorElem ourXorSet;
                for (auto i = lower; i < upper; ++i) ourXorSet ^= *i;

                if (theirXorSet.getId() != ourXorSet.getId(idSize)) {
                    splitRange(lower, upper, prevBound, currBound, outputs);
                }
            } else if (mode == 2) { // IdList
                auto numIds = decodeVarInt(query);

                struct TheirElem {
                    uint64_t offset;
                    bool onBothSides;
                };

                std::unordered_map<std::string, TheirElem> theirElems;
                for (uint64_t i = 0; i < numIds; i++) {
                    auto e = getBytes(query, idSize);
                    theirElems.emplace(e, TheirElem{i, false});
                }

                std::vector<std::string> responseHaveIds;
                std::vector<uint64_t> responseNeedIndices;

                for (auto it = lower; it < upper; ++it) {
                    auto e = theirElems.find(std::string(it->getId()));

                    if (e == theirElems.end()) {
                        // ID exists on our side, but not their side
                        if (isInitiator) haveIds.emplace_back(it->getId());
                        else responseHaveIds.emplace_back(it->getId());
                    } else {
                        // ID exists on both sides
                        e->second.onBothSides = true;
                    }
                }

                for (const auto &[k, v] : theirElems) {
                    if (!v.onBothSides) {
                        // ID exists on their side, but not our side
                        if (isInitiator) needIds.emplace_back(k);
                        else responseNeedIndices.emplace_back(v.offset);
                    }
                }

                if (!isInitiator) {
                    std::string payload = encodeVarInt(3); // mode = IdListResponse

                    payload += encodeVarInt(responseHaveIds.size());
                    for (const auto &id : responseHaveIds) payload += id;

                    auto bitField = encodeBitField(responseNeedIndices);
                    payload += encodeVarInt(bitField.size());
                    payload += bitField;

                    outputs.emplace_back(BoundOutput({ prevBound, currBound, std::move(payload) }));
                }
            } else if (mode == 3) { // IdListResponse
                if (!isInitiator) throw negentropy::err("unexpected IdListResponse");

                auto numIds = decodeVarInt(query);
                for (uint64_t i = 0; i < numIds; i++) {
                    needIds.emplace_back(getBytes(query, idSize));
                }

                auto bitFieldSize = decodeVarInt(query);
                auto bitField = getBytes(query, bitFieldSize);

                for (auto it = lower; it < upper; ++it) {
                    if (bitFieldLookup(bitField, it - lower)) haveIds.emplace_back(it->getId());
                }
            } else {
                throw negentropy::err("unexpected mode");
            }

            prevIndex = upper;
            prevBound = currBound;
        }

        while (outputs.size()) {
            pendingOutputs.emplace_front(std::move(outputs.back()));
            outputs.pop_back();
        }
    }

    void splitRange(std::vector<XorElem>::iterator lower, std::vector<XorElem>::iterator upper, const XorElem &lowerBound, const XorElem &upperBound, std::deque<BoundOutput> &outputs) {
        uint64_t numElems = upper - lower;
        const uint64_t buckets = 16;

        if (numElems < buckets * 2) {
            std::string payload = encodeVarInt(2); // mode = IdList
            payload += encodeVarInt(numElems);
            for (auto it = lower; it < upper; ++it) payload += it->getId(idSize);

            outputs.emplace_back(BoundOutput({ lowerBound, upperBound, std::move(payload) }));
        } else {
            uint64_t itemsPerBucket = numElems / buckets;
            uint64_t bucketsWithExtra = numElems % buckets;
            auto curr = lower;
            XorElem prevBound = *curr;

            for (uint64_t i = 0; i < buckets; i++) {
                XorElem ourXorSet;
                for (auto bucketEnd = curr + itemsPerBucket + (i < bucketsWithExtra ? 1 : 0); curr != bucketEnd; curr++) {
                    ourXorSet ^= *curr;
                }

                std::string payload = encodeVarInt(1); // mode = Fingerprint
                payload += ourXorSet.getId(idSize);

                outputs.emplace_back(BoundOutput({
                    i == 0 ? lowerBound : prevBound,
                    getMinimalBound(*std::prev(curr), *curr),
                    std::move(payload)
                }));

                prevBound = outputs.back().end;
            }

            outputs.back().end = upperBound;
        }
    }

    std::string buildOutput() {
        std::string output;
        auto currBound = XorElem(0, "");
        uint64_t lastTimestampOut = 0;

        while (pendingOutputs.size()) {
            std::string o;

            auto &p = pendingOutputs.front();
            if (p.start < currBound) break;

            if (currBound != p.start) {
                o += encodeBound(p.start, lastTimestampOut);
                o += encodeVarInt(0); // mode = Skip
            }

            o += encodeBound(p.end, lastTimestampOut);
            o += p.payload;

            if (frameSizeLimit && output.size() + o.size() > frameSizeLimit) break;
            output += o;

            pendingOutputs.pop_front();

            currBound = p.end;
        }

        return output;
    }

    // Decoding

    std::string getBytes(std::string_view &encoded, size_t n) {
        if (encoded.size() < n) throw negentropy::err("parse ends prematurely");
        auto res = encoded.substr(0, n);
        encoded = encoded.substr(n);
        return std::string(res);
    };

    uint64_t decodeVarInt(std::string_view &encoded) {
        uint64_t res = 0;

        while (1) {
            if (encoded.size() == 0) throw negentropy::err("premature end of varint");
            uint64_t byte = encoded[0];
            encoded = encoded.substr(1);
            res = (res << 7) | (byte & 0b0111'1111);
            if ((byte & 0b1000'0000) == 0) break;
        }

        return res;
    }

    uint64_t decodeTimestampIn(std::string_view &encoded, uint64_t &lastTimestampIn) {
        uint64_t timestamp = decodeVarInt(encoded);
        timestamp = timestamp == 0 ? MAX_U64 : timestamp - 1;
        timestamp += lastTimestampIn;
        if (timestamp < lastTimestampIn) timestamp = MAX_U64; // saturate
        lastTimestampIn = timestamp;
        return timestamp;
    }

    XorElem decodeBound(std::string_view &encoded, uint64_t &lastTimestampIn) {
        auto timestamp = decodeTimestampIn(encoded, lastTimestampIn);
        auto len = decodeVarInt(encoded);
        return XorElem(timestamp, getBytes(encoded, len));
    }


    // Encoding

    std::string encodeVarInt(uint64_t n) {
        if (n == 0) return std::string(1, '\0');

        std::string o;

        while (n) {
            o.push_back(static_cast<unsigned char>(n & 0x7F));
            n >>= 7;
        }

        std::reverse(o.begin(), o.end());

        for (size_t i = 0; i < o.size() - 1; i++) {
            o[i] |= 0x80;
        }

        return o;
    }

    std::string encodeTimestampOut(uint64_t timestamp, uint64_t &lastTimestampOut) {
        if (timestamp == MAX_U64) {
            lastTimestampOut = MAX_U64;
            return encodeVarInt(0);
        }

        uint64_t temp = timestamp;
        timestamp -= lastTimestampOut;
        lastTimestampOut = temp;
        return encodeVarInt(timestamp + 1);
    };

    std::string encodeBound(const XorElem &bound, uint64_t &lastTimestampOut) {
        std::string output;

        output += encodeTimestampOut(bound.timestamp, lastTimestampOut);
        output += encodeVarInt(bound.idSize);
        output += bound.getId(idSize);

        return output;
    };

    XorElem getMinimalBound(const XorElem &prev, const XorElem &curr) {
        if (curr.timestamp != prev.timestamp) {
            return XorElem(curr.timestamp, "");
        } else {
            uint64_t sharedPrefixBytes = 0;
            auto currKey = curr.getId();
            auto prevKey = prev.getId();

            for (uint64_t i = 0; i < idSize; i++) {
                if (currKey[i] != prevKey[i]) break;
                sharedPrefixBytes++;
            }

            return XorElem(curr.timestamp, currKey.substr(0, sharedPrefixBytes + 1));
        }
    }

    std::string encodeBitField(const std::vector<uint64_t> inds) {
        if (inds.size() == 0) return "";
        uint64_t max = *std::max_element(inds.begin(), inds.end());

        std::string bitField = std::string((max + 8) / 8, '\0');
        for (auto ind : inds) bitField[ind / 8] |= 1 << ind % 8;

        return bitField;
    }

    bool bitFieldLookup(const std::string &bitField, uint64_t ind) {
        if ((ind + 8) / 8 > bitField.size()) return false;
        return !!(bitField[ind / 8] & 1 << (ind % 8));
    }
};


}


using Negentropy = negentropy::Negentropy;
