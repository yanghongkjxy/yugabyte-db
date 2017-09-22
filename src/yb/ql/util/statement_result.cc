//--------------------------------------------------------------------------------------------------
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
//
// Different results of processing a statement.
//--------------------------------------------------------------------------------------------------

#include "yb/ql/util/statement_result.h"

#include "yb/client/client.h"
#include "yb/client/schema-internal.h"
#include "yb/common/wire_protocol.h"
#include "yb/util/pb_util.h"
#include "yb/ql/ptree/pt_select.h"

namespace yb {
namespace ql {

using std::string;
using std::vector;
using std::unique_ptr;
using strings::Substitute;

using client::YBOperation;
using client::YBqlOp;
using client::YBqlReadOp;
using client::YBqlWriteOp;

//------------------------------------------------------------------------------------------------
namespace {

// Get bind column schemas for DML.
vector<ColumnSchema> GetBindVariableSchemasFromDmlStmt(const PTDmlStmt& stmt) {
  vector<ColumnSchema> bind_variable_schemas;
  bind_variable_schemas.reserve(stmt.bind_variables().size());
  for (const PTBindVar *var : stmt.bind_variables()) {
    bind_variable_schemas.emplace_back(string(var->name()->c_str()), var->ql_type());
  }
  return bind_variable_schemas;
}

// Get column schemas from different statements / QL ops.
vector<ColumnSchema> GetColumnSchemasFromSelectStmt(const PTSelectStmt& stmt) {
  vector<ColumnSchema> column_schemas;
  column_schemas.reserve(stmt.selected_columns().size());
  const auto& schema = stmt.table()->schema();
  for (const ColumnDesc *col_desc : stmt.selected_columns()) {
    const auto column = schema.ColumnById(col_desc->id());
    column_schemas.emplace_back(column.name(), column.type());
  }
  return column_schemas;
}

vector<ColumnSchema> GetColumnSchemasFromOp(const YBqlOp& op) {
  vector<ColumnSchema> column_schemas;
  switch (op.type()) {
    case YBOperation::Type::QL_READ: {
      const auto& read_op = static_cast<const YBqlReadOp&>(op);
      column_schemas.reserve(read_op.request().column_ids_size());
      const auto& schema = read_op.table()->schema();
      for (const auto column_id : read_op.request().column_ids()) {
        const auto column = schema.ColumnById(column_id);
        column_schemas.emplace_back(column.name(), column.type());
      }
      return column_schemas;
    }
    case YBOperation::Type::QL_WRITE: {
      const auto& write_op = static_cast<const YBqlWriteOp&>(op);
      column_schemas.reserve(write_op.response().column_schemas_size());
      for (const auto column_schema : write_op.response().column_schemas()) {
        column_schemas.emplace_back(ColumnSchemaFromPB(column_schema));
      }
      return column_schemas;
    }
    case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
    case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_WRITE:
      break;
    // default: fallthrough
  }
  LOG(FATAL) << "Internal error: invalid or unknown QL operation: " << op.type();
}

QLClient GetClientFromOp(const YBqlOp& op) {
  switch (op.type()) {
    case YBOperation::Type::QL_READ:
      return static_cast<const YBqlReadOp&>(op).request().client();
    case YBOperation::Type::QL_WRITE:
      return static_cast<const YBqlWriteOp&>(op).request().client();
    case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
    case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
    case YBOperation::Type::REDIS_WRITE:
      break;
    // default: fallthrough
  }
  LOG(FATAL) << "Internal error: invalid or unknown QL operation: " << op.type();

  // Inactive code: It's only meant to avoid compilation warning.
  return QLClient();
}

} // namespace

//------------------------------------------------------------------------------------------------
PreparedResult::PreparedResult(const PTDmlStmt& stmt)
    : table_name_(stmt.table()->name()),
      hash_col_indices_(stmt.hash_col_indices()),
      bind_variable_schemas_(GetBindVariableSchemasFromDmlStmt(stmt)),
      column_schemas_(stmt.opcode() == TreeNodeOpcode::kPTSelectStmt ?
                      GetColumnSchemasFromSelectStmt(static_cast<const PTSelectStmt&>(stmt)) :
                      vector<ColumnSchema>()) {
}

PreparedResult::~PreparedResult() {
}

//------------------------------------------------------------------------------------------------
RowsResult::RowsResult(YBqlOp *op)
    : table_name_(op->table()->name()),
      column_schemas_(GetColumnSchemasFromOp(*op)),
      client_(GetClientFromOp(*op)),
      rows_data_(op->rows_data()) {
  // If there is a paging state in the response, fill in the table ID also and serialize the
  // paging state as bytes.
  if (op->response().has_paging_state()) {
    QLPagingStatePB *paging_state = op->mutable_response()->mutable_paging_state();
    paging_state->set_table_id(op->table()->id());
    faststring serialized_paging_state;
    CHECK(pb_util::SerializeToString(*paging_state, &serialized_paging_state));
    paging_state_ = serialized_paging_state.ToString();
  }
}

RowsResult::RowsResult(const client::YBTableName& table_name,
                       const std::vector<ColumnSchema>& column_schemas,
                       const std::string& rows_data)
    : table_name_(table_name),
      column_schemas_(column_schemas),
      client_(QLClient::YQL_CLIENT_CQL),
      rows_data_(rows_data) {
}

RowsResult::~RowsResult() {
}

Status RowsResult::Append(const RowsResult& other) {
  if (rows_data_.empty()) {
    rows_data_ = other.rows_data_;
  } else {
    RETURN_NOT_OK(QLRowBlock::AppendRowsData(other.client_, other.rows_data_, &rows_data_));
  }
  paging_state_ = other.paging_state_;
  return Status::OK();
}

std::unique_ptr<QLRowBlock> RowsResult::GetRowBlock() const {
  Schema schema(column_schemas_, 0);
  unique_ptr<QLRowBlock> rowblock(new QLRowBlock(schema));
  Slice data(rows_data_);
  if (!data.empty()) {
    // TODO: a better way to handle errors here?
    CHECK_OK(rowblock->Deserialize(client_, &data));
  }
  return rowblock;
}

//------------------------------------------------------------------------------------------------
SchemaChangeResult::SchemaChangeResult(
    const string& change_type, const string& object_type,
    const string& keyspace_name, const string& object_name)
    : change_type_(change_type), object_type_(object_type),
      keyspace_name_(keyspace_name), object_name_(object_name) {
}

SchemaChangeResult::~SchemaChangeResult() {
}


} // namespace ql
} // namespace yb