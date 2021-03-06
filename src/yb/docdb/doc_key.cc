// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/doc_key.h"

#include <memory>
#include <sstream>

#include "yb/rocksdb/util/string_util.h"

#include "yb/common/partition.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/doc_path.h"
#include "yb/docdb/value_type.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/util/enums.h"
#include "yb/util/compare_util.h"

using std::ostringstream;

using strings::Substitute;

using yb::util::to_underlying;
using yb::util::CompareVectors;
using yb::util::CompareUsingLessThan;

namespace yb {
namespace docdb {

namespace {

template<class Callback>
Status ConsumePrimitiveValuesFromKey(rocksdb::Slice* slice, Callback callback) {
  const auto initial_slice(*slice);  // For error reporting.
  while (true) {
    if (PREDICT_FALSE(slice->empty())) {
      return STATUS(Corruption, "Unexpected end of key when decoding document key");
    }
    ValueType current_value_type = static_cast<ValueType>(*slice->data());
    if (current_value_type == ValueType::kGroupEnd) {
      slice->consume_byte();
      return Status::OK();
    }
    if (PREDICT_FALSE(!IsPrimitiveValueType(current_value_type))) {
      return STATUS_FORMAT(Corruption,
          "Expected a primitive value type, got $0",
          current_value_type);
    }
    RETURN_NOT_OK_PREPEND(callback(),
        Substitute("while consuming primitive values from $0",
                   initial_slice.ToDebugHexString()));
  }
}

Status ConsumePrimitiveValuesFromKey(rocksdb::Slice* slice,
                                     boost::container::small_vector_base<Slice>* result) {
  return ConsumePrimitiveValuesFromKey(slice, [slice, result] {
    auto begin = slice->data();
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, nullptr));
    if (result) {
      result->emplace_back(begin, slice->data());
    }
    return Status::OK();
  });
}

void AppendDocKeyItems(const vector<PrimitiveValue>& doc_key_items, KeyBytes* result) {
  for (const PrimitiveValue& item : doc_key_items) {
    item.AppendToKey(result);
  }
  result->AppendValueType(ValueType::kGroupEnd);
}

} // namespace

Status ConsumePrimitiveValuesFromKey(rocksdb::Slice* slice,
                                     std::vector<PrimitiveValue>* result) {
  return ConsumePrimitiveValuesFromKey(slice, [slice, result] {
    result->emplace_back();
    return result->back().DecodeFromKey(slice);
  });
}

// ------------------------------------------------------------------------------------------------
// DocKey
// ------------------------------------------------------------------------------------------------

DocKey::DocKey() : hash_present_(false) {
}

DocKey::DocKey(const vector<PrimitiveValue>& range_components)
    : hash_present_(false),
      range_group_(range_components) {
}

DocKey::DocKey(DocKeyHash hash,
               const vector<PrimitiveValue>& hashed_components,
               const vector<PrimitiveValue>& range_components)
    : hash_present_(true),
      hash_(hash),
      hashed_group_(hashed_components),
      range_group_(range_components) {
}

KeyBytes DocKey::Encode() const {
  KeyBytes result;
  AppendTo(&result);
  return result;
}

void DocKey::AppendTo(KeyBytes* out) const {
  if (hash_present_) {
    // We are not setting the "more items in group" bit on the hash field because it is not part
    // of "hashed" or "range" groups.
    out->AppendValueType(ValueType::kUInt16Hash);
    out->AppendUInt16(hash_);
    AppendDocKeyItems(hashed_group_, out);
  }
  AppendDocKeyItems(range_group_, out);
}

void DocKey::Clear() {
  hash_present_ = false;
  hash_ = 0xdead;
  hashed_group_.clear();
  range_group_.clear();
}

void DocKey::ClearRangeComponents() {
  range_group_.clear();
}

namespace {

class DecodeDocKeyCallback {
 public:
  explicit DecodeDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return out_;
  }

  void SetHash(...) const {}
 private:
  boost::container::small_vector_base<Slice>* out_;
};

class DummyCallback {
 public:
  boost::container::small_vector_base<Slice>* hashed_group() const {
    return nullptr;
  }

  boost::container::small_vector_base<Slice>* range_group() const {
    return nullptr;
  }

  void SetHash(...) const {}

  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }
};

} // namespace

yb::Status DocKey::PartiallyDecode(rocksdb::Slice *slice,
                                   boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  return DoDecode(slice, DocKeyPart::WHOLE_DOC_KEY, DecodeDocKeyCallback(out));
}

Result<size_t> DocKey::EncodedSize(Slice slice, DocKeyPart part) {
  auto initial_begin = slice.cdata();
  RETURN_NOT_OK(DoDecode(&slice, part, DummyCallback()));
  return slice.cdata() - initial_begin;
}

class DocKey::DecodeFromCallback {
 public:
  explicit DecodeFromCallback(DocKey* key) : key_(key) {
  }

  std::vector<PrimitiveValue>* hashed_group() const {
    return &key_->hashed_group_;
  }

  std::vector<PrimitiveValue>* range_group() const {
    return &key_->range_group_;
  }

  void SetHash(bool present, DocKeyHash hash = 0) const {
    key_->hash_present_ = present;
    if (present) {
      key_->hash_ = hash;
    }
  }
 private:
  DocKey* key_;
};

yb::Status DocKey::DecodeFrom(rocksdb::Slice *slice, DocKeyPart part_to_decode) {
  Clear();

  return DoDecode(slice, part_to_decode, DecodeFromCallback(this));
}

template<class Callback>
yb::Status DocKey::DoDecode(rocksdb::Slice *slice,
                            DocKeyPart part_to_decode,
                            const Callback& callback) {
  if (slice->empty()) {
    return STATUS(Corruption, "Document key is empty");
  }
  if ((*slice)[0] == static_cast<uint8_t>(ValueType::kIntentPrefix)) {
    slice->consume_byte();
  }

  const ValueType first_value_type = static_cast<ValueType>(*slice->data());

  if (!IsPrimitiveValueType(first_value_type) && first_value_type != ValueType::kGroupEnd) {
    return STATUS_FORMAT(Corruption,
        "Expected first value type to be primitive or GroupEnd, got $0 in $1",
        first_value_type, slice->ToDebugHexString());
  }

  if (first_value_type == ValueType::kUInt16Hash) {
    if (slice->size() >= sizeof(DocKeyHash) + 1) {
      // We'll need to update this code if we ever change the size of the hash field.
      static_assert(sizeof(DocKeyHash) == sizeof(uint16_t),
          "It looks like the DocKeyHash's size has changed -- need to update encoder/decoder.");
      callback.SetHash(/* present */ true, BigEndian::Load16(slice->data() + 1));
      slice->remove_prefix(sizeof(DocKeyHash) + 1);
    } else {
      return STATUS_SUBSTITUTE(Corruption,
          "Could not decode a 16-bit hash component of a document key: only $0 bytes left",
          slice->size());
    }
    RETURN_NOT_OK_PREPEND(ConsumePrimitiveValuesFromKey(slice, callback.hashed_group()),
        "Error when decoding hashed components of a document key");
  } else {
    callback.SetHash(/* present */ false);
  }

  switch (part_to_decode) {
    case DocKeyPart::WHOLE_DOC_KEY:
      RETURN_NOT_OK_PREPEND(ConsumePrimitiveValuesFromKey(slice, callback.range_group()),
          "Error when decoding range components of a document key");
      return Status::OK();
    case DocKeyPart::HASHED_PART_ONLY:
      return Status::OK();
  }
  auto part_to_decode_printable =
      static_cast<std::underlying_type_t<DocKeyPart>>(part_to_decode);
  LOG(FATAL) << "Corrupted part_to_decode parameter: " << part_to_decode_printable;
  // TODO: should we abort process here to avoid data corruption, since we have memory corruption?
  return STATUS_SUBSTITUTE(Corruption, "Corrupted part_to_decode parameter: $0",
      part_to_decode_printable);
}

yb::Status DocKey::FullyDecodeFrom(const rocksdb::Slice& slice) {
  rocksdb::Slice mutable_slice = slice;
  Status status = DecodeFrom(&mutable_slice);
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into DocKey, found $0 extra bytes",
        mutable_slice.size());
  }
  return status;
}

string DocKey::ToString() const {
  string result = "DocKey(";
  if (hash_present_) {
    result += StringPrintf("0x%04x", hash_);
    result += ", ";
  }

  result += rocksdb::VectorToString(hashed_group_);
  result += ", ";
  result += rocksdb::VectorToString(range_group_);
  result.push_back(')');
  return result;
}

bool DocKey::operator ==(const DocKey& other) const {
  return HashedComponentsEqual(other) && range_group_ == other.range_group_;
}

bool DocKey::HashedComponentsEqual(const DocKey& other) const {
  return hash_present_ == other.hash_present_ &&
      // Only compare hashes and hashed groups if the hash presence flag is set.
      (!hash_present_ || (hash_ == other.hash_ && hashed_group_ == other.hashed_group_));
}

void DocKey::AddRangeComponent(const PrimitiveValue& val) {
  range_group_.push_back(val);
}

int DocKey::CompareTo(const DocKey& other) const {
  // Each table will only contain keys with hash present or absent, so we should never compare
  // keys from both categories.
  //
  // TODO: see how we can prevent this from ever happening in production. This might change
  //       if we decide to rethink DocDB's implementation of hash components as part of end-to-end
  //       integration of CQL's hash partition keys in December 2016.
  DCHECK_EQ(hash_present_, other.hash_present_);

  int result = 0;
  if (hash_present_) {
    result = CompareUsingLessThan(hash_, other.hash_);
    if (result != 0) return result;
  }
  result = CompareVectors(hashed_group_, other.hashed_group_);
  if (result != 0) return result;

  return CompareVectors(range_group_, other.range_group_);
}

DocKey DocKey::FromKuduEncodedKey(const EncodedKey &encoded_key, const Schema &schema) {
  DocKey new_doc_key;
  std::string hash_key;
  for (int i = 0; i < encoded_key.num_key_columns(); ++i) {
    bool hash_column = i < schema.num_hash_key_columns();
    auto& dest = hash_column ? new_doc_key.hashed_group_ : new_doc_key.range_group_;
    const auto& type_info = *schema.column(i).type_info();
    const void* const raw_key = encoded_key.raw_keys()[i];
    switch (type_info.type()) {
      case DataType::INT64:
        dest.emplace_back(*reinterpret_cast<const int64_t*>(raw_key));
        break;
      case DataType::INT32: {
          auto value = *reinterpret_cast<const int32_t*>(raw_key);
          dest.emplace_back(PrimitiveValue::Int32(value));
          if (hash_column) {
            YBPartition::AppendIntToKey<int32_t, uint32_t>(value, &hash_key);
          }
        } break;
      case DataType::INT16:
        dest.emplace_back(
            PrimitiveValue::Int32(*reinterpret_cast<const int16_t*>(raw_key)));
        break;
      case DataType::INT8:
        dest.emplace_back(
            PrimitiveValue::Int32(*reinterpret_cast<const int8_t*>(raw_key)));
        break;
      case DataType::STRING: FALLTHROUGH_INTENDED;
      case DataType::BINARY:
        dest.emplace_back(reinterpret_cast<const Slice*>(raw_key)->ToBuffer());
        break;

      default:
        LOG(FATAL) << "Decoding kudu data type " << type_info.name() << " is not supported";
    }
  }
  if (!hash_key.empty()) {
    new_doc_key.hash_present_ = true;
    new_doc_key.hash_ = YBPartition::HashColumnCompoundValue(hash_key);
  }
  return new_doc_key;
}

DocKey DocKey::FromRedisKey(uint16_t hash, const string &key) {
  DocKey new_doc_key;
  new_doc_key.hash_present_ = true;
  new_doc_key.hash_ = hash;
  new_doc_key.hashed_group_.emplace_back(key);
  return new_doc_key;
}

// ------------------------------------------------------------------------------------------------
// SubDocKey
// ------------------------------------------------------------------------------------------------

KeyBytes SubDocKey::Encode(bool include_hybrid_time) const {
  KeyBytes key_bytes = doc_key_.Encode();
  for (const auto& subkey : subkeys_) {
    subkey.AppendToKey(&key_bytes);
  }
  if (has_hybrid_time() && include_hybrid_time) {
    AppendDocHybridTime(doc_ht_, &key_bytes);
  }
  return key_bytes;
}

namespace {

class DecodeSubDocKeyCallback {
 public:
  explicit DecodeSubDocKeyCallback(boost::container::small_vector_base<Slice>* out) : out_(out) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return DocKey::PartiallyDecode(slice, out_);
  }

  // We don't need subkeys in partial decoding.
  PrimitiveValue* AddSubkey() const {
    return nullptr;
  }

  DocHybridTime& doc_hybrid_time() const {
    return doc_hybrid_time_;
  }

  void DocHybridTimeSlice(Slice slice) const {
    out_->push_back(slice);
  }
 private:
  boost::container::small_vector_base<Slice>* out_;
  mutable DocHybridTime doc_hybrid_time_;
};

} // namespace

Status SubDocKey::PartiallyDecode(Slice* slice, boost::container::small_vector_base<Slice>* out) {
  CHECK_NOTNULL(out);
  return DoDecode(slice, HybridTimeRequired::kTrue, DecodeSubDocKeyCallback(out));
}

class SubDocKey::DecodeCallback {
 public:
  explicit DecodeCallback(SubDocKey* key) : key_(key) {}

  CHECKED_STATUS DecodeDocKey(Slice* slice) const {
    return key_->doc_key_.DecodeFrom(slice);
  }

  PrimitiveValue* AddSubkey() const {
    key_->subkeys_.emplace_back();
    return &key_->subkeys_.back();
  }

  DocHybridTime& doc_hybrid_time() const {
    return key_->doc_ht_;
  }

  void DocHybridTimeSlice(Slice slice) const {
  }
 private:
  SubDocKey* key_;
};

Status SubDocKey::DecodeFrom(rocksdb::Slice* slice, HybridTimeRequired require_hybrid_time) {
  Clear();
  return DoDecode(slice, require_hybrid_time, DecodeCallback(this));
}

Result<bool> SubDocKey::DecodeSubkey(Slice* slice) {
  return DecodeSubkey(slice, DummyCallback());
}

template<class Callback>
Result<bool> SubDocKey::DecodeSubkey(Slice* slice, const Callback& callback) {
  if (!slice->empty() && *slice->data() != static_cast<char>(ValueType::kHybridTime)) {
    RETURN_NOT_OK(PrimitiveValue::DecodeKey(slice, callback.AddSubkey()));
    return true;
  }
  return false;
}

template<class Callback>
Status SubDocKey::DoDecode(rocksdb::Slice* slice,
                           const HybridTimeRequired require_hybrid_time,
                           const Callback& callback) {
  const rocksdb::Slice original_bytes(*slice);

  RETURN_NOT_OK(callback.DecodeDocKey(slice));
  for (;;) {
    auto decode_result = DecodeSubkey(slice, callback);
    RETURN_NOT_OK_PREPEND(
        decode_result,
        Substitute("While decoding SubDocKey $0", ToShortDebugStr(original_bytes)));
    if (!decode_result.get()) {
      break;
    }
  }
  if (slice->empty()) {
    if (!require_hybrid_time) {
      callback.doc_hybrid_time() = DocHybridTime::kInvalid;
      return Status::OK();
    }
    return STATUS_SUBSTITUTE(
        Corruption,
        "Found too few bytes in the end of a SubDocKey for a type-prefixed hybrid_time: $0",
        ToShortDebugStr(*slice));
  }

  // The reason the following is not handled as a Status is that the logic above (loop + emptiness
  // check) should guarantee this is the only possible case left.
  DCHECK_EQ(ValueType::kHybridTime, DecodeValueType(*slice));
  slice->consume_byte();

  auto begin = slice->data();
  RETURN_NOT_OK(ConsumeHybridTimeFromKey(slice, &callback.doc_hybrid_time()));
  callback.DocHybridTimeSlice(Slice(begin, slice->data()));

  return Status::OK();
}

Status SubDocKey::FullyDecodeFrom(const rocksdb::Slice& slice,
                                  HybridTimeRequired require_hybrid_time) {
  rocksdb::Slice mutable_slice = slice;
  Status status = DecodeFrom(&mutable_slice, require_hybrid_time);
  if (!mutable_slice.empty()) {
    return STATUS_SUBSTITUTE(InvalidArgument,
        "Expected all bytes of the slice to be decoded into DocKey, found $0 extra bytes: $1",
        mutable_slice.size(), ToShortDebugStr(mutable_slice));
  }
  return status;
}

std::string SubDocKey::DebugSliceToString(Slice slice) {
  SubDocKey key;
  auto status = key.FullyDecodeFrom(slice, HybridTimeRequired::kFalse);
  if (!status.ok()) {
    return status.ToString();
  }
  return key.ToString();
}

string SubDocKey::ToString() const {
  std::stringstream result;
  result << "SubDocKey(" << doc_key_.ToString() << ", [";

  bool need_comma = false;
  for (const auto& subkey : subkeys_) {
    if (need_comma) {
      result << ", ";
    }
    need_comma = true;
    result << subkey.ToString();
  }

  if (has_hybrid_time()) {
    if (need_comma) {
      result << "; ";
    }
    result << doc_ht_.ToString();
  }
  result << "])";
  return result.str();
}

Status SubDocKey::FromDocPath(const DocPath& doc_path) {
  RETURN_NOT_OK(doc_key_.FullyDecodeFrom(doc_path.encoded_doc_key().AsSlice()));
  subkeys_ = doc_path.subkeys();
  return Status::OK();
}

void SubDocKey::Clear() {
  doc_key_.Clear();
  subkeys_.clear();
  doc_ht_ = DocHybridTime::kInvalid;
}

bool SubDocKey::StartsWith(const SubDocKey& prefix) const {
  return doc_key_ == prefix.doc_key_ &&
         // Subkeys precede the hybrid_time field in the encoded representation, so the hybrid_time
         // either has to be undefined in the prefix, or the entire key must match, including
         // subkeys and the hybrid_time (in this case the prefix is the same as this key).
         (!prefix.has_hybrid_time() ||
          (doc_ht_ == prefix.doc_ht_ && prefix.num_subkeys() == num_subkeys())) &&
         prefix.num_subkeys() <= num_subkeys() &&
         // std::mismatch finds the first difference between two sequences. Prior to C++14, the
         // behavior is undefined if the second range is shorter than the first range, so we make
         // sure the potentially shorter range is first.
         std::mismatch(
             prefix.subkeys_.begin(), prefix.subkeys_.end(), subkeys_.begin()
         ).first == prefix.subkeys_.end();
}

bool SubDocKey::operator==(const SubDocKey& other) const {
  return doc_key_ == other.doc_key_ &&
         doc_ht_ == other.doc_ht_&&
         subkeys_ == other.subkeys_;
}

int SubDocKey::CompareTo(const SubDocKey& other) const {
  int result = CompareToIgnoreHt(other);
  if (result != 0) return result;

  // HybridTimes are sorted in reverse order.
  return -doc_ht_.CompareTo(other.doc_ht_);
}

int SubDocKey::CompareToIgnoreHt(const SubDocKey& other) const {
  int result = doc_key_.CompareTo(other.doc_key_);
  if (result != 0) return result;

  result = CompareVectors(subkeys_, other.subkeys_);
  return result;
}

string BestEffortDocDBKeyToStr(const KeyBytes &key_bytes) {
  rocksdb::Slice mutable_slice(key_bytes.AsSlice());
  SubDocKey subdoc_key;
  Status decode_status = subdoc_key.DecodeFrom(&mutable_slice, HybridTimeRequired::kFalse);
  if (decode_status.ok()) {
    ostringstream ss;
    if (!subdoc_key.has_hybrid_time() && subdoc_key.num_subkeys() == 0) {
      // This is really just a DocKey.
      ss << subdoc_key.doc_key().ToString();
    } else {
      ss << subdoc_key.ToString();
    }
    if (mutable_slice.size() > 0) {
      ss << " followed by raw bytes " << FormatRocksDBSliceAsStr(mutable_slice);
      // Can append the above status of why we could not decode a SubDocKey, if needed.
    }
    return ss.str();
  }

  // We could not decode a SubDocKey at all, even without a hybrid_time.
  return key_bytes.ToString();
}

std::string BestEffortDocDBKeyToStr(const rocksdb::Slice& slice) {
  return BestEffortDocDBKeyToStr(KeyBytes(slice));
}

int SubDocKey::NumSharedPrefixComponents(const SubDocKey& other) const {
  if (doc_key_ != other.doc_key_) {
    return 0;
  }
  const int min_num_subkeys = min(num_subkeys(), other.num_subkeys());
  for (int i = 0; i < min_num_subkeys; ++i) {
    if (subkeys_[i] != other.subkeys_[i]) {
      // If we found a mismatch at the first subkey (i = 0), but the DocKey matches, we return 1.
      // If one subkey matches but the second one (i = 1) is a mismatch, we return 2, etc.
      return i + 1;
    }
  }
  // The DocKey and all subkeys match up until the subkeys in one of the SubDocKeys are exhausted.
  return min_num_subkeys + 1;
}

KeyBytes SubDocKey::AdvanceOutOfSubDoc() const {
  KeyBytes subdoc_key_no_ts = Encode(/* include_hybrid_time = */ false);
  subdoc_key_no_ts.AppendValueType(ValueType::kMaxByte);
  return subdoc_key_no_ts;
}

KeyBytes SubDocKey::AdvanceOutOfDocKeyPrefix() const {
  // To construct key bytes that will seek past this DocKey and DocKeys that have the same hash
  // components but add more range components to it, we will strip the group-end of the range
  // components and append 0xff, which will be lexicographically higher than any key bytes
  // with the same hash and range component prefix. For example,
  //
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd"])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00!
  // Result:  H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00\xff
  // This key will also skip all DocKeys that have additional range components, e.g.
  // DocKey(0x1234, ["aa", "bb"], ["cc", "dd", "ee"])
  // (encoded as H\0x12\0x34$aa\x00\x00$bb\x00\x00!$cc\x00\x00$dd\x00\x00$ee\x00\00!). That should
  // make no difference to DocRowwiseIterator in a valid database, because all keys actually stored
  // in DocDB will have exactly the same number of range components.
  //
  // Now, suppose there are no range components in the key passed to us (note: that does not
  // necessarily mean there are no range components in the schema, just the doc key being passed to
  // us is a custom-constructed DocKey with no range components because the caller wants a key
  // that will skip pass all doc keys with the same hash components prefix). Example:
  //
  // DocKey(0x1234, ["aa", "bb"], [])
  // Encoded: H\0x12\0x34$aa\x00\x00$bb\x00\x00!!
  // Result: H\0x12\0x34$aa\x00\x00$bb\x00\x00!\xff
  KeyBytes doc_key_encoded = doc_key_.Encode();
  doc_key_encoded.RemoveValueTypeSuffix(ValueType::kGroupEnd);
  doc_key_encoded.AppendValueType(ValueType::kMaxByte);
  return doc_key_encoded;
}

// ------------------------------------------------------------------------------------------------
// DocDbAwareFilterPolicy
// ------------------------------------------------------------------------------------------------

namespace {

class HashedComponentsExtractor : public rocksdb::FilterPolicy::KeyTransformer {
 public:
  HashedComponentsExtractor() {}
  HashedComponentsExtractor(const HashedComponentsExtractor&) = delete;
  HashedComponentsExtractor& operator=(const HashedComponentsExtractor&) = delete;

  static HashedComponentsExtractor& GetInstance() {
    static HashedComponentsExtractor instance;
    return instance;
  }

  Slice Transform(Slice key) const override {
    auto size = DocKey::EncodedSize(key, DocKeyPart::HASHED_PART_ONLY);
    CHECK_OK(size);
    return Slice(key.data(), *size);
  }
};

} // namespace


void DocDbAwareFilterPolicy::CreateFilter(
    const rocksdb::Slice* keys, int n, std::string* dst) const {
  CHECK_GT(n, 0);
  return builtin_policy_->CreateFilter(keys, n, dst);
}

bool DocDbAwareFilterPolicy::KeyMayMatch(
    const rocksdb::Slice& key, const rocksdb::Slice& filter) const {
  return builtin_policy_->KeyMayMatch(key, filter);
}

rocksdb::FilterBitsBuilder* DocDbAwareFilterPolicy::GetFilterBitsBuilder() const {
  return builtin_policy_->GetFilterBitsBuilder();
}

rocksdb::FilterBitsReader* DocDbAwareFilterPolicy::GetFilterBitsReader(
    const rocksdb::Slice& contents) const {
  return builtin_policy_->GetFilterBitsReader(contents);
}

rocksdb::FilterPolicy::FilterType DocDbAwareFilterPolicy::GetFilterType() const {
  return builtin_policy_->GetFilterType();
}

const rocksdb::FilterPolicy::KeyTransformer* DocDbAwareFilterPolicy::GetKeyTransformer() const {
  return &HashedComponentsExtractor::GetInstance();
}

}  // namespace docdb

}  // namespace yb
