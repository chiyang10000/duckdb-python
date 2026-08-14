#include "twn_extension.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

#include "lmdb.h"

#include <algorithm>
#include <cstring>

extern "C" {
int c_database_n(size_t value_size);
void c_orderbook_delete(void *orderbook);
void c_orderbook_extract(void *orderbook, const void *actions, int count, float *output);
const char *c_sources();
}

namespace duckdb {

// GPT-5: 大小来自当前 libhft C ABI，并由完整矩阵对拍防止静默漂移。
constexpr idx_t HFT_ACTION_SIZE = 24;
constexpr idx_t HFT_ORDERBOOK_SIZE = 184;
static_assert(sizeof(void *) == 8, "hft_ob requires a 64-bit libhft ABI");

static vector<string> HftSources() {
	auto source_csv = string(c_sources());
	if (source_csv.empty() || source_csv.back() != ',') {
		throw InvalidInputException("Invalid source list returned by libhft");
	}
	vector<string> sources;
	idx_t begin = 0;
	for (idx_t index = 0; index < source_csv.size(); index++) {
		if (source_csv[index] != ',') {
			continue;
		}
		sources.emplace_back(source_csv.substr(begin, index - begin));
		begin = index + 1;
	}
	return sources;
}

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

class HftLmdbValue {
public:
	HftLmdbValue(const string &path_p, int32_t key_p)
	    : path(path_p), key(key_p), env(nullptr), txn(nullptr), action_count(0), actions(nullptr) {
		Check("create LMDB environment", mdb_env_create(&env));
		Check("open LMDB environment",
		      mdb_env_open(env, path.c_str(), MDB_RDONLY | MDB_NOSUBDIR | MDB_NOLOCK | MDB_NORDAHEAD, 0444));
		Check("begin LMDB transaction", mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn));

		MDB_dbi dbi;
		Check("open LMDB database", mdb_dbi_open(txn, nullptr, 0, &dbi));
		MDB_val key_value {sizeof(key), &key};
		MDB_val value;
		auto rc = mdb_get(txn, dbi, &key_value, &value);
		if (rc == MDB_NOTFOUND) {
			Close();
			throw IOException("HFT action key %d not found in '%s'", key, path);
		}
		Check("read LMDB value", rc);

		int count;
		try {
			count = c_database_n(value.mv_size);
		} catch (int) {
			Close();
			throw InvalidInputException("Invalid HFT action value for key %d in '%s'", key, path);
		}
		if (count < 0 || NumericCast<idx_t>(count) > value.mv_size / HFT_ACTION_SIZE) {
			Close();
			throw InvalidInputException("Invalid HFT action value for key %d in '%s'", key, path);
		}
		action_count = NumericCast<idx_t>(count);
		actions = static_cast<const_data_ptr_t>(value.mv_data) + value.mv_size - action_count * HFT_ACTION_SIZE;
	}

	~HftLmdbValue() {
		Close();
	}

	idx_t Count() const {
		return action_count;
	}

	const_data_ptr_t Action(idx_t index) const {
		return actions + index * HFT_ACTION_SIZE;
	}

private:
	void Check(const char *operation, int rc) {
		if (!rc) {
			return;
		}
		auto message = string(mdb_strerror(rc));
		Close();
		throw IOException("Failed to %s for HFT ACTION-bin '%s': %s", operation, path, message);
	}

	void Close() {
		if (txn) {
			mdb_txn_abort(txn);
			txn = nullptr;
		}
		if (env) {
			mdb_env_close(env);
			env = nullptr;
		}
	}

	string path;
	int32_t key;
	MDB_env *env;
	MDB_txn *txn;
	idx_t action_count;
	const_data_ptr_t actions;
};

struct HftOrderbookState {
	HftOrderbookState() : bytes {} {
	}

	~HftOrderbookState() {
		c_orderbook_delete(bytes);
	}

	void Reset() {
		c_orderbook_delete(bytes);
		std::memset(bytes, 0, sizeof(bytes));
	}

	alignas(8) data_t bytes[HFT_ORDERBOOK_SIZE];
};

struct HftObBindData : public TableFunctionData {
	HftObBindData(string path_p, int32_t key_p, idx_t action_count_p, idx_t source_count_p)
	    : path(std::move(path_p)), key(key_p), action_count(action_count_p), source_count(source_count_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<HftObBindData>(path, key, action_count, source_count);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<HftObBindData>();
		return path == other.path && key == other.key && action_count == other.action_count &&
		       source_count == other.source_count;
	}

	string path;
	int32_t key;
	idx_t action_count;
	idx_t source_count;
};

struct HftObGlobalState : public GlobalTableFunctionState {
	HftObGlobalState(const string &path, int32_t key, vector<column_t> column_ids_p)
	    : value(path, key), column_ids(std::move(column_ids_p)), action_offset(0) {
	}

	HftLmdbValue value;
	HftOrderbookState orderbook;
	vector<column_t> column_ids;
	idx_t action_offset;
};

static bool HftKeyMatchesFilter(int32_t key, const TableFilter &filter) {
	auto key_value = Value::INTEGER(key);
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON:
		return filter.Cast<ConstantFilter>().Compare(key_value);
	case TableFilterType::IS_NULL:
		return false;
	case TableFilterType::IS_NOT_NULL:
		return true;
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		for (auto &child : conjunction.child_filters) {
			if (!HftKeyMatchesFilter(key, *child)) {
				return false;
			}
		}
		return true;
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		for (auto &child : conjunction.child_filters) {
			if (HftKeyMatchesFilter(key, *child)) {
				return true;
			}
		}
		return false;
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		for (auto &value : in_filter.values) {
			if (Value::DefaultValuesAreEqual(key_value, value)) {
				return true;
			}
		}
		return false;
	}
	case TableFilterType::OPTIONAL_FILTER:
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		// GPT-5.6: optional/dynamic filter 只是 scan hint，忽略它们仍保持 SQL 结果正确。
		return true;
	default:
		throw InternalException("Unsupported hft_ob key filter type");
	}
}

static bool HftKeyEquality(const TableFilter &filter, int32_t &key) {
	if (filter.filter_type == TableFilterType::CONSTANT_COMPARISON) {
		auto &constant = filter.Cast<ConstantFilter>();
		if (constant.comparison_type != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		key = constant.constant.GetValue<int32_t>();
		return true;
	}
	if (filter.filter_type != TableFilterType::CONJUNCTION_AND) {
		return false;
	}
	auto &conjunction = filter.Cast<ConjunctionAndFilter>();
	for (auto &child : conjunction.child_filters) {
		if (HftKeyEquality(*child, key)) {
			return true;
		}
	}
	return false;
}

class HftLmdbCursor {
public:
	explicit HftLmdbCursor(const string &path_p)
	    : path(path_p), env(nullptr), txn(nullptr), cursor(nullptr), action_count(0), actions(nullptr), key(0) {
		Check("create LMDB environment", mdb_env_create(&env));
		Check("open LMDB environment",
		      mdb_env_open(env, path.c_str(), MDB_RDONLY | MDB_NOSUBDIR | MDB_NOLOCK | MDB_NORDAHEAD, 0444));
		Check("begin LMDB transaction", mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn));
		Check("open LMDB database", mdb_dbi_open(txn, nullptr, 0, &dbi));
	}

	~HftLmdbCursor() {
		Close();
	}

	bool Get(int32_t exact_key, const TableFilter *filter) {
		MDB_val key_value {sizeof(exact_key), &exact_key};
		MDB_val value;
		auto rc = mdb_get(txn, dbi, &key_value, &value);
		if (rc == MDB_NOTFOUND) {
			return false;
		}
		Check("read LMDB value", rc);
		return Load(key_value, value, filter);
	}

	bool First(const TableFilter *filter) {
		if (!cursor) {
			Check("open LMDB cursor", mdb_cursor_open(txn, dbi, &cursor));
		}
		return Move(MDB_FIRST, filter);
	}

	bool Next(const TableFilter *filter) {
		return Move(MDB_NEXT, filter);
	}

	int32_t Key() const {
		return key;
	}

	idx_t Count() const {
		return action_count;
	}

	const_data_ptr_t Action(idx_t index) const {
		return actions + index * HFT_ACTION_SIZE;
	}

private:
	bool Move(MDB_cursor_op operation, const TableFilter *filter) {
		MDB_val key_value;
		MDB_val value;
		auto rc = mdb_cursor_get(cursor, &key_value, &value, operation);
		while (rc == 0) {
			if (Load(key_value, value, filter)) {
				return true;
			}
			rc = mdb_cursor_get(cursor, &key_value, &value, MDB_NEXT);
		}
		if (rc != MDB_NOTFOUND) {
			Check("advance LMDB cursor", rc);
		}
		return false;
	}

	bool Load(const MDB_val &key_value, const MDB_val &value, const TableFilter *filter) {
		if (key_value.mv_size != sizeof(key)) {
			throw InvalidInputException("Invalid HFT action key size %llu in '%s'", key_value.mv_size, path);
		}
		std::memcpy(&key, key_value.mv_data, sizeof(key));
		if (filter && !HftKeyMatchesFilter(key, *filter)) {
			return false;
		}

		int count;
		try {
			count = c_database_n(value.mv_size);
		} catch (int) {
			throw InvalidInputException("Invalid HFT action value for key %d in '%s'", key, path);
		}
		if (count < 0 || NumericCast<idx_t>(count) > value.mv_size / HFT_ACTION_SIZE) {
			throw InvalidInputException("Invalid HFT action value for key %d in '%s'", key, path);
		}
		action_count = NumericCast<idx_t>(count);
		actions = static_cast<const_data_ptr_t>(value.mv_data) + value.mv_size - action_count * HFT_ACTION_SIZE;
		return action_count != 0;
	}

	void Check(const char *operation, int rc) {
		if (!rc) {
			return;
		}
		auto message = string(mdb_strerror(rc));
		Close();
		throw IOException("Failed to %s for HFT ACTION-bin '%s': %s", operation, path, message);
	}

	void Close() {
		if (cursor) {
			mdb_cursor_close(cursor);
			cursor = nullptr;
		}
		if (txn) {
			mdb_txn_abort(txn);
			txn = nullptr;
		}
		if (env) {
			mdb_env_close(env);
			env = nullptr;
		}
	}

	string path;
	MDB_env *env;
	MDB_txn *txn;
	MDB_dbi dbi;
	MDB_cursor *cursor;
	idx_t action_count;
	const_data_ptr_t actions;
	int32_t key;
};

struct HftObAllBindData : public TableFunctionData {
	HftObAllBindData(string path_p, idx_t source_count_p) : path(std::move(path_p)), source_count(source_count_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<HftObAllBindData>(path, source_count);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<HftObAllBindData>();
		return path == other.path && source_count == other.source_count;
	}

	string path;
	idx_t source_count;
};

struct HftObAllGlobalState : public GlobalTableFunctionState {
	HftObAllGlobalState(const string &path, vector<column_t> column_ids_p, unique_ptr<TableFilter> key_filter_p)
	    : key_filter(std::move(key_filter_p)), cursor(path), column_ids(std::move(column_ids_p)), action_offset(0),
	      row_offset(0), is_exact(false), has_value(false) {
		int32_t exact_key;
		if (key_filter && HftKeyEquality(*key_filter, exact_key)) {
			is_exact = true;
			has_value = cursor.Get(exact_key, key_filter.get());
		} else {
			has_value = cursor.First(key_filter.get());
		}
	}

	bool NextKey() {
		orderbook.Reset();
		action_offset = 0;
		has_value = !is_exact && cursor.Next(key_filter.get());
		return has_value;
	}

	unique_ptr<TableFilter> key_filter;
	HftLmdbCursor cursor;
	HftOrderbookState orderbook;
	vector<column_t> column_ids;
	idx_t action_offset;
	idx_t row_offset;
	bool is_exact;
	bool has_value;
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

static unique_ptr<FunctionData> HftObBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("hft_ob filename and key cannot be NULL");
	}
	auto path = StringValue::Get(input.inputs[0]);
	auto key = input.inputs[1].GetValue<int32_t>();
	HftLmdbValue value(path, key);
	auto sources = HftSources();
	for (auto &source : sources) {
		names.emplace_back(source);
		return_types.emplace_back(LogicalType::FLOAT);
	}
	return make_uniq<HftObBindData>(std::move(path), key, value.Count(), sources.size());
}

static unique_ptr<GlobalTableFunctionState> HftObInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HftObBindData>();
	return make_uniq<HftObGlobalState>(bind_data.path, bind_data.key, input.column_ids);
}

static unique_ptr<NodeStatistics> HftObCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<HftObBindData>();
	return make_uniq<NodeStatistics>(bind_data.action_count, bind_data.action_count);
}

static void HftObScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<HftObBindData>();
	auto &state = input.global_state->Cast<HftObGlobalState>();
	if (state.action_offset >= bind_data.action_count) {
		return;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.action_count - state.action_offset);
	if (!state.column_ids.empty()) {
		vector<float> source_values(bind_data.source_count);
		for (idx_t row = 0; row < count; row++) {
			c_orderbook_extract(state.orderbook.bytes, state.value.Action(state.action_offset + row), 1,
			                    source_values.data());
			for (idx_t output_column = 0; output_column < state.column_ids.size(); output_column++) {
				auto column_id = state.column_ids[output_column];
				if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
					FlatVector::GetData<int64_t>(output.data[output_column])[row] =
					    NumericCast<int64_t>(state.action_offset + row);
					continue;
				}
				FlatVector::GetData<float>(output.data[output_column])[row] =
				    source_values[NumericCast<idx_t>(column_id)];
			}
		}
	}
	state.action_offset += count;
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> HftObAllBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("hft_ob filename cannot be NULL");
	}
	auto path = StringValue::Get(input.inputs[0]);
	HftLmdbCursor database(path);
	auto sources = HftSources();
	names.emplace_back("key");
	return_types.emplace_back(LogicalType::INTEGER);
	for (auto &source : sources) {
		names.emplace_back(source);
		return_types.emplace_back(LogicalType::FLOAT);
	}
	return make_uniq<HftObAllBindData>(std::move(path), sources.size());
}

static unique_ptr<TableFilter> HftObAllKeyFilter(TableFunctionInitInput &input) {
	if (!input.filters) {
		return nullptr;
	}
	for (auto &entry : input.filters->filters) {
		if (entry.first >= input.column_ids.size()) {
			throw InternalException("hft_ob filter column is out of range");
		}
		if (input.column_ids[entry.first] == 0) {
			return entry.second->Copy();
		}
	}
	return nullptr;
}

static vector<column_t> HftObAllOutputColumns(TableFunctionInitInput &input) {
	if (input.projection_ids.empty()) {
		return input.column_ids;
	}
	vector<column_t> result;
	result.reserve(input.projection_ids.size());
	for (auto projection_id : input.projection_ids) {
		result.push_back(input.column_ids[projection_id]);
	}
	return result;
}

static unique_ptr<GlobalTableFunctionState> HftObAllInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HftObAllBindData>();
	return make_uniq<HftObAllGlobalState>(bind_data.path, HftObAllOutputColumns(input), HftObAllKeyFilter(input));
}

static bool HftObAllSupportsPushdownType(const FunctionData &bind_data, idx_t column_index) {
	return column_index == 0;
}

static void HftObAllScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<HftObAllBindData>();
	auto &state = input.global_state->Cast<HftObAllGlobalState>();
	idx_t output_count = 0;
	bool needs_orderbook = false;
	for (auto column_id : state.column_ids) {
		if (column_id != 0 && column_id != COLUMN_IDENTIFIER_ROW_ID) {
			needs_orderbook = true;
			break;
		}
	}
	vector<float> source_values;
	if (needs_orderbook) {
		source_values.resize(bind_data.source_count);
	}

	while (output_count < STANDARD_VECTOR_SIZE && state.has_value) {
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE - output_count, state.cursor.Count() - state.action_offset);
		for (idx_t row = 0; row < count; row++) {
			if (needs_orderbook) {
				c_orderbook_extract(state.orderbook.bytes, state.cursor.Action(state.action_offset + row), 1,
				                    source_values.data());
			}
			for (idx_t output_column = 0; output_column < state.column_ids.size(); output_column++) {
				auto column_id = state.column_ids[output_column];
				if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
					FlatVector::GetData<int64_t>(output.data[output_column])[output_count + row] =
					    NumericCast<int64_t>(state.row_offset + row);
				} else if (column_id == 0) {
					FlatVector::GetData<int32_t>(output.data[output_column])[output_count + row] = state.cursor.Key();
				} else {
					FlatVector::GetData<float>(output.data[output_column])[output_count + row] =
					    source_values[NumericCast<idx_t>(column_id - 1)];
				}
			}
		}
		state.action_offset += count;
		state.row_offset += count;
		output_count += count;
		if (state.action_offset == state.cursor.Count()) {
			state.NextKey();
		}
	}
	output.SetCardinality(output_count);
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_decision("read_twn_decision", {LogicalType::VARCHAR}, TwnScan, TwnDecisionBind, TwnScanInit);
	read_decision.projection_pushdown = true;
	loader.RegisterFunction(read_decision);

	TableFunction read_execution("read_twn_execution", {LogicalType::VARCHAR}, TwnScan, TwnExecutionBind, TwnScanInit);
	read_execution.projection_pushdown = true;
	loader.RegisterFunction(read_execution);

	TableFunction hft_ob_key({LogicalType::VARCHAR, LogicalType::INTEGER}, HftObScan, HftObBind, HftObInit);
	hft_ob_key.projection_pushdown = true;
	hft_ob_key.cardinality = HftObCardinality;

	TableFunction hft_ob_all({LogicalType::VARCHAR}, HftObAllScan, HftObAllBind, HftObAllInit);
	hft_ob_all.projection_pushdown = true;
	hft_ob_all.filter_pushdown = true;
	hft_ob_all.filter_prune = true;
	hft_ob_all.supports_pushdown_type = HftObAllSupportsPushdownType;

	TableFunctionSet hft_ob("hft_ob");
	hft_ob.AddFunction(std::move(hft_ob_key));
	hft_ob.AddFunction(std::move(hft_ob_all));
	loader.RegisterFunction(std::move(hft_ob));
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
