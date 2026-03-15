#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cdr {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr uint32_t MAX_ARRAY_SIZE = 100000;

// ── Primitive Types ────────────────────────────────────────────────────────
enum class PrimType {
	BOOL,
	INT8,
	UINT8,
	INT16,
	UINT16,
	INT32,
	UINT32,
	INT64,
	UINT64,
	FLOAT32,
	FLOAT64,
	STRING,
	BYTE,
	CHAR
};

inline std::optional<PrimType> GetPrimType(const std::string &type_name) {
	static const std::unordered_map<std::string, PrimType> map = {
	    {"bool", PrimType::BOOL},       {"int8", PrimType::INT8},     {"uint8", PrimType::UINT8},
	    {"byte", PrimType::BYTE},       {"char", PrimType::CHAR},     {"int16", PrimType::INT16},
	    {"uint16", PrimType::UINT16},   {"int32", PrimType::INT32},   {"uint32", PrimType::UINT32},
	    {"int64", PrimType::INT64},     {"uint64", PrimType::UINT64}, {"float32", PrimType::FLOAT32},
	    {"float64", PrimType::FLOAT64}, {"string", PrimType::STRING},
	};
	auto it = map.find(type_name);
	if (it != map.end())
		return it->second;
	return std::nullopt;
}

inline bool IsByteType(const std::string &type_name) {
	return type_name == "uint8" || type_name == "byte" || type_name == "char";
}

// ── Field / Message Definitions ────────────────────────────────────────────
struct FieldDef {
	std::string type_name;  // "float64", "geometry_msgs/msg/Point", etc.
	std::string field_name; // "x", "position", etc.
	bool is_array = false;
	int32_t array_size = -1; // -1 = dynamic (sequence), >= 0 = fixed
};

struct MsgDef {
	std::string full_name;
	std::vector<FieldDef> fields;
};

// ── ParseFieldLine: parse a single msg line into FieldDef (pure function) ──
inline std::optional<FieldDef> ParseFieldLine(const std::string &line) {
	auto start = line.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
		return std::nullopt;
	auto end = line.find_last_not_of(" \t\r\n");
	auto trimmed = line.substr(start, end - start + 1);

	if (trimmed.empty() || trimmed[0] == '#')
		return std::nullopt;

	std::istringstream stream(trimmed);
	std::string type_str, field_name;
	if (!(stream >> type_str >> field_name))
		return std::nullopt;

	// Skip constants: "uint8 FOO=42"
	if (field_name.find('=') != std::string::npos)
		return std::nullopt;

	FieldDef field;
	field.field_name = field_name;

	// Strip bounded type suffix like string<=256
	auto leq = type_str.find("<=");
	if (leq != std::string::npos)
		type_str = type_str.substr(0, leq);

	// Array notation: type[] or type[N]
	auto br_open = type_str.find('[');
	if (br_open != std::string::npos) {
		field.is_array = true;
		auto br_close = type_str.find(']', br_open);
		if (br_close != std::string::npos) {
			std::string size_str = type_str.substr(br_open + 1, br_close - br_open - 1);
			if (!size_str.empty()) {
				try {
					field.array_size = std::stoi(size_str);
				} catch (...) {
					field.array_size = -1;
				}
			}
		}
		type_str = type_str.substr(0, br_open);
	}

	field.type_name = type_str;
	return field;
}

// ── ROS2 Msg Schema Parser ────────────────────────────────────────────────
class MsgParser {
public:
	static std::unordered_map<std::string, MsgDef> Parse(const std::string &schema_text, const std::string &main_type) {
		std::unordered_map<std::string, MsgDef> result;
		auto sections = SplitSections(schema_text);

		if (!sections.empty()) {
			MsgDef def;
			def.full_name = main_type;
			def.fields = ParseFields(sections[0].second);
			result[main_type] = std::move(def);
		}
		for (size_t i = 1; i < sections.size(); i++) {
			MsgDef def;
			def.full_name = sections[i].first;
			def.fields = ParseFields(sections[i].second);
			result[def.full_name] = std::move(def);
		}
		return result;
	}

private:
	static std::vector<std::pair<std::string, std::string>> SplitSections(const std::string &text) {
		std::vector<std::pair<std::string, std::string>> sections;
		std::istringstream stream(text);
		std::string line;
		std::string current_name;
		std::string current_body;
		bool has_content = false;

		while (std::getline(stream, line)) {
			if (line.size() >= 10 && line.find_first_not_of("= \t\r\n") == std::string::npos) {
				sections.emplace_back(current_name, current_body);
				current_body.clear();
				has_content = false;
				if (std::getline(stream, line)) {
					auto trimmed = Trim(line);
					if (trimmed.substr(0, 5) == "MSG: ") {
						current_name = trimmed.substr(5);
					}
				}
				continue;
			}
			current_body += line + "\n";
			has_content = true;
		}
		if (has_content || sections.empty()) {
			sections.emplace_back(current_name, current_body);
		}
		return sections;
	}

	static std::vector<FieldDef> ParseFields(const std::string &body) {
		std::vector<FieldDef> fields;
		std::istringstream stream(body);
		std::string line;
		while (std::getline(stream, line)) {
			auto field = ParseFieldLine(line);
			if (field.has_value()) {
				fields.push_back(std::move(*field));
			}
		}
		return fields;
	}

	static std::string Trim(const std::string &s) {
		auto start = s.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return "";
		auto end = s.find_last_not_of(" \t\r\n");
		return s.substr(start, end - start + 1);
	}
};

// ── CDR Binary Reader ──────────────────────────────────────────────────────
class CdrReader {
public:
	CdrReader(const uint8_t *data, size_t size) {
		if (size < 4) {
			base_ = data;
			size_ = 0;
			valid_ = false;
			return;
		}
		le_ = (data[1] == 0x01 || data[1] == 0x06);
		base_ = data + 4;
		size_ = size - 4;
		valid_ = true;
	}

	CdrReader(const uint8_t *data, size_t size, bool little_endian)
	    : base_(data), size_(size), le_(little_endian), valid_(true) {
	}

	bool Ok() const {
		return valid_;
	}

	void Align(size_t alignment) {
		if (!valid_ || alignment <= 1)
			return;
		size_t rem = pos_ % alignment;
		if (rem != 0)
			pos_ += alignment - rem;
		if (pos_ > size_)
			valid_ = false;
	}

	bool ReadBool() {
		if (!valid_ || pos_ >= size_) {
			valid_ = false;
			return false;
		}
		return base_[pos_++] != 0;
	}

	int8_t ReadInt8() {
		if (!valid_ || pos_ >= size_) {
			valid_ = false;
			return 0;
		}
		return static_cast<int8_t>(base_[pos_++]);
	}

	uint8_t ReadUint8() {
		if (!valid_ || pos_ >= size_) {
			valid_ = false;
			return 0;
		}
		return base_[pos_++];
	}

	int16_t ReadInt16() {
		return ReadFixed<int16_t>(2);
	}
	uint16_t ReadUint16() {
		return ReadFixed<uint16_t>(2);
	}
	int32_t ReadInt32() {
		return ReadFixed<int32_t>(4);
	}
	uint32_t ReadUint32() {
		return ReadFixed<uint32_t>(4);
	}
	int64_t ReadInt64() {
		return ReadFixed<int64_t>(8);
	}
	uint64_t ReadUint64() {
		return ReadFixed<uint64_t>(8);
	}

	float ReadFloat32() {
		return ReadFixed<float>(4);
	}
	double ReadFloat64() {
		return ReadFixed<double>(8);
	}

	std::string ReadString() {
		uint32_t len = ReadUint32();
		if (!valid_ || len == 0 || pos_ + len > size_) {
			valid_ = false;
			return "";
		}
		std::string result(reinterpret_cast<const char *>(base_ + pos_), len - 1);
		pos_ += len;
		return result;
	}

	// Bulk read bytes (for uint8[] / byte[] arrays). Returns empty string on failure.
	std::string ReadBytes(size_t count) {
		if (!valid_ || pos_ + count > size_) {
			valid_ = false;
			return "";
		}
		std::string result(reinterpret_cast<const char *>(base_ + pos_), count);
		pos_ += count;
		return result;
	}

	// Read array element count (fixed or dynamic).
	// Byte arrays (uint8[]/byte[]) are bounded by the buffer size, so no artificial cap.
	// Non-byte arrays are capped at MAX_ARRAY_SIZE to prevent runaway struct decoding.
	uint32_t ReadArrayCount(const FieldDef &field) {
		uint32_t count;
		if (field.array_size >= 0) {
			count = static_cast<uint32_t>(field.array_size);
		} else {
			count = ReadUint32();
		}
		// Byte arrays are read in bulk (ReadBytes) and bounded by buffer size — no cap needed.
		// Non-byte arrays decode per-element, so cap to prevent excessive work on corrupt data.
		if (!IsByteType(field.type_name) && count > MAX_ARRAY_SIZE) {
			valid_ = false;
			return 0;
		}
		return count;
	}

private:
	template <typename T>
	T ReadFixed(size_t alignment) {
		Align(alignment);
		if (!valid_ || pos_ + sizeof(T) > size_) {
			valid_ = false;
			return T {};
		}
		T val;
		std::memcpy(&val, base_ + pos_, sizeof(T));
		pos_ += sizeof(T);
		if (!le_ && sizeof(T) > 1)
			val = ByteSwap(val);
		return val;
	}

	template <typename T>
	static T ByteSwap(T val) {
		T result;
		auto *src = reinterpret_cast<const uint8_t *>(&val);
		auto *dst = reinterpret_cast<uint8_t *>(&result);
		for (size_t i = 0; i < sizeof(T); i++)
			dst[i] = src[sizeof(T) - 1 - i];
		return result;
	}

	const uint8_t *base_ = nullptr;
	size_t size_ = 0;
	size_t pos_ = 0;
	bool le_ = true;
	bool valid_ = false;
};

// ── Type lookup helper (returns optional reference, not raw pointer) ────────
using TypeMap = std::unordered_map<std::string, MsgDef>;

inline std::optional<std::reference_wrapper<const MsgDef>> FindType(const std::string &type_name,
                                                                    const TypeMap &types) {
	// 1. Exact match
	auto it = types.find(type_name);
	if (it != types.end())
		return std::cref(it->second);

	// 2. Insert /msg/: "geometry_msgs/Point" → "geometry_msgs/msg/Point"
	auto slash = type_name.find('/');
	if (slash != std::string::npos && type_name.find("/msg/") == std::string::npos) {
		std::string with_msg = type_name.substr(0, slash) + "/msg/" + type_name.substr(slash + 1);
		it = types.find(with_msg);
		if (it != types.end())
			return std::cref(it->second);
	}

	// 3. Short name fallback: "Header" matches "std_msgs/msg/Header"
	for (const auto &[name, def] : types) {
		auto last_slash = name.rfind('/');
		if (last_slash != std::string::npos && name.substr(last_slash + 1) == type_name) {
			return std::cref(def);
		}
	}

	return std::nullopt;
}

} // namespace cdr
