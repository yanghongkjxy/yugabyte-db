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

#ifndef YB_TSERVER_TABLET_SERVER_TEST_BASE_H_
#define YB_TSERVER_TABLET_SERVER_TEST_BASE_H_

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

#include "yb/client/yb_table_name.h"
#include "yb/common/wire_protocol-test-util.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/consensus/log_reader.h"
#include "yb/gutil/atomicops.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/messenger.h"
#include "yb/server/server_base.proxy.h"
#include "yb/tablet/local_tablet_writer.h"
#include "yb/tablet/maintenance_manager.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/remote_bootstrap.proxy.h"
#include "yb/tserver/scanners.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/tablet_server_test_util.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/util/metrics.h"
#include "yb/util/test_graph.h"
#include "yb/util/test_util.h"

DEFINE_int32(rpc_timeout, 1000, "Timeout for RPC calls, in seconds");
DEFINE_int32(num_updater_threads, 1, "Number of updating threads to launch");
DECLARE_bool(durable_wal_write);
DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(enable_data_block_fsync);
DECLARE_int32(heartbeat_rpc_timeout_ms);

METRIC_DEFINE_entity(test);

namespace yb {
namespace client {
class YBTableName;
}
namespace tserver {

class TabletServerTestBase : public YBTest {
 public:
  typedef pair<int32_t, int32_t> KeyValue;

  explicit TabletServerTestBase(TableType table_type = YQL_TABLE_TYPE)
      : schema_(GetSimpleTestSchema()),
        table_type_(table_type),
        ts_test_metric_entity_(METRIC_ENTITY_test.Instantiate(
                                   &ts_test_metric_registry_, "ts_server-test")) {

    // Disable the maintenance ops manager since we want to trigger our own
    // maintenance operations at predetermined times.
    FLAGS_enable_maintenance_manager = false;

    // Decrease heartbeat timeout: we keep re-trying heartbeats when a
    // single master server fails due to a network error. Decreasing
    // the hearbeat timeout to 1 second speeds up unit tests which
    // purposefully specify non-running Master servers.
    FLAGS_heartbeat_rpc_timeout_ms = 1000;

    // Keep unit tests fast, but only if no one has set the flag explicitly.
    if (google::GetCommandLineFlagInfoOrDie("enable_data_block_fsync").is_default) {
      FLAGS_enable_data_block_fsync = false;
    }
  }

  // Starts the tablet server, override to start it later.
  virtual void SetUp() override {
    YBTest::SetUp();

    key_schema_ = schema_.CreateKeyProjection();
    rb_.reset(new RowBuilder(schema_));

    rpc::MessengerBuilder bld("Client");
    ASSERT_OK(bld.Build().MoveTo(&client_messenger_));
  }

  virtual void StartTabletServer() {
    // Start server with an invalid master address, so it never successfully
    // heartbeats, even if there happens to be a master running on this machine.
    mini_server_.reset(new MiniTabletServer(GetTestPath("TabletServerTest-fsroot"), 0));
    auto addr = std::make_shared<vector<HostPort>>();
    addr->push_back(HostPort("255.255.255.255", 1));
    mini_server_->options()->SetMasterAddresses(addr);
    CHECK_OK(mini_server_->Start());

    // Set up a tablet inside the server.
    CHECK_OK(mini_server_->AddTestTablet(kTableName.table_name(), kTabletId, schema_, table_type_));
    CHECK(mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet_peer_));

    // Creating a tablet is async, we wait here instead of having to handle errors later.
    CHECK_OK(WaitForTabletRunning(kTabletId));

    // Connect to it.
    ResetClientProxies();
  }

  CHECKED_STATUS WaitForTabletRunning(const char *tablet_id) {
    scoped_refptr<tablet::TabletPeer> tablet_peer;
    RETURN_NOT_OK(mini_server_->server()->tablet_manager()->GetTabletPeer(tablet_id, &tablet_peer));

    // Sometimes the disk can be really slow and hence we need a high timeout to wait for consensus.
    RETURN_NOT_OK(tablet_peer->WaitUntilConsensusRunning(MonoDelta::FromSeconds(60)));

    RETURN_NOT_OK(tablet_peer->consensus()->EmulateElection());

    // Wait to ensure there are no pending transitions for the tablet.
    const MonoDelta timeout(MonoDelta::FromSeconds(10));
    const MonoTime start(MonoTime::Now());
    while (mini_server_->server()->tablet_manager()->IsTabletInTransition(tablet_id)) {
      MonoTime now(MonoTime::Now());
      MonoDelta elapsed(now.GetDeltaSince(start));
      if (elapsed.MoreThan(timeout)) {
        return STATUS(TimedOut, strings::Substitute(
            "State transitions are still pending after waiting for $0 for tablet $1",
            elapsed.ToString(), tablet_id));
      }
      SleepFor(MonoDelta::FromMilliseconds(100));
    }
    return Status::OK();
  }

  void UpdateTestRowRemote(int tid,
                           int64_t row_idx,
                           int32_t new_val,
                           TimeSeries *ts = NULL) {

    WriteRequestPB req;
    req.set_tablet_id(kTabletId);

    WriteResponsePB resp;
    rpc::RpcController controller;
    controller.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));
    string new_string_val(strings::Substitute("mutated$0", row_idx));

    AddTestRowUpdate(row_idx, new_val, new_string_val, &req);
    ASSERT_OK(proxy_->Write(req, &resp, &controller));

    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error())<< resp.ShortDebugString();
    ASSERT_EQ(0, resp.per_row_errors_size());
    if (ts) {
      ts->AddValue(1);
    }
  }

  void ResetClientProxies() {
    CreateTsClientProxies(mini_server_->bound_rpc_addr(),
                          client_messenger_,
                          &proxy_, &admin_proxy_, &consensus_proxy_, &generic_proxy_);
  }

  // Inserts 'num_rows' test rows directly into the tablet (i.e not via RPC)
  void InsertTestRowsDirect(int64_t start_row, uint64_t num_rows) {
    tablet::LocalTabletWriter writer(tablet_peer_->tablet());
    QLWriteRequestPB req;
    for (int64_t i = 0; i < num_rows; i++) {
      BuildTestRow(start_row + i, &req);
      CHECK_OK(writer.Write(&req));
    }
  }

  // Inserts 'num_rows' test rows remotely into the tablet (i.e via RPC)
  // Rows are grouped in batches of 'count'/'num_batches' size.
  // Batch size defaults to 1.
  void InsertTestRowsRemote(int tid,
                            int64_t first_row,
                            uint64_t count,
                            uint64_t num_batches = -1,
                            TabletServerServiceProxy* proxy = NULL,
                            string tablet_id = kTabletId,
                            vector<uint64_t>* write_hybrid_times_collector = NULL,
                            TimeSeries *ts = NULL,
                            bool string_field_defined = true) {

    if (!proxy) {
      proxy = proxy_.get();
    }

    if (num_batches == -1) {
      num_batches = count;
    }

    WriteRequestPB req;
    req.set_tablet_id(tablet_id);

    WriteResponsePB resp;
    rpc::RpcController controller;

    uint64_t inserted_since_last_report = 0;
    for (int i = 0; i < num_batches; ++i) {
      // reset the controller and the request
      controller.Reset();
      controller.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));
      req.clear_ql_write_batch();

      uint64_t first_row_in_batch = first_row + (i * count / num_batches);
      uint64_t last_row_in_batch = first_row_in_batch + count / num_batches;

      for (int j = first_row_in_batch; j < last_row_in_batch; j++) {
        if (!string_field_defined) {
          AddTestRowInsert(j, j, &req);
        } else {
          AddTestRowInsert(j, j, strings::Substitute("original$0", j), &req);
        }
      }
      CHECK_OK(DCHECK_NOTNULL(proxy)->Write(req, &resp, &controller));
      if (write_hybrid_times_collector) {
        write_hybrid_times_collector->push_back(resp.propagated_hybrid_time());
      }

      if (resp.has_error() || resp.per_row_errors_size() > 0) {
        LOG(FATAL) << "Failed to insert batch "
                   << first_row_in_batch << "-" << last_row_in_batch
                   << ": " << resp.DebugString();
      }

      inserted_since_last_report += count / num_batches;
      if ((inserted_since_last_report > 100) && ts) {
        ts->AddValue(static_cast<double>(inserted_since_last_report));
        inserted_since_last_report = 0;
      }
    }

    if (ts) {
      ts->AddValue(static_cast<double>(inserted_since_last_report));
    }
  }

  // Delete specified test row range.
  void DeleteTestRowsRemote(int64_t first_row,
                            uint64_t count,
                            TabletServerServiceProxy* proxy = NULL,
                            string tablet_id = kTabletId) {
    if (!proxy) {
      proxy = proxy_.get();
    }

    WriteRequestPB req;
    WriteResponsePB resp;
    rpc::RpcController controller;

    req.set_tablet_id(tablet_id);

    for (int64_t rowid = first_row; rowid < first_row + count; rowid++) {
      AddTestRowDelete(rowid, &req);
    }

    SCOPED_TRACE(req.DebugString());
    ASSERT_OK(proxy_->Write(req, &resp, &controller));
    SCOPED_TRACE(resp.DebugString());
    ASSERT_FALSE(resp.has_error()) << resp.ShortDebugString();
  }

  void BuildTestRow(int index, QLWriteRequestPB* req) {
    req->add_hashed_column_values()->mutable_value()->set_int32_value(index);
    auto column_value = req->add_column_values();
    column_value->set_column_id(kFirstColumnId + 1);
    column_value->mutable_expr()->mutable_value()->set_int32_value(index * 2);
    column_value = req->add_column_values();
    column_value->set_column_id(kFirstColumnId + 2);
    column_value->mutable_expr()->mutable_value()->set_string_value(
        StringPrintf("hello %d", index));
  }

  void DrainScannerToStrings(const string& scanner_id,
                             const Schema& projection,
                             vector<string>* results,
                             TabletServerServiceProxy* proxy = NULL,
                             uint32_t call_seq_id = 1) {

    if (!proxy) {
      proxy = proxy_.get();
    }

    rpc::RpcController rpc;
    rpc.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));
    ScanRequestPB req;
    ScanResponsePB resp;
    req.set_scanner_id(scanner_id);

    // NOTE: we do not sort the results here, since this function is used
    // by test cases which are verifying the server side's ability to
    // do ordered scans.
    do {
      rpc.Reset();
      req.set_batch_size_bytes(10000);
      req.set_call_seq_id(call_seq_id);
      SCOPED_TRACE(req.DebugString());
      ASSERT_OK(DCHECK_NOTNULL(proxy)->Scan(req, &resp, &rpc));
      SCOPED_TRACE(resp.DebugString());
      ASSERT_FALSE(resp.has_error());

      StringifyRowsFromResponse(projection, rpc, resp, results);
      call_seq_id += 1;
    } while (resp.has_more_results());
  }

  void StringifyRowsFromResponse(const Schema& projection,
                                 const rpc::RpcController& rpc,
                                 const ScanResponsePB& resp,
                                 vector<string>* results) {
    const RowwiseRowBlockPB& rrpb = resp.data();
    Slice direct, indirect; // sidecar data buffers
    ASSERT_OK(rpc.GetSidecar(rrpb.rows_sidecar(), &direct));
    if (rrpb.has_indirect_data_sidecar()) {
      ASSERT_OK(rpc.GetSidecar(rrpb.indirect_data_sidecar(),
                               &indirect));
    }
    vector<const uint8_t*> rows;
    ASSERT_OK(ExtractRowsFromRowBlockPB(projection, rrpb,
                                        indirect, &direct, &rows));
    VLOG(1) << "Round trip got " << rows.size() << " rows";
    for (const uint8_t* row_ptr : rows) {
      ConstContiguousRow row(&projection, row_ptr);
      results->push_back(projection.DebugRow(row));
    }
  }

  void ShutdownTablet() {
    if (mini_server_.get()) {
      // The tablet peer must be destroyed before the TS, otherwise data
      // blocks may be destroyed after their owning block manager.
      tablet_peer_.reset();
      mini_server_->Shutdown();
      mini_server_.reset();
    }
  }

  CHECKED_STATUS ShutdownAndRebuildTablet() {
    ShutdownTablet();

    // Start server.
    mini_server_.reset(new MiniTabletServer(GetTestPath("TabletServerTest-fsroot"), 0));
    auto addr = std::make_shared<vector<HostPort>>();
    addr->push_back(HostPort("255.255.255.255", 1));
    mini_server_->options()->SetMasterAddresses(addr);
    // this should open the tablet created on StartTabletServer()
    RETURN_NOT_OK(mini_server_->Start());
    RETURN_NOT_OK(mini_server_->WaitStarted());

    if (!mini_server_->server()->tablet_manager()->LookupTablet(kTabletId, &tablet_peer_)) {
      return STATUS(NotFound, "Tablet was not found");
    }
    // Connect to it.
    ResetClientProxies();

    // Opening a tablet is async, we wait here instead of having to handle errors later.
    RETURN_NOT_OK(WaitForTabletRunning(kTabletId));
    return Status::OK();

  }

  // Verifies that a set of expected rows (key, value) is present in the tablet.
  void VerifyRows(const Schema& schema, const vector<KeyValue>& expected) {
    gscoped_ptr<RowwiseIterator> iter;
    ASSERT_OK(tablet_peer_->tablet()->NewRowIterator(schema, boost::none, &iter));
    ScanSpec scan_spec;
    ASSERT_OK(iter->Init(&scan_spec));

    int batch_size = std::max(
        (size_t)1, std::min((size_t)(expected.size() / 10),
                            4*1024*1024 / schema.byte_size()));

    Arena arena(32*1024, 256*1024);
    RowBlock block(schema, batch_size, &arena);

    int count = 0;
    while (iter->HasNext()) {
      ASSERT_OK_FAST(iter->NextBlock(&block));
      RowBlockRow rb_row = block.row(0);
      for (int i = 0; i < block.nrows(); i++) {
        if (block.selection_vector()->IsRowSelected(i)) {
          rb_row.Reset(&block, i);
          count++;
        }
      }
    }
    ASSERT_EQ(count, expected.size());
  }

  // Verifies that a simple scan request fails with the specified error code/message.
  void VerifyScanRequestFailure(const Schema& projection,
                                TabletServerErrorPB::Code expected_code,
                                const char *expected_message) {
    ScanRequestPB req;
    ScanResponsePB resp;
    rpc::RpcController rpc;

    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
    req.set_call_seq_id(0);

    // Send the call
    {
      SCOPED_TRACE(req.DebugString());
      ASSERT_OK(proxy_->Scan(req, &resp, &rpc));
      SCOPED_TRACE(resp.DebugString());
      ASSERT_TRUE(resp.has_error());
      ASSERT_EQ(expected_code, resp.error().code());
      ASSERT_STR_CONTAINS(resp.error().status().message(), expected_message);
    }
  }

  // Open a new scanner which scans all of the columns in the table.
  void OpenScannerWithAllColumns(ScanResponsePB* resp) {
    ScanRequestPB req;
    rpc::RpcController rpc;

    // Set up a new request with no predicates, all columns.
    const Schema& projection = schema_;
    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(kTabletId);
    ASSERT_OK(SchemaToColumnPBs(projection, scan->mutable_projected_columns()));
    req.set_call_seq_id(0);
    req.set_batch_size_bytes(0); // so it won't return data right away

    // Send the call
    {
      SCOPED_TRACE(req.DebugString());
      ASSERT_OK(proxy_->Scan(req, resp, &rpc));
      SCOPED_TRACE(resp->DebugString());
      ASSERT_FALSE(resp->has_error());
      ASSERT_TRUE(resp->has_more_results());
    }
  }

 protected:
  static const client::YBTableName kTableName;
  static const char* kTabletId;

  const Schema schema_;
  Schema key_schema_;
  gscoped_ptr<RowBuilder> rb_;
  TableType table_type_;

  std::shared_ptr<rpc::Messenger> client_messenger_;

  gscoped_ptr<MiniTabletServer> mini_server_;
  scoped_refptr<tablet::TabletPeer> tablet_peer_;
  gscoped_ptr<TabletServerServiceProxy> proxy_;
  gscoped_ptr<TabletServerAdminServiceProxy> admin_proxy_;
  gscoped_ptr<consensus::ConsensusServiceProxy> consensus_proxy_;
  gscoped_ptr<server::GenericServiceProxy> generic_proxy_;

  MetricRegistry ts_test_metric_registry_;
  scoped_refptr<MetricEntity> ts_test_metric_entity_;

  void* shared_region_;
};

const client::YBTableName TabletServerTestBase::kTableName("my_keyspace", "test-table");
const char* TabletServerTestBase::kTabletId = "test-tablet";

} // namespace tserver
} // namespace yb

#endif /* YB_TSERVER_TABLET_SERVER_TEST_BASE_H_ */
