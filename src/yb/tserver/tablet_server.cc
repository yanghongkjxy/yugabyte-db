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

#include "yb/tserver/tablet_server.h"

#include <algorithm>
#include <list>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "yb/fs/fs_manager.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rpc/service_if.h"
#include "yb/server/rpc_server.h"
#include "yb/server/webserver.h"
#include "yb/tablet/maintenance_manager.h"
#include "yb/tserver/heartbeater.h"
#include "yb/tserver/scanners.h"
#include "yb/tserver/tablet_service.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver-path-handlers.h"
#include "yb/tserver/remote_bootstrap_service.h"
#include "yb/util/flag_tags.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/status.h"

using std::make_shared;
using std::shared_ptr;
using std::vector;
using yb::rpc::ServiceIf;
using yb::tablet::TabletPeer;

DEFINE_int32(tablet_server_svc_num_threads, -1,
             "Number of RPC worker threads for the TS service. If -1, it is auto configured.");
TAG_FLAG(tablet_server_svc_num_threads, advanced);

DEFINE_int32(ts_admin_svc_num_threads, 10,
             "Number of RPC worker threads for the TS admin service");
TAG_FLAG(ts_admin_svc_num_threads, advanced);

DEFINE_int32(ts_consensus_svc_num_threads, -1,
             "Number of RPC worker threads for the TS consensus service. If -1, it is auto "
             "configured.");
TAG_FLAG(ts_consensus_svc_num_threads, advanced);

DEFINE_int32(ts_remote_bootstrap_svc_num_threads, 10,
             "Number of RPC worker threads for the TS remote bootstrap service");
TAG_FLAG(ts_remote_bootstrap_svc_num_threads, advanced);

DEFINE_int32(tablet_server_svc_queue_length, -1,
             "RPC queue length for the TS service. If -1, it is auto configured.");
TAG_FLAG(tablet_server_svc_queue_length, advanced);

DEFINE_int32(ts_admin_svc_queue_length, 50,
             "RPC queue length for the TS admin service");
TAG_FLAG(ts_admin_svc_queue_length, advanced);

DEFINE_int32(ts_consensus_svc_queue_length, -1,
             "RPC queue length for the TS consensus service. If -1, it is auto configured.");
TAG_FLAG(ts_consensus_svc_queue_length, advanced);

DEFINE_int32(ts_remote_bootstrap_svc_queue_length, 50,
             "RPC queue length for the TS remote bootstrap service");
TAG_FLAG(ts_remote_bootstrap_svc_queue_length, advanced);

DEFINE_bool(enable_direct_local_tablet_server_call,
            true,
            "Enable direct call to local tablet server");
TAG_FLAG(enable_direct_local_tablet_server_call, advanced);

namespace yb {
namespace tserver {

TabletServer::TabletServer(const TabletServerOptions& opts)
    : RpcAndWebServerBase("TabletServer", opts, "yb.tabletserver"),
      initted_(false),
      fail_heartbeats_for_tests_(false),
      opts_(opts),
      tablet_manager_(new TSTabletManager(fs_manager_.get(), this, metric_registry())),
      scanner_manager_(new ScannerManager(metric_entity())),
      path_handlers_(new TabletServerPathHandlers(this)),
      maintenance_manager_(new MaintenanceManager(MaintenanceManager::DEFAULT_OPTIONS)),
      master_config_index_(0),
      tablet_server_service_(nullptr) {
}

TabletServer::~TabletServer() {
  Shutdown();
}

std::string TabletServer::ToString() const {
  return strings::Substitute("TabletServer : rpc=$0, uuid=$1",
                             yb::ToString(first_rpc_address()),
                             fs_manager_->uuid());
}

Status TabletServer::ValidateMasterAddressResolution() const {
  for (const HostPort& master_addr : *opts_.GetMasterAddresses().get()) {
    RETURN_NOT_OK_PREPEND(master_addr.ResolveAddresses(NULL),
                          strings::Substitute(
                              "Couldn't resolve master service address '$0'",
                              master_addr.ToString()));
  }
  return Status::OK();
}

Status TabletServer::UpdateMasterAddresses(const consensus::RaftConfigPB& new_config) {
  shared_ptr<vector<HostPort>> new_master_addresses = make_shared<vector<HostPort>>();

  SetCurrentMasterIndex(new_config.opid_index());

  for (const auto& peer : new_config.peers()) {
    HostPort hp;
    RETURN_NOT_OK(HostPortFromPB(peer.last_known_addr(), &hp));
    new_master_addresses->push_back(std::move(hp));
  }
  opts_.SetMasterAddresses(new_master_addresses);

  LOG(INFO) << "Got new list of " << new_config.peers_size() << " masters at index "
            << new_config.opid_index() << " new masters="
            << HostPort::ToCommaSeparatedString(*new_master_addresses.get());

  heartbeater_->set_master_addresses(new_master_addresses);

  return Status::OK();
}

Status TabletServer::Init() {
  CHECK(!initted_);

  // Validate that the passed master address actually resolves.
  // We don't validate that we can connect at this point -- it should
  // be allowed to start the TS and the master in whichever order --
  // our heartbeat thread will loop until successfully connecting.
  RETURN_NOT_OK(ValidateMasterAddressResolution());

  RETURN_NOT_OK(RpcAndWebServerBase::Init());
  RETURN_NOT_OK(path_handlers_->Register(web_server_.get()));

  heartbeater_.reset(new Heartbeater(opts_, this));

  RETURN_NOT_OK_PREPEND(tablet_manager_->Init(),
                        "Could not init Tablet Manager");

  RETURN_NOT_OK_PREPEND(scanner_manager_->StartRemovalThread(),
                        "Could not start expired Scanner removal thread");

  initted_ = true;
  return Status::OK();
}

Status TabletServer::WaitInited() {
  return tablet_manager_->WaitForAllBootstrapsToFinish();
}

void TabletServer::AutoInitServiceFlags() {
  const int32 num_cores = std::thread::hardware_concurrency();

  if (FLAGS_tablet_server_svc_num_threads == -1) {
    // Auto select number of threads for the TS service based on number of cores.
    // But bound it between 64 & 512.
    const int32 num_threads = std::min(512, num_cores * 32);
    FLAGS_tablet_server_svc_num_threads = std::max(64, num_threads);
    LOG(INFO) << "Auto setting FLAGS_tablet_server_svc_num_threads to "
              << FLAGS_tablet_server_svc_num_threads;
  }

  if (FLAGS_ts_consensus_svc_num_threads == -1) {
    // Auto select number of threads for the TS service based on number of cores.
    // But bound it between 64 & 512.
    const int32 num_threads = std::min(512, num_cores * 32);
    FLAGS_ts_consensus_svc_num_threads = std::max(64, num_threads);
    LOG(INFO) << "Auto setting FLAGS_ts_consensus_svc_num_threads to "
              << FLAGS_ts_consensus_svc_num_threads;
  }

  if (FLAGS_tablet_server_svc_queue_length == -1) {
    if (num_cores <= 4) {
      FLAGS_tablet_server_svc_queue_length = kLighterSvcQueueLength;
    } else {
      FLAGS_tablet_server_svc_queue_length = kDefaultSvcQueueLength;
    }
    LOG(INFO) << "Auto setting FLAGS_tablet_server_svc_queue_length to "
              << FLAGS_tablet_server_svc_queue_length;
  }

  if (FLAGS_ts_consensus_svc_queue_length == -1) {
    if (num_cores <= 4) {
      FLAGS_ts_consensus_svc_queue_length = kLighterSvcQueueLength;
    } else {
      FLAGS_ts_consensus_svc_queue_length = kDefaultSvcQueueLength;
    }
    LOG(INFO) << "Auto setting FLAGS_ts_consensus_svc_queue_length to "
              << FLAGS_ts_consensus_svc_queue_length;
  }
}

Status TabletServer::RegisterServices() {
  tablet_server_service_ = new TabletServiceImpl(this);
  std::unique_ptr<ServiceIf> ts_service(tablet_server_service_);
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_tablet_server_svc_queue_length,
                                                     std::move(ts_service)));

  std::unique_ptr<ServiceIf> admin_service(new TabletServiceAdminImpl(this));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_admin_svc_queue_length,
                                                     std::move(admin_service)));

  std::unique_ptr<ServiceIf> consensus_service(new ConsensusServiceImpl(metric_entity(),
                                                                        tablet_manager_.get()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_consensus_svc_queue_length,
                                                     std::move(consensus_service)));

  std::unique_ptr<ServiceIf> remote_bootstrap_service(
      new RemoteBootstrapServiceImpl(fs_manager_.get(), tablet_manager_.get(), metric_entity()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_ts_remote_bootstrap_svc_queue_length,
                                                     std::move(remote_bootstrap_service)));
  return Status::OK();
}

Status TabletServer::Start() {
  CHECK(initted_);

  AutoInitServiceFlags();
  RETURN_NOT_OK(RegisterServices());
  RETURN_NOT_OK(RpcAndWebServerBase::Start());

  // If enabled, creates a proxy to call this tablet server locally.
  if (FLAGS_enable_direct_local_tablet_server_call) {
    proxy_.reset(new TabletServerServiceProxy(messenger_, Endpoint()));
  }

  RETURN_NOT_OK(heartbeater_->Start());
  RETURN_NOT_OK(maintenance_manager_->Init());

  google::FlushLogFiles(google::INFO); // Flush the startup messages.

  return Status::OK();
}

void TabletServer::Shutdown() {
  LOG(INFO) << "TabletServer shutting down...";

  if (initted_) {
    maintenance_manager_->Shutdown();
    WARN_NOT_OK(heartbeater_->Stop(), "Failed to stop TS Heartbeat thread");
    {
      std::lock_guard<simple_spinlock> l(lock_);
      tablet_server_service_ = nullptr;
    }
    RpcAndWebServerBase::Shutdown();
    scanner_manager_.reset();
    tablet_manager_->Shutdown();
  }

  LOG(INFO) << "TabletServer shut down complete. Bye!";
}

Status TabletServer::PopulateLiveTServers(const master::TSHeartbeatResponsePB& heartbeat_resp) {
  std::lock_guard<simple_spinlock> l(lock_);
  // We reset the list each time, since we want to keep the tservers that are live from the
  // master's perspective.
  // TODO: In the future, we should enhance the logic here to keep track information retrieved
  // from the master and compare it with information stored here. Based on this information, we
  // can only send diff updates CQL clients about whether a node came up or went down.
  live_tservers_.assign(heartbeat_resp.tservers().begin(), heartbeat_resp.tservers().end());
  return Status::OK();
}

void TabletServer::set_cluster_uuid(const std::string& cluster_uuid) {
  std::lock_guard<simple_spinlock> l(lock_);
  cluster_uuid_ = cluster_uuid;
}

std::string TabletServer::cluster_uuid() const {
  std::lock_guard<simple_spinlock> l(lock_);
  return cluster_uuid_;
}

TabletServiceImpl* TabletServer::tablet_server_service() {
  std::lock_guard<simple_spinlock> l(lock_);
  return tablet_server_service_;
}

}  // namespace tserver
}  // namespace yb
