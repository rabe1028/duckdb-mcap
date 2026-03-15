#define DUCKDB_EXTENSION_MAIN
#define MCAP_IMPLEMENTATION

#include "mcap_extension.hpp"
#include "cdr_deserializer.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"

// zstd: reuse DuckDB's bundled zstd (lives in duckdb_zstd namespace)
#include <zstd.h>
#include <zstd_errors.h>
using namespace duckdb_zstd; // NOLINT

// lz4: bundled in third_party/lz4 (global namespace, no conflict)
#include <mcap/reader.hpp>

#include <memory>
#include <unordered_set>

namespace duckdb {

// ── Shared: open reader and read summary ─────────────────────────────────────
static void OpenAndReadSummary(mcap::McapReader &reader, const std::string &path) {
	auto status = reader.open(path);
	if (!status.ok()) {
		throw IOException("Failed to open MCAP file '%s': %s", path, status.message);
	}
	auto sum = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
	if (!sum.ok()) {
		throw IOException("Failed to read MCAP summary for '%s': %s", path, sum.message);
	}
}

// ── Shared: file-path-only bind data ─────────────────────────────────────────
struct McapFileBindData : public TableFunctionData {
	string file_path;
};

// ── Shared: Global state for message iteration ──────────────────────────────
struct McapIterState : public GlobalTableFunctionState {
	mcap::McapReader reader;
	bool opened = false;
	bool done = false;
	std::unique_ptr<mcap::LinearMessageView> message_view;
	std::optional<mcap::LinearMessageView::Iterator> it;

	idx_t MaxThreads() const override {
		return 1;
	}

	void Open(const std::string &path) {
		OpenAndReadSummary(reader, path);
		auto on_problem = [](const mcap::Status &) {
		};
		message_view = std::make_unique<mcap::LinearMessageView>(reader.readMessages(on_problem));
		it = message_view->begin();
		opened = true;
	}
};

// ── Shared: field → LogicalType (single field, handles byte→BLOB, array→LIST) ─
static LogicalType PrimTypeToLogical(const std::string &type_name) {
	if (type_name == "bool")
		return LogicalType(LogicalTypeId::BOOLEAN);
	if (type_name == "int8")
		return LogicalType(LogicalTypeId::TINYINT);
	if (cdr::IsByteType(type_name))
		return LogicalType(LogicalTypeId::UTINYINT);
	if (type_name == "int16")
		return LogicalType(LogicalTypeId::SMALLINT);
	if (type_name == "uint16")
		return LogicalType(LogicalTypeId::USMALLINT);
	if (type_name == "int32")
		return LogicalType(LogicalTypeId::INTEGER);
	if (type_name == "uint32")
		return LogicalType(LogicalTypeId::UINTEGER);
	if (type_name == "int64")
		return LogicalType(LogicalTypeId::BIGINT);
	if (type_name == "uint64")
		return LogicalType(LogicalTypeId::UBIGINT);
	if (type_name == "float32")
		return LogicalType(LogicalTypeId::FLOAT);
	if (type_name == "float64")
		return LogicalType(LogicalTypeId::DOUBLE);
	if (type_name == "string")
		return LogicalType(LogicalTypeId::VARCHAR);
	return LogicalType(LogicalTypeId::VARCHAR);
}

static LogicalType MsgDefToLogical(const cdr::MsgDef &msg, const std::unordered_map<std::string, cdr::MsgDef> &types);

static LogicalType FieldTypeToLogical(const std::string &type_name,
                                      const std::unordered_map<std::string, cdr::MsgDef> &types) {
	if (cdr::GetPrimType(type_name).has_value())
		return PrimTypeToLogical(type_name);
	const cdr::MsgDef *def = cdr::FindType(type_name, types);
	if (def)
		return MsgDefToLogical(*def, types);
	return LogicalType(LogicalTypeId::VARCHAR);
}

// Maps a FieldDef to the appropriate DuckDB LogicalType (byte[]→BLOB, other[]→LIST).
static LogicalType FieldDefToLogical(const cdr::FieldDef &field,
                                     const std::unordered_map<std::string, cdr::MsgDef> &types) {
	auto base_type = FieldTypeToLogical(field.type_name, types);
	if (field.is_array) {
		return cdr::IsByteType(field.type_name) ? LogicalType(LogicalTypeId::BLOB) : LogicalType::LIST(base_type);
	}
	return base_type;
}

static LogicalType MsgDefToLogical(const cdr::MsgDef &msg, const std::unordered_map<std::string, cdr::MsgDef> &types) {
	child_list_t<LogicalType> children;
	for (const auto &field : msg.fields) {
		children.push_back(make_pair(field.field_name, FieldDefToLogical(field, types)));
	}
	return LogicalType::STRUCT(children);
}

// ── CDR binary → DuckDB Value ───────────────────────────────────────────────
static Value CdrFieldToValue(cdr::CdrReader &reader, const std::string &type_name,
                             const std::unordered_map<std::string, cdr::MsgDef> &types);

// Decode a single array field to a DuckDB Value (BLOB for byte arrays, LIST otherwise).
static Value CdrArrayToValue(cdr::CdrReader &reader, const cdr::FieldDef &field,
                             const std::unordered_map<std::string, cdr::MsgDef> &types) {
	uint32_t cnt = reader.ReadArrayCount(field);
	if (cdr::IsByteType(field.type_name)) {
		auto blob_data = reader.ReadBytes(cnt);
		return Value::BLOB(reinterpret_cast<const_data_ptr_t>(blob_data.data()), static_cast<idx_t>(blob_data.size()));
	}
	vector<Value> elements;
	elements.reserve(cnt);
	auto elem_type = FieldTypeToLogical(field.type_name, types);
	for (uint32_t i = 0; i < cnt && reader.Ok(); i++)
		elements.push_back(CdrFieldToValue(reader, field.type_name, types));
	return Value::LIST(elem_type, std::move(elements));
}

static Value CdrMsgToValue(cdr::CdrReader &reader, const cdr::MsgDef &msg,
                           const std::unordered_map<std::string, cdr::MsgDef> &types) {
	child_list_t<Value> children;
	for (const auto &field : msg.fields) {
		Value val =
		    field.is_array ? CdrArrayToValue(reader, field, types) : CdrFieldToValue(reader, field.type_name, types);
		children.push_back(make_pair(field.field_name, std::move(val)));
	}
	return Value::STRUCT(std::move(children));
}

static Value CdrFieldToValue(cdr::CdrReader &reader, const std::string &type_name,
                             const std::unordered_map<std::string, cdr::MsgDef> &types) {
	auto prim = cdr::GetPrimType(type_name);
	if (prim.has_value()) {
		switch (*prim) {
		case cdr::PrimType::BOOL:
			return Value::BOOLEAN(reader.ReadBool());
		case cdr::PrimType::INT8:
			return Value::TINYINT(reader.ReadInt8());
		case cdr::PrimType::UINT8:
		case cdr::PrimType::BYTE:
		case cdr::PrimType::CHAR:
			return Value::UTINYINT(reader.ReadUint8());
		case cdr::PrimType::INT16:
			return Value::SMALLINT(reader.ReadInt16());
		case cdr::PrimType::UINT16:
			return Value::USMALLINT(reader.ReadUint16());
		case cdr::PrimType::INT32:
			return Value::INTEGER(reader.ReadInt32());
		case cdr::PrimType::UINT32:
			return Value::UINTEGER(reader.ReadUint32());
		case cdr::PrimType::INT64:
			return Value::BIGINT(reader.ReadInt64());
		case cdr::PrimType::UINT64:
			return Value::UBIGINT(reader.ReadUint64());
		case cdr::PrimType::FLOAT32:
			return Value::FLOAT(reader.ReadFloat32());
		case cdr::PrimType::FLOAT64:
			return Value::DOUBLE(reader.ReadFloat64());
		case cdr::PrimType::STRING:
			return Value(reader.ReadString());
		}
	}
	const cdr::MsgDef *def = cdr::FindType(type_name, types);
	if (def)
		return CdrMsgToValue(reader, *def, types);
	return Value();
}

// ═════════════════════════════════════════════════════════════════════════════
// read_mcap_channel(filename, topic) — CDR decoded to native columns
// ═════════════════════════════════════════════════════════════════════════════

struct ReadMcapChannelBindData : public TableFunctionData {
	string file_path;
	string topic;
	// A topic can span multiple MCAP channels (one per publisher connection)
	std::unordered_set<mcap::ChannelId> target_channel_ids;
	string schema_name;
	std::unordered_map<std::string, cdr::MsgDef> types;
};

static unique_ptr<FunctionData> ReadMcapChannelBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadMcapChannelBindData>();
	result->file_path = input.inputs[0].GetValue<string>();
	result->topic = input.inputs[1].GetValue<string>();

	mcap::McapReader bind_reader;
	OpenAndReadSummary(bind_reader, result->file_path);

	bool found = false;
	const auto &schemas_map = bind_reader.schemas();
	for (const auto &[ch_id, ch_ptr] : bind_reader.channels()) {
		if (ch_ptr->topic == result->topic) {
			result->target_channel_ids.insert(ch_ptr->id);
			// Parse schema from the first matching channel (all channels for the same topic share schema)
			if (!found) {
				auto schema_it = schemas_map.find(ch_ptr->schemaId);
				if (schema_it != schemas_map.end()) {
					const auto &schema = *schema_it->second;
					result->schema_name = schema.name;
					std::string schema_text(reinterpret_cast<const char *>(schema.data.data()), schema.data.size());
					result->types = cdr::MsgParser::Parse(schema_text, schema.name);
					found = true;
				}
			}
		}
	}
	bind_reader.close();

	if (!found) {
		throw IOException("Topic '%s' not found in MCAP file '%s'", result->topic, result->file_path);
	}

	names.emplace_back("sequence");
	return_types.emplace_back(LogicalType(LogicalTypeId::UINTEGER));
	names.emplace_back("log_time");
	return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP_NS));
	names.emplace_back("publish_time");
	return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP_NS));

	auto main_it = result->types.find(result->schema_name);
	if (main_it != result->types.end()) {
		for (const auto &field : main_it->second.fields) {
			names.emplace_back(field.field_name);
			return_types.emplace_back(FieldDefToLogical(field, result->types));
		}
	}

	return result;
}

static unique_ptr<GlobalTableFunctionState> ReadMcapChannelInitGlobal(ClientContext &context,
                                                                      TableFunctionInitInput &input) {
	return make_uniq<McapIterState>();
}

static void ReadMcapChannelFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ReadMcapChannelBindData>();
	auto &gstate = data_p.global_state->Cast<McapIterState>();

	if (gstate.done) {
		output.SetCardinality(0);
		return;
	}
	if (!gstate.opened) {
		gstate.Open(bind_data.file_path);
	}

	auto main_it = bind_data.types.find(bind_data.schema_name);
	if (main_it == bind_data.types.end()) {
		output.SetCardinality(0);
		gstate.done = true;
		return;
	}
	const auto &msg_def = main_it->second;
	const idx_t n_fields = msg_def.fields.size();

	idx_t count = 0;
	const auto end = gstate.message_view->end();

	while (count < STANDARD_VECTOR_SIZE && *gstate.it != end) {
		const auto &msg_view = **gstate.it;
		const auto &msg = msg_view.message;

		// Filter by channel ID set (integer lookup, not string comparison)
		if (bind_data.target_channel_ids.find(msg.channelId) == bind_data.target_channel_ids.end()) {
			++(*gstate.it);
			continue;
		}

		output.data[0].SetValue(count, Value::UINTEGER(msg.sequence));
		output.data[1].SetValue(count, Value::TIMESTAMPNS(timestamp_ns_t(static_cast<int64_t>(msg.logTime))));
		output.data[2].SetValue(count, Value::TIMESTAMPNS(timestamp_ns_t(static_cast<int64_t>(msg.publishTime))));

		if (msg_view.channel->messageEncoding == "cdr" && msg.data && msg.dataSize > 4) {
			cdr::CdrReader cdr_reader(reinterpret_cast<const uint8_t *>(msg.data), msg.dataSize);
			idx_t fi = 0;
			for (; fi < n_fields && cdr_reader.Ok(); fi++) {
				const auto &field = msg_def.fields[fi];
				if (field.is_array) {
					output.data[3 + fi].SetValue(count, CdrArrayToValue(cdr_reader, field, bind_data.types));
				} else {
					output.data[3 + fi].SetValue(count, CdrFieldToValue(cdr_reader, field.type_name, bind_data.types));
				}
			}
			// NULL-fill remaining fields if CDR decode failed mid-message
			for (; fi < n_fields; fi++) {
				output.data[3 + fi].SetValue(count, Value());
			}
		} else {
			for (idx_t fi = 0; fi < n_fields; fi++)
				output.data[3 + fi].SetValue(count, Value());
		}

		++count;
		++(*gstate.it);
	}

	if (*gstate.it == end)
		gstate.done = true;
	output.SetCardinality(count);
}

// ═════════════════════════════════════════════════════════════════════════════
// mcap_channels / mcap_schemas / mcap_statistics
// ═════════════════════════════════════════════════════════════════════════════

// ── mcap_channels ───────────────────────────────────────────────────────────
struct McapChannelsGlobalState : public GlobalTableFunctionState {
	struct ChannelInfo {
		mcap::ChannelId id;
		string topic;
		string message_encoding;
		mcap::SchemaId schema_id;
		string schema_name;
		string metadata_str;
	};
	vector<ChannelInfo> channels;
	idx_t current_idx = 0;
};

static unique_ptr<FunctionData> McapChannelsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<McapFileBindData>();
	result->file_path = input.inputs[0].GetValue<string>();
	names.emplace_back("channel_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::USMALLINT));
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("message_encoding");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("schema_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::USMALLINT));
	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("metadata");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	return result;
}

static unique_ptr<GlobalTableFunctionState> McapChannelsInitGlobal(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<McapFileBindData>();
	auto gstate = make_uniq<McapChannelsGlobalState>();
	mcap::McapReader reader;
	OpenAndReadSummary(reader, bind_data.file_path);
	auto schemas = reader.schemas();
	for (const auto &[id, ch_ptr] : reader.channels()) {
		McapChannelsGlobalState::ChannelInfo info;
		info.id = ch_ptr->id;
		info.topic = ch_ptr->topic;
		info.message_encoding = ch_ptr->messageEncoding;
		info.schema_id = ch_ptr->schemaId;
		auto sit = schemas.find(ch_ptr->schemaId);
		if (sit != schemas.end())
			info.schema_name = sit->second->name;
		string meta;
		for (const auto &[k, v] : ch_ptr->metadata) {
			if (!meta.empty())
				meta += ", ";
			meta += k + "=" + v;
		}
		info.metadata_str = meta;
		gstate->channels.push_back(std::move(info));
	}
	reader.close();
	return gstate;
}

static void McapChannelsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<McapChannelsGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.current_idx < gstate.channels.size()) {
		const auto &ch = gstate.channels[gstate.current_idx];
		output.data[0].SetValue(count, Value::USMALLINT(ch.id));
		output.data[1].SetValue(count, Value(ch.topic));
		output.data[2].SetValue(count, Value(ch.message_encoding));
		output.data[3].SetValue(count, Value::USMALLINT(ch.schema_id));
		output.data[4].SetValue(count, Value(ch.schema_name));
		output.data[5].SetValue(count, Value(ch.metadata_str));
		++count;
		++gstate.current_idx;
	}
	output.SetCardinality(count);
}

// ── mcap_schemas ────────────────────────────────────────────────────────────
struct McapSchemasGlobalState : public GlobalTableFunctionState {
	vector<mcap::Schema> schemas;
	idx_t current_idx = 0;
};

static unique_ptr<FunctionData> McapSchemasBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<McapFileBindData>();
	result->file_path = input.inputs[0].GetValue<string>();
	names.emplace_back("schema_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::USMALLINT));
	names.emplace_back("name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("encoding");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("data");
	return_types.emplace_back(LogicalType(LogicalTypeId::BLOB));
	return result;
}

static unique_ptr<GlobalTableFunctionState> McapSchemasInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<McapFileBindData>();
	auto gstate = make_uniq<McapSchemasGlobalState>();
	mcap::McapReader reader;
	OpenAndReadSummary(reader, bind_data.file_path);
	for (const auto &[id, schema_ptr] : reader.schemas())
		gstate->schemas.push_back(*schema_ptr);
	reader.close();
	return gstate;
}

static void McapSchemasFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<McapSchemasGlobalState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && gstate.current_idx < gstate.schemas.size()) {
		const auto &schema = gstate.schemas[gstate.current_idx];
		output.data[0].SetValue(count, Value::USMALLINT(schema.id));
		output.data[1].SetValue(count, Value(schema.name));
		output.data[2].SetValue(count, Value(schema.encoding));
		if (!schema.data.empty()) {
			output.data[3].SetValue(
			    count, Value::BLOB(reinterpret_cast<const_data_ptr_t>(schema.data.data()), schema.data.size()));
		} else {
			output.data[3].SetValue(count, Value(LogicalType(LogicalTypeId::BLOB)));
		}
		++count;
		++gstate.current_idx;
	}
	output.SetCardinality(count);
}

// ── mcap_statistics ─────────────────────────────────────────────────────────
struct McapStatisticsGlobalState : public GlobalTableFunctionState {
	bool done = false;
	// Pre-read values from init
	std::optional<mcap::Statistics> stats;
	std::optional<mcap::Header> header;
};

static unique_ptr<FunctionData> McapStatisticsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<McapFileBindData>();
	result->file_path = input.inputs[0].GetValue<string>();
	names.emplace_back("message_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::UBIGINT));
	names.emplace_back("schema_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::USMALLINT));
	names.emplace_back("channel_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::UINTEGER));
	names.emplace_back("attachment_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::UINTEGER));
	names.emplace_back("metadata_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::UINTEGER));
	names.emplace_back("chunk_count");
	return_types.emplace_back(LogicalType(LogicalTypeId::UINTEGER));
	names.emplace_back("message_start_time");
	return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP_NS));
	names.emplace_back("message_end_time");
	return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP_NS));
	names.emplace_back("profile");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("library");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	return result;
}

static unique_ptr<GlobalTableFunctionState> McapStatisticsInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<McapFileBindData>();
	auto gstate = make_uniq<McapStatisticsGlobalState>();
	mcap::McapReader reader;
	OpenAndReadSummary(reader, bind_data.file_path);
	gstate->stats = reader.statistics();
	gstate->header = reader.header();
	reader.close();
	return gstate;
}

static void McapStatisticsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<McapStatisticsGlobalState>();
	if (gstate.done) {
		output.SetCardinality(0);
		return;
	}
	if (gstate.stats.has_value()) {
		const auto &s = *gstate.stats;
		output.data[0].SetValue(0, Value::UBIGINT(s.messageCount));
		output.data[1].SetValue(0, Value::USMALLINT(s.schemaCount));
		output.data[2].SetValue(0, Value::UINTEGER(s.channelCount));
		output.data[3].SetValue(0, Value::UINTEGER(s.attachmentCount));
		output.data[4].SetValue(0, Value::UINTEGER(s.metadataCount));
		output.data[5].SetValue(0, Value::UINTEGER(s.chunkCount));
		output.data[6].SetValue(0, Value::TIMESTAMPNS(timestamp_ns_t(static_cast<int64_t>(s.messageStartTime))));
		output.data[7].SetValue(0, Value::TIMESTAMPNS(timestamp_ns_t(static_cast<int64_t>(s.messageEndTime))));
	} else {
		for (idx_t i = 0; i < 8; i++)
			output.data[i].SetValue(0, Value());
	}
	if (gstate.header.has_value()) {
		output.data[8].SetValue(0, Value(gstate.header->profile));
		output.data[9].SetValue(0, Value(gstate.header->library));
	} else {
		output.data[8].SetValue(0, Value());
		output.data[9].SetValue(0, Value());
	}
	output.SetCardinality(1);
	gstate.done = true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Extension Registration
// ═════════════════════════════════════════════════════════════════════════════
static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_mcap_channel("read_mcap_channel",
	                                {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR)},
	                                ReadMcapChannelFunction, ReadMcapChannelBind, ReadMcapChannelInitGlobal);
	loader.RegisterFunction(read_mcap_channel);

	TableFunction mcap_channels("mcap_channels", {LogicalType(LogicalTypeId::VARCHAR)}, McapChannelsFunction,
	                            McapChannelsBind, McapChannelsInitGlobal);
	loader.RegisterFunction(mcap_channels);

	TableFunction mcap_schemas("mcap_schemas", {LogicalType(LogicalTypeId::VARCHAR)}, McapSchemasFunction,
	                           McapSchemasBind, McapSchemasInitGlobal);
	loader.RegisterFunction(mcap_schemas);

	TableFunction mcap_statistics("mcap_statistics", {LogicalType(LogicalTypeId::VARCHAR)}, McapStatisticsFunction,
	                              McapStatisticsBind, McapStatisticsInitGlobal);
	loader.RegisterFunction(mcap_statistics);
}

void McapExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string McapExtension::Name() {
	return "mcap";
}

std::string McapExtension::Version() const {
#ifdef EXT_VERSION_MCAP
	return EXT_VERSION_MCAP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mcap, loader) {
	duckdb::LoadInternal(loader);
}
}
