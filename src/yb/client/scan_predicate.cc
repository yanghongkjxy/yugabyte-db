// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include "yb/client/scan_predicate.h"
#include "yb/client/scan_predicate-internal.h"
#include "yb/client/value.h"
#include "yb/client/value-internal.h"

#include "yb/common/scan_spec.h"
#include "yb/common/scan_predicate.h"

#include "yb/gutil/strings/substitute.h"

using strings::Substitute;

namespace yb {
namespace client {

YBPredicate::YBPredicate(Data* d)
  : data_(d) {
}

YBPredicate::~YBPredicate() {
  delete data_;
}

YBPredicate::Data::Data() {
}

YBPredicate::Data::~Data() {
}

YBPredicate* YBPredicate::Clone() const {
  return new YBPredicate(data_->Clone());
}

ComparisonPredicateData::ComparisonPredicateData(ColumnSchema col,
                                                 YBPredicate::ComparisonOp op,
                                                 YBValue* val)
    : col_(std::move(col)),
      op_(op),
      val_(val) {
}
ComparisonPredicateData::~ComparisonPredicateData() {
}


Status ComparisonPredicateData::AddToScanSpec(ScanSpec* spec) {
  void* val_void;
  RETURN_NOT_OK(val_->data_->CheckTypeAndGetPointer(col_.name(),
                                                    col_.type(),
                                                    &val_void));

  void* lower_bound = nullptr;
  void* upper_bound = nullptr;
  switch (op_) {
    case YBPredicate::LESS_EQUAL:
      upper_bound = val_void;
      break;
    case YBPredicate::GREATER_EQUAL:
      lower_bound = val_void;
      break;
    case YBPredicate::EQUAL:
      lower_bound = upper_bound = val_void;
      break;
    default:
      return STATUS(InvalidArgument, Substitute("invalid comparison op: $0", op_));
  }

  ColumnRangePredicate p(col_, lower_bound, upper_bound);
  spec->AddPredicate(p);

  return Status::OK();
}

} // namespace client
} // namespace yb
