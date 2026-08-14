#include "twn_extension.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

enum class TwnColumnKind : uint8_t {
	INT8,
	UINT8,
	INT16,
	UINT16,
	INT32,
	INT64,
	BOOLEAN,
	VARCHAR,
	BLOB,
	INT32_ARRAY,
	FLOAT_LIST
};

struct TwnColumnDefinition {
	TwnColumnDefinition(const char *name_p, LogicalType type_p, TwnColumnKind kind_p, idx_t offset_p,
	                    idx_t length_p = 0, idx_t length_offset_p = 0)
	    : name(name_p), type(std::move(type_p)), kind(kind_p), offset(offset_p), length(length_p),
	      length_offset(length_offset_p) {
	}

	const char *name;
	LogicalType type;
	TwnColumnKind kind;
	idx_t offset;
	idx_t length;
	idx_t length_offset;
};

struct TwnScanBindData : public TableFunctionData {
	TwnScanBindData(string path_p, idx_t record_size_p, idx_t record_count_p, vector<TwnColumnDefinition> columns_p)
	    : path(std::move(path_p)), record_size(record_size_p), record_count(record_count_p),
	      columns(std::move(columns_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<TwnScanBindData>(path, record_size, record_count, columns);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<TwnScanBindData>();
		return path == other.path && record_size == other.record_size && record_count == other.record_count;
	}

	string path;
	idx_t record_size;
	idx_t record_count;
	vector<TwnColumnDefinition> columns;
};

struct TwnScanGlobalState : public GlobalTableFunctionState {
	TwnScanGlobalState(unique_ptr<FileHandle> handle_p, vector<column_t> column_ids_p)
	    : handle(std::move(handle_p)), column_ids(std::move(column_ids_p)), record_offset(0) {
	}

	unique_ptr<FileHandle> handle;
	vector<column_t> column_ids;
	idx_t record_offset;
	vector<data_t> buffer;
};

static vector<TwnColumnDefinition> DecisionColumns() {
	vector<TwnColumnDefinition> columns;
	columns.emplace_back("local_time_ns", LogicalType::BIGINT, TwnColumnKind::INT64, 0);
	columns.emplace_back("strategy_index", LogicalType::INTEGER, TwnColumnKind::INT32, 8);
	columns.emplace_back("symbol_index", LogicalType::INTEGER, TwnColumnKind::INT32, 12);
	columns.emplace_back("key", LogicalType::INTEGER, TwnColumnKind::INT32, 16);
	columns.emplace_back("state_size", LogicalType::INTEGER, TwnColumnKind::INT32, 20);
	columns.emplace_back("hidden_size", LogicalType::INTEGER, TwnColumnKind::INT32, 24);
	columns.emplace_back("message_exchange", LogicalType::UTINYINT, TwnColumnKind::UINT8, 32);
	columns.emplace_back("message_link_number", LogicalType::INTEGER, TwnColumnKind::INT32, 36);
	columns.emplace_back("message_role", LogicalType::UTINYINT, TwnColumnKind::UINT8, 40);
	columns.emplace_back("message_is_decision_enabled", LogicalType::BOOLEAN, TwnColumnKind::BOOLEAN, 41);
	columns.emplace_back("message_symbol_index", LogicalType::INTEGER, TwnColumnKind::INT32, 44);
	columns.emplace_back("message_seq", LogicalType::INTEGER, TwnColumnKind::INT32, 48);
	columns.emplace_back("message_local_time_ns", LogicalType::BIGINT, TwnColumnKind::INT64, 56);
	columns.emplace_back("message_body_length", LogicalType::USMALLINT, TwnColumnKind::UINT16, 64);
	columns.emplace_back("message_body", LogicalType::BLOB, TwnColumnKind::BLOB, 66, 118, 64);
	columns.emplace_back("action_status", LogicalType::TINYINT, TwnColumnKind::INT8, 184);
	columns.emplace_back("action_side", LogicalType::TINYINT, TwnColumnKind::INT8, 185);
	columns.emplace_back("action_int16", LogicalType::SMALLINT, TwnColumnKind::INT16, 186);
	columns.emplace_back("action_int32", LogicalType::INTEGER, TwnColumnKind::INT32, 188);
	columns.emplace_back("action_time", LogicalType::BIGINT, TwnColumnKind::INT64, 192);
	columns.emplace_back("action_price", LogicalType::INTEGER, TwnColumnKind::INT32, 200);
	columns.emplace_back("action_volume", LogicalType::INTEGER, TwnColumnKind::INT32, 204);
	columns.emplace_back("account_index", LogicalType::INTEGER, TwnColumnKind::INT32, 208);
	columns.emplace_back("account_identifier", LogicalType::INTEGER, TwnColumnKind::INT32, 212);
	columns.emplace_back("account_position", LogicalType::INTEGER, TwnColumnKind::INT32, 216);
	columns.emplace_back("account_quota", LogicalType::INTEGER, TwnColumnKind::INT32, 220);
	columns.emplace_back("account_insert", LogicalType::INTEGER, TwnColumnKind::INT32, 224);
	columns.emplace_back("account_cancel", LogicalType::INTEGER, TwnColumnKind::INT32, 228);
	columns.emplace_back("pending_status", LogicalType::TINYINT, TwnColumnKind::INT8, 232);
	columns.emplace_back("pending_side", LogicalType::TINYINT, TwnColumnKind::INT8, 233);
	columns.emplace_back("pending_int16", LogicalType::SMALLINT, TwnColumnKind::INT16, 234);
	columns.emplace_back("pending_int32", LogicalType::INTEGER, TwnColumnKind::INT32, 236);
	columns.emplace_back("pending_time", LogicalType::BIGINT, TwnColumnKind::INT64, 240);
	columns.emplace_back("pending_price", LogicalType::INTEGER, TwnColumnKind::INT32, 248);
	columns.emplace_back("pending_volume", LogicalType::INTEGER, TwnColumnKind::INT32, 252);
	columns.emplace_back("decision_status", LogicalType::TINYINT, TwnColumnKind::INT8, 256);
	columns.emplace_back("decision_side", LogicalType::TINYINT, TwnColumnKind::INT8, 257);
	columns.emplace_back("decision_int16", LogicalType::SMALLINT, TwnColumnKind::INT16, 258);
	columns.emplace_back("decision_int32", LogicalType::INTEGER, TwnColumnKind::INT32, 260);
	columns.emplace_back("decision_time", LogicalType::BIGINT, TwnColumnKind::INT64, 264);
	columns.emplace_back("decision_price", LogicalType::INTEGER, TwnColumnKind::INT32, 272);
	columns.emplace_back("decision_volume", LogicalType::INTEGER, TwnColumnKind::INT32, 276);
	columns.emplace_back("orderbook_timestamp", LogicalType::BIGINT, TwnColumnKind::INT64, 280);
	columns.emplace_back("orderbook_action_num", LogicalType::INTEGER, TwnColumnKind::INT32, 288);
	columns.emplace_back("orderbook_bid_levels", LogicalType::INTEGER, TwnColumnKind::INT32, 292);
	columns.emplace_back("orderbook_ask_levels", LogicalType::INTEGER, TwnColumnKind::INT32, 296);
	columns.emplace_back("orderbook_bid_prices", LogicalType::ARRAY(LogicalType::INTEGER, 10),
	                     TwnColumnKind::INT32_ARRAY, 300, 10);
	columns.emplace_back("orderbook_bid_volumes", LogicalType::ARRAY(LogicalType::INTEGER, 10),
	                     TwnColumnKind::INT32_ARRAY, 340, 10);
	columns.emplace_back("orderbook_ask_prices", LogicalType::ARRAY(LogicalType::INTEGER, 10),
	                     TwnColumnKind::INT32_ARRAY, 380, 10);
	columns.emplace_back("orderbook_ask_volumes", LogicalType::ARRAY(LogicalType::INTEGER, 10),
	                     TwnColumnKind::INT32_ARRAY, 420, 10);
	columns.emplace_back("order_error_code", LogicalType::INTEGER, TwnColumnKind::INT32, 460);
	columns.emplace_back("order_key", LogicalType::INTEGER, TwnColumnKind::INT32, 464);
	columns.emplace_back("order_side", LogicalType::INTEGER, TwnColumnKind::INT32, 468);
	columns.emplace_back("order_qapi_side", LogicalType::INTEGER, TwnColumnKind::INT32, 472);
	columns.emplace_back("order_price", LogicalType::INTEGER, TwnColumnKind::INT32, 476);
	columns.emplace_back("order_volume", LogicalType::INTEGER, TwnColumnKind::INT32, 480);
	columns.emplace_back("order_qty", LogicalType::INTEGER, TwnColumnKind::INT32, 484);
	columns.emplace_back("order_reserved", LogicalType::INTEGER, TwnColumnKind::INT32, 488);
	columns.emplace_back("order_clord_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 492, 8);
	columns.emplace_back("order_orig_clord_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 500, 8);
	columns.emplace_back("order_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 508, 8);
	columns.emplace_back("state", LogicalType::LIST(LogicalType::FLOAT), TwnColumnKind::FLOAT_LIST, 516, 1536, 20);
	columns.emplace_back("hidden", LogicalType::LIST(LogicalType::FLOAT), TwnColumnKind::FLOAT_LIST, 6660, 512, 24);
	return columns;
}

static vector<TwnColumnDefinition> ExecutionColumns() {
	vector<TwnColumnDefinition> columns;
	columns.emplace_back("local_time_ns", LogicalType::BIGINT, TwnColumnKind::INT64, 0);
	columns.emplace_back("symbol_index", LogicalType::INTEGER, TwnColumnKind::INT32, 8);
	columns.emplace_back("key", LogicalType::INTEGER, TwnColumnKind::INT32, 12);
	columns.emplace_back("is_recover", LogicalType::INTEGER, TwnColumnKind::INT32, 16);
	columns.emplace_back("is_cancel_reject", LogicalType::INTEGER, TwnColumnKind::INT32, 20);
	columns.emplace_back("exec_sequence", LogicalType::INTEGER, TwnColumnKind::INT32, 24);
	columns.emplace_back("side", LogicalType::INTEGER, TwnColumnKind::INT32, 28);
	columns.emplace_back("order_qty", LogicalType::INTEGER, TwnColumnKind::INT32, 32);
	columns.emplace_back("price", LogicalType::INTEGER, TwnColumnKind::INT32, 36);
	columns.emplace_back("last_qty", LogicalType::INTEGER, TwnColumnKind::INT32, 40);
	columns.emplace_back("last_price", LogicalType::INTEGER, TwnColumnKind::INT32, 44);
	columns.emplace_back("leaves_qty", LogicalType::INTEGER, TwnColumnKind::INT32, 48);
	columns.emplace_back("cum_qty", LogicalType::INTEGER, TwnColumnKind::INT32, 52);
	columns.emplace_back("avg_price", LogicalType::INTEGER, TwnColumnKind::INT32, 56);
	columns.emplace_back("cxl_rej_reason", LogicalType::INTEGER, TwnColumnKind::INT32, 60);
	columns.emplace_back("cxl_rej_response_to", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 64, 1);
	columns.emplace_back("msg_type", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 65, 1);
	columns.emplace_back("exec_type", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 66, 1);
	columns.emplace_back("ord_status", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 67, 1);
	columns.emplace_back("target_sub_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 68, 1);
	columns.emplace_back("clord_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 69, 16);
	columns.emplace_back("orig_clord_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 85, 16);
	columns.emplace_back("order_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 101, 8);
	columns.emplace_back("exec_id", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 109, 16);
	columns.emplace_back("account", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 125, 8);
	columns.emplace_back("symbol", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 133, 8);
	columns.emplace_back("sub_account", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 141, 8);
	columns.emplace_back("transact_time", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 149, 24);
	columns.emplace_back("text", LogicalType::VARCHAR, TwnColumnKind::VARCHAR, 173, 128);
	return columns;
}

static unique_ptr<FunctionData> TwnScanBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names, idx_t record_size,
                                            vector<TwnColumnDefinition> columns) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("TWN persist filename cannot be NULL");
	}
	auto path = StringValue::Get(input.inputs[0]);
	auto &file_system = FileSystem::GetFileSystem(context);
	auto handle = file_system.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = handle->GetFileSize();
	if (file_size % record_size != 0) {
		throw InvalidInputException("TWN persist file '%s' has size %llu, which is not aligned to %llu-byte records",
		                            path, file_size, record_size);
	}
	for (auto &column : columns) {
		names.emplace_back(column.name);
		return_types.emplace_back(column.type);
	}
	return make_uniq<TwnScanBindData>(std::move(path), record_size, file_size / record_size, std::move(columns));
}

static unique_ptr<FunctionData> TwnDecisionBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	return TwnScanBind(context, input, return_types, names, 8712, DecisionColumns());
}

static unique_ptr<FunctionData> TwnExecutionBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return TwnScanBind(context, input, return_types, names, 304, ExecutionColumns());
}

static unique_ptr<GlobalTableFunctionState> TwnScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TwnScanBindData>();
	auto &file_system = FileSystem::GetFileSystem(context);
	auto handle = file_system.OpenFile(bind_data.path, FileFlags::FILE_FLAGS_READ);
	return make_uniq<TwnScanGlobalState>(std::move(handle), input.column_ids);
}

template <class T>
static void SetPrimitive(Vector &output, idx_t row, const_data_ptr_t record, idx_t offset) {
	FlatVector::GetData<T>(output)[row] = Load<T>(record + offset);
}

static idx_t BoundedLength(const TwnColumnDefinition &column, const_data_ptr_t record) {
	if (!column.length_offset) {
		return column.length;
	}
	idx_t length;
	if (column.kind == TwnColumnKind::BLOB) {
		length = Load<uint16_t>(record + column.length_offset);
	} else {
		auto signed_length = Load<int32_t>(record + column.length_offset);
		if (signed_length < 0) {
			throw InvalidInputException("TWN persist record has negative list length %d", signed_length);
		}
		length = NumericCast<idx_t>(signed_length);
	}
	if (length > column.length) {
		throw InvalidInputException("TWN persist record length %llu exceeds capacity %llu", length, column.length);
	}
	return length;
}

static void SetColumn(Vector &output, idx_t row, const TwnColumnDefinition &column, const_data_ptr_t record) {
	switch (column.kind) {
	case TwnColumnKind::INT8:
		SetPrimitive<int8_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::UINT8:
		SetPrimitive<uint8_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::INT16:
		SetPrimitive<int16_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::UINT16:
		SetPrimitive<uint16_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::INT32:
		SetPrimitive<int32_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::INT64:
		SetPrimitive<int64_t>(output, row, record, column.offset);
		break;
	case TwnColumnKind::BOOLEAN:
		FlatVector::GetData<bool>(output)[row] = Load<uint8_t>(record + column.offset) != 0;
		break;
	case TwnColumnKind::VARCHAR: {
		auto length = column.length;
		auto begin = reinterpret_cast<const char *>(record + column.offset);
		auto end = static_cast<const char *>(std::memchr(begin, '\0', length));
		if (end) {
			length = NumericCast<idx_t>(end - begin);
		}
		FlatVector::GetData<string_t>(output)[row] = StringVector::AddString(output, begin, length);
		break;
	}
	case TwnColumnKind::BLOB: {
		auto length = BoundedLength(column, record);
		output.SetValue(row, Value::BLOB(record + column.offset, length));
		break;
	}
	case TwnColumnKind::INT32_ARRAY: {
		vector<Value> values;
		values.reserve(column.length);
		for (idx_t index = 0; index < column.length; index++) {
			values.emplace_back(Value::INTEGER(Load<int32_t>(record + column.offset + index * sizeof(int32_t))));
		}
		output.SetValue(row, Value::ARRAY(LogicalType::INTEGER, std::move(values)));
		break;
	}
	case TwnColumnKind::FLOAT_LIST: {
		auto length = BoundedLength(column, record);
		vector<Value> values;
		values.reserve(length);
		for (idx_t index = 0; index < length; index++) {
			values.emplace_back(Value::FLOAT(Load<float>(record + column.offset + index * sizeof(float))));
		}
		output.SetValue(row, Value::LIST(LogicalType::FLOAT, std::move(values)));
		break;
	}
	}
}

static void TwnScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<TwnScanBindData>();
	auto &state = input.global_state->Cast<TwnScanGlobalState>();
	if (state.record_offset >= bind_data.record_count) {
		return;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.record_count - state.record_offset);
	if (!state.column_ids.empty()) {
		state.buffer.resize(count * bind_data.record_size);
		state.handle->Read(state.buffer.data(), state.buffer.size(), state.record_offset * bind_data.record_size);
		for (idx_t output_column = 0; output_column < state.column_ids.size(); output_column++) {
			auto column_id = state.column_ids[output_column];
			if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
				continue;
			}
			auto &column = bind_data.columns[NumericCast<idx_t>(column_id)];
			for (idx_t row = 0; row < count; row++) {
				auto record = state.buffer.data() + row * bind_data.record_size;
				SetColumn(output.data[output_column], row, column, record);
			}
		}
	}
	state.record_offset += count;
	output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_decision("read_twn_decision", {LogicalType::VARCHAR}, TwnScan, TwnDecisionBind, TwnScanInit);
	read_decision.projection_pushdown = true;
	loader.RegisterFunction(read_decision);

	TableFunction read_execution("read_twn_execution", {LogicalType::VARCHAR}, TwnScan, TwnExecutionBind, TwnScanInit);
	read_execution.projection_pushdown = true;
	loader.RegisterFunction(read_execution);
}

void TwnExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string TwnExtension::Name() {
	return "twn";
}

std::string TwnExtension::Version() const {
#ifdef EXT_VERSION_TWN
	return EXT_VERSION_TWN;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(twn, loader) {
	duckdb::LoadInternal(loader);
}
}
