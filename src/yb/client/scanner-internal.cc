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

#include "yb/client/scanner-internal.h"

#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "yb/gutil/strings/substitute.h"
#include "yb/util/hexdump.h"
#include "yb/common/transaction.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/client/client-internal.h"
#include "yb/client/transaction.h"
#include "yb/client/meta_cache.h"
#include "yb/client/row_result.h"
#include "yb/client/table-internal.h"

using std::set;
using std::string;

namespace yb {

using rpc::RpcController;
using strings::Substitute;
using strings::SubstituteAndAppend;
using tserver::ColumnRangePredicatePB;
using tserver::NewScanRequestPB;
using tserver::ScanResponsePB;

namespace client {

using internal::RemoteTabletServer;

YBScanner::Data::Data(YBTable* table, const YBTransactionPtr& transaction)
  : open_(false),
    data_in_open_(false),
    has_batch_size_bytes_(false),
    batch_size_bytes_(0),
    selection_(YBClient::CLOSEST_REPLICA),
    is_fault_tolerant_(false),
    table_(DCHECK_NOTNULL(table)),
    arena_(1024, 1024*1024),
    spec_encoder_(&internal::GetSchema(table->schema()), &arena_),
    timeout_(MonoDelta::FromMilliseconds(kScanTimeoutMillis)),
    scan_attempts_(0),
    transaction_(transaction) {
  SetProjectionSchema(&internal::GetSchema(table->schema()));
}

YBScanner::Data::~Data() {
}

Status YBScanner::Data::CheckForErrors() {
  if (PREDICT_TRUE(!last_response_.has_error())) {
    return Status::OK();
  }

  return StatusFromPB(last_response_.error().status());
}

void YBScanner::Data::CopyPredicateBound(const ColumnSchema& col,
                                           const void* bound_src,
                                           string* bound_dst) {
  const void* src;
  size_t size;
  if (col.type_info()->physical_type() == BINARY) {
    // Copying a string involves an extra level of indirection through its
    // owning slice.
    const Slice* s = reinterpret_cast<const Slice*>(bound_src);
    src = s->data();
    size = s->size();
  } else {
    src = bound_src;
    size = col.type_info()->size();
  }
  bound_dst->assign(reinterpret_cast<const char*>(src), size);
}

Status YBScanner::Data::CanBeRetried(const bool isNewScan,
                                     const Status& rpc_status, const Status& server_status,
                                     const MonoTime& actual_deadline, const MonoTime& deadline,
                                     const vector<RemoteTabletServer*>& candidates,
                                     set<string>* blacklist) {
  CHECK(!rpc_status.ok() || !server_status.ok());

  // Check for ERROR_SERVER_TOO_BUSY, which should result in a retry after a delay.
  if (server_status.ok() &&
      !rpc_status.ok() &&
      controller_.error_response() &&
      controller_.error_response()->code() == rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY) {
    UpdateLastError(rpc_status);

    // Exponential backoff with jitter anchored between 10ms and 20ms, and an
    // upper bound between 2.5s and 5s.
    MonoDelta sleep = MonoDelta::FromMilliseconds(
        (10 + random() % 10) * static_cast<int>(std::pow(2.0, std::min(8, scan_attempts_ - 1))));
    MonoTime now = MonoTime::Now();
    now.AddDelta(sleep);
    if (deadline.ComesBefore(now)) {
      Status ret = STATUS(TimedOut, "unable to retry before timeout",
                                    rpc_status.ToString());
      return last_error_.ok() ?
          ret : ret.CloneAndAppend(last_error_.ToString());
    }
    LOG(INFO) << "Retrying scan to busy tablet server " << ts_->ToString()
              << " after " << sleep.ToString() << "; attempt " << scan_attempts_;
    SleepFor(sleep);
    return Status::OK();
  }

  // Start by checking network errors.
  if (!rpc_status.ok()) {
    if (rpc_status.IsTimedOut() && actual_deadline.Equals(deadline)) {
      // If we ended because of the overall deadline, we're done.
      // We didn't wait a full RPC timeout though, so don't mark the tserver as failed.
      LOG(INFO) << "Scan of tablet " << remote_->tablet_id() << " at "
          << ts_->ToString() << " deadline expired.";
      return last_error_.ok()
          ? rpc_status : rpc_status.CloneAndAppend(last_error_.ToString());
    } else {
      // All other types of network errors are retriable, and also indicate the tserver is failed.
      UpdateLastError(rpc_status);
      table_->client()->data_->meta_cache_->MarkTSFailed(ts_, rpc_status);
    }
  }

  // If we're in the middle of a batch and doing a non fault-tolerant scan, then
  // we cannot retry. Non fault-tolerant scans can still be retried on a tablet
  // boundary (i.e. an OpenTablet call).
  if (!isNewScan && !is_fault_tolerant_) {
    return !rpc_status.ok() ? rpc_status : server_status;
  }

  // For retries, the correct action depends on the particular failure condition.
  //
  // On an RPC error, we retry at a different tablet server.
  //
  // If the server returned an error code, it depends:
  //
  //   - SCANNER_EXPIRED           : The scan can be retried at the same tablet server.
  //
  //   - TABLET_NOT_RUNNING        : The scan can be retried at a different tablet server, subject
  //                                 to the client's specified selection criteria.
  //
  //   - TABLET_NOT_FOUND          : The scan can be retried at a different tablet server, subject
  //                                 to the client's specified selection criteria.
  //                                 The metadata for this tablet should be refreshed.
  //
  //   - NOT_THE_LEADER            : The scan must be retried at the leader, because this tablet
  //                                 server is no longer the leader.
  //                                 The metadata for this tablet should be refreshed.
  //
  //   - LEADER_NOT_READY_TO SERVE : The scan must be retried at the same tablet server, because
  //                                 this tablet server is the leader, but it's not ready to serve
  //                                 up-to-date reads yet.
  //
  //   - Any other error           : Fatal. This indicates an unexpected error while processing the
  //                                 scan request.
  if (rpc_status.ok() && !server_status.ok()) {
    UpdateLastError(server_status);

    const tserver::TabletServerErrorPB& error = last_response_.error();
    switch (error.code()) {
      case tserver::TabletServerErrorPB::SCANNER_EXPIRED:
        VLOG(1) << "Got error code " << tserver::TabletServerErrorPB::Code_Name(error.code())
            << ": non-fatal error.";
        break;
      case tserver::TabletServerErrorPB::LEADER_NOT_READY_TO_SERVE: {
        VLOG(1) << "Got error code " << tserver::TabletServerErrorPB::Code_Name(error.code())
            << ": non-fatal error.";
        // The Leader was successfully elected, but it's not ready to serve requests.
        // Let's give him 200ms to become ready (to commit sync NoOp request) and retry.
        // Such simplified error-handling mechanism is only OK because
        // this is not a production codepath.
        const MonoDelta sleep_delta = MonoDelta::FromMilliseconds(200);
        SleepFor(sleep_delta);
        break;
      }
      case tserver::TabletServerErrorPB::TABLET_NOT_RUNNING:
        VLOG(1) << "Got error code " << tserver::TabletServerErrorPB::Code_Name(error.code())
            << ": temporarily blacklisting node " << ts_->permanent_uuid();
        blacklist->insert(ts_->permanent_uuid());
        // We've blacklisted all the live candidate tservers.
        // Do a short random sleep, clear the temp blacklist, then do another round of retries.
        if (!candidates.empty() && candidates.size() == blacklist->size()) {
          const MonoDelta sleep_delta = MonoDelta::FromMilliseconds((random() % 5000) + 1000);
          LOG(INFO) << "All live candidate nodes are unavailable because of transient errors."
              << " Sleeping for " << sleep_delta.ToMilliseconds() << " ms before trying again.";
          SleepFor(sleep_delta);
          blacklist->clear();
        }
        break;
      case tserver::TabletServerErrorPB::TABLET_NOT_FOUND:
      case tserver::TabletServerErrorPB::NOT_THE_LEADER: {
        // There was either a tablet configuration change, leader change or the table was
        // deleted, since at the time of this writing we don't support splits.
        // Backoff, then force a re-fetch of the tablet metadata.
        remote_->MarkStale();
        // TODO: Only backoff on the second time we hit TABLET_NOT_FOUND on the
        // same tablet (see KUDU-1314).
        const MonoDelta backoff_time = MonoDelta::FromMilliseconds((random() % 1000) + 500);
        SleepFor(backoff_time);
        VLOG(1) << "Tried to make a request to a non-leader or tablet. Refreshing metadata. "
                << "Error Code: " << tserver::TabletServerErrorPB::Code_Name(error.code());
        break;
      }
      default:
        // All other server errors are fatal. Usually indicates a malformed request, e.g. a bad scan
        // specification.
        return server_status;
    }
  }

  return Status::OK();
}

Status YBScanner::Data::OpenTablet(const string& partition_key,
                                   const MonoTime& deadline,
                                   set<string>* blacklist) {

  PrepareRequest(YBScanner::Data::NEW);
  next_req_.clear_scanner_id();
  // Set leader only parameter based on the selection.
  next_req_.set_leader_only(selection_ == YBClient::LEADER_ONLY);
  if (transaction_) {
    next_req_.set_transaction_id(transaction_->id().begin(), transaction_->id().size());
  } else {
    next_req_.clear_transaction_id();
  }

  NewScanRequestPB* scan = next_req_.mutable_new_scan_request();

  if (is_fault_tolerant_) {
    scan->set_order_mode(yb::ORDERED);
  } else {
    scan->set_order_mode(yb::UNORDERED);
  }

  if (last_primary_key_.length() > 0) {
    VLOG(1) << "Setting NewScanRequestPB last_primary_key to hex value "
        << HexDump(last_primary_key_);
    scan->set_last_primary_key(last_primary_key_);
  }

  scan->set_cache_blocks(spec_.cache_blocks());

  // Set up the predicates.
  scan->clear_range_predicates();
  for (const ColumnRangePredicate& pred : spec_.predicates()) {
    const ColumnSchema& col = pred.column();
    const ValueRange& range = pred.range();
    ColumnRangePredicatePB* pb = scan->add_range_predicates();
    if (range.has_lower_bound()) {
      CopyPredicateBound(col, range.lower_bound(),
                         pb->mutable_lower_bound());
    }
    if (range.has_upper_bound()) {
      CopyPredicateBound(col, range.upper_bound(),
                         pb->mutable_upper_bound());
    }
    ColumnSchemaToPB(col, pb->mutable_column());
  }

  if (spec_.lower_bound_key()) {
    scan->mutable_start_primary_key()->assign(
      reinterpret_cast<const char*>(spec_.lower_bound_key()->encoded_key().data()),
      spec_.lower_bound_key()->encoded_key().size());
  } else {
    scan->clear_start_primary_key();
  }
  if (spec_.exclusive_upper_bound_key()) {
    scan->mutable_stop_primary_key()->assign(
      reinterpret_cast<const char*>(spec_.exclusive_upper_bound_key()->encoded_key().data()),
      spec_.exclusive_upper_bound_key()->encoded_key().size());
  } else {
    scan->clear_stop_primary_key();
  }
  RETURN_NOT_OK(SchemaToColumnPBs(*projection_, scan->mutable_projected_columns(),
                                  SCHEMA_PB_WITHOUT_IDS));

  for (int attempt = 1;; attempt++) {
    Synchronizer sync;
    table_->client()->data_->meta_cache_->LookupTabletByKey(table_,
                                                            partition_key,
                                                            deadline,
                                                            &remote_,
                                                            sync.AsStatusCallback());
    RETURN_NOT_OK(sync.Wait());

    scan->set_tablet_id(remote_->tablet_id());

    RemoteTabletServer *ts;
    vector<RemoteTabletServer*> candidates;
    Status lookup_status = table_->client()->data_->GetTabletServer(
        table_->client(),
        remote_,
        selection_,
        *blacklist,
        &candidates,
        &ts);
    // If we get ServiceUnavailable, this indicates that the tablet doesn't
    // currently have any known leader. We should sleep and retry, since
    // it's likely that the tablet is undergoing a leader election and will
    // soon have one.
    if (lookup_status.IsServiceUnavailable() &&
        MonoTime::Now().ComesBefore(deadline)) {
      int sleep_ms = attempt * 100;
      VLOG(1) << "Tablet " << remote_->tablet_id() << " current unavailable: "
              << lookup_status.ToString() << ". Sleeping for " << sleep_ms << "ms "
              << "and retrying...";
      SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
      continue;
    }
    RETURN_NOT_OK(lookup_status);

    MonoTime now = MonoTime::Now();
    if (deadline.ComesBefore(now)) {
      Status ret = STATUS(TimedOut, "Scan timed out, deadline expired");
      return last_error_.ok() ?
          ret : ret.CloneAndAppend(last_error_.ToString());
    }

    // Recalculate the deadlines.
    // If we have other replicas beyond this one to try, then we'll try to
    // open the scanner with the default RPC timeout. That gives us time to
    // try other replicas later. Otherwise, we open the scanner using the
    // full remaining deadline for the user's call.
    MonoTime rpc_deadline;
    if (static_cast<int>(candidates.size()) - blacklist->size() > 1) {
      rpc_deadline = now;
      rpc_deadline.AddDelta(table_->client()->default_rpc_timeout());
      rpc_deadline = MonoTime::Earliest(deadline, rpc_deadline);
    } else {
      rpc_deadline = deadline;
    }

    controller_.Reset();
    controller_.set_deadline(rpc_deadline);

    CHECK(ts->proxy());
    ts_ = CHECK_NOTNULL(ts);
    proxy_ = ts->proxy();
    const Status rpc_status = proxy_->Scan(next_req_, &last_response_, &controller_);
    const Status server_status = CheckForErrors();
    if (rpc_status.ok() && server_status.ok()) {
      scan_attempts_ = 0;
      break;
    }
    scan_attempts_++;
    RETURN_NOT_OK(CanBeRetried(true, rpc_status, server_status, rpc_deadline, deadline,
                               candidates, blacklist));
  }

  next_req_.clear_new_scan_request();
  data_in_open_ = last_response_.has_data();
  if (last_response_.has_more_results()) {
    next_req_.set_scanner_id(last_response_.scanner_id());
    VLOG(1) << "Opened tablet " << remote_->tablet_id()
            << ", scanner ID " << last_response_.scanner_id();
  } else if (last_response_.has_data()) {
    VLOG(1) << "Opened tablet " << remote_->tablet_id() << ", no scanner ID assigned";
  } else {
    VLOG(1) << "Opened tablet " << remote_->tablet_id() << " (no rows), no scanner ID assigned";
  }

  // If present in the response, set the snapshot hybrid_time and the encoded last
  // primary key.  This is used when retrying the scan elsewhere.  The last
  // primary key is also updated on each scan response.
  if (is_fault_tolerant_) {
    if (last_response_.has_last_primary_key()) {
      last_primary_key_ = last_response_.last_primary_key();
    }
  }

  if (last_response_.has_snap_hybrid_time()) {
    table_->client()->data_->UpdateLatestObservedHybridTime(last_response_.snap_hybrid_time());
  }

  return Status::OK();
}

Status YBScanner::Data::KeepAlive() {
  if (!open_) return STATUS(IllegalState, "Scanner was not open.");
  // If there is no scanner to keep alive, we still return Status::OK().
  if (!last_response_.IsInitialized() || !last_response_.has_more_results() ||
      !next_req_.has_scanner_id()) {
    return Status::OK();
  }

  RpcController controller;
  controller.set_timeout(timeout_);
  tserver::ScannerKeepAliveRequestPB request;
  request.set_scanner_id(next_req_.scanner_id());
  tserver::ScannerKeepAliveResponsePB response;
  RETURN_NOT_OK(proxy_->ScannerKeepAlive(request, &response, &controller));
  if (response.has_error()) {
    return StatusFromPB(response.error().status());
  }
  return Status::OK();
}

bool YBScanner::Data::MoreTablets() const {
  CHECK(open_);
  // TODO(KUDU-565): add a test which has a scan end on a tablet boundary

  if (remote_->partition().partition_key_end().empty()) {
    // Last tablet -- nothing more to scan.
    return false;
  }

  if (!spec_.exclusive_upper_bound_partition_key().empty() &&
      spec_.exclusive_upper_bound_partition_key() <= remote_->partition().partition_key_end()) {
    // We are not past the scan's upper bound partition key.
    return false;
  }

  if (spec_.exclusive_upper_bound_key() == nullptr) {
    // No upper bound - keep going!
    return true;
  }

  // Otherwise, we have to compare the upper bound.
  return spec_.exclusive_upper_bound_key()->encoded_key()
          .compare(remote_->partition().partition_key_end()) > 0;
}

void YBScanner::Data::PrepareRequest(RequestType state) {
  if (state == YBScanner::Data::CLOSE) {
    next_req_.set_batch_size_bytes(0);
  } else if (has_batch_size_bytes_) {
    next_req_.set_batch_size_bytes(batch_size_bytes_);
  } else {
    next_req_.clear_batch_size_bytes();
  }

  if (state == YBScanner::Data::NEW) {
    next_req_.set_call_seq_id(0);
  } else {
    next_req_.set_call_seq_id(next_req_.call_seq_id() + 1);
  }
}

void YBScanner::Data::UpdateLastError(const Status& error) {
  if (last_error_.ok() || last_error_.IsTimedOut()) {
    last_error_ = error;
  }
}

void YBScanner::Data::SetProjectionSchema(const Schema* schema) {
  projection_ = schema;
  client_projection_ = YBSchema(*schema);
}



////////////////////////////////////////////////////////////
// YBScanBatch
////////////////////////////////////////////////////////////

YBScanBatch::Data::Data() : projection_(NULL) {}

YBScanBatch::Data::~Data() {}

size_t YBScanBatch::Data::CalculateProjectedRowSize(const Schema& proj) {
  return proj.byte_size() +
        (proj.has_nullables() ? BitmapSize(proj.num_columns()) : 0);
}

Status YBScanBatch::Data::Reset(RpcController* controller,
                                  const Schema* projection,
                                  const YBSchema* client_projection,
                                  gscoped_ptr<RowwiseRowBlockPB> data) {
  CHECK(controller->finished());
  controller_.Swap(controller);
  projection_ = projection;
  client_projection_ = client_projection;
  resp_data_.Swap(data.get());

  // First, rewrite the relative addresses into absolute ones.
  if (PREDICT_FALSE(!resp_data_.has_rows_sidecar())) {
    return STATUS(Corruption, "Server sent invalid response: no row data");
  } else {
    Status s = controller_.GetSidecar(resp_data_.rows_sidecar(), &direct_data_);
    if (!s.ok()) {
      return STATUS(Corruption, "Server sent invalid response: row data "
                                "sidecar index corrupt", s.ToString());
    }
  }

  if (resp_data_.has_indirect_data_sidecar()) {
    Status s = controller_.GetSidecar(resp_data_.indirect_data_sidecar(),
                                      &indirect_data_);
    if (!s.ok()) {
      return STATUS(Corruption, "Server sent invalid response: indirect data "
                                "sidecar index corrupt", s.ToString());
    }
  }

  RETURN_NOT_OK(RewriteRowBlockPointers(*projection_, resp_data_, indirect_data_, &direct_data_));
  projected_row_size_ = CalculateProjectedRowSize(*projection_);
  return Status::OK();
}

void YBScanBatch::Data::ExtractRows(vector<YBScanBatch::RowPtr>* rows) {
  int n_rows = resp_data_.num_rows();
  rows->resize(n_rows);

  if (PREDICT_FALSE(n_rows == 0)) {
    // Early-out here to avoid a UBSAN failure.
    VLOG(1) << "Extracted 0 rows";
    return;
  }

  // Initialize each RowPtr with data from the response.
  //
  // Doing this resize and array indexing turns out to be noticeably faster
  // than using reserve and push_back.
  const uint8_t* src = direct_data_.data();
  YBScanBatch::RowPtr* dst = &(*rows)[0];
  while (n_rows > 0) {
    *dst = YBScanBatch::RowPtr(projection_, client_projection_, src);
    dst++;
    src += projected_row_size_;
    n_rows--;
  }
  VLOG(1) << "Extracted " << rows->size() << " rows";
}

void YBScanBatch::Data::Clear() {
  resp_data_.Clear();
  controller_.Reset();
}

} // namespace client
} // namespace yb
