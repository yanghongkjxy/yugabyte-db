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

package org.yb.client;

import org.yb.annotations.InterfaceAudience;
import org.yb.master.Master;

@InterfaceAudience.Public
public class GetMasterClusterConfigResponse extends YRpcResponse {
  private Master.SysClusterConfigEntryPB clusterConfig;
  private Master.MasterErrorPB serverError;

  GetMasterClusterConfigResponse(
      long ellapsedMillis, String masterUUID, Master.SysClusterConfigEntryPB config,
      Master.MasterErrorPB error) {
    super(ellapsedMillis, masterUUID);
    serverError = error;
    clusterConfig = config;
  }

  public Master.SysClusterConfigEntryPB getConfig() {
    return clusterConfig;
  }

  public boolean hasError() {
    return serverError != null;
  }

  public String errorMessage() {
    if (serverError == null) {
      return "";
    }

    return serverError.getStatus().getMessage();
  }
}
