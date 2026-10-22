#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace rooms::json {

namespace {

const Value kNull;

class Parser {
public:
	explicit Parser(std::string_view text) : s_(text) {}

	std::optional<Value> Document() {
		auto v = ParseValue(0);
		SkipSpace();
		if (!v || pos_ != s_.size()) return std::nullopt;
		return v;
	}

private:
	std::string_view s_;
	size_t pos_ = 0;

	void SkipSpace() {
		while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
	}

	bool Literal(std::string_view word) {
		if (s_.substr(pos_, word.size()) != word) return false;
		pos_ += word.size();
		return true;
	}

	std::optional<Value> ParseValue(int depth) {
		if (depth > 64) return std::nullopt;
		SkipSpace();
		if (pos_ >= s_.size()) return std::nullopt;
		const char c = s_[pos_];
		if (c == '{') return ParseObject(depth);
		if (c == '[') return ParseArray(depth);
		if (c == '"') {
			auto str = ParseString();
			if (!str) return std::nullopt;
			return Value::String(std::move(*str));
		}
		if (Literal("true")) return Value::Bool(true);
		if (Literal("false")) return Value::Bool(false);
		if (Literal("null")) return Value();
		return ParseNumber();
	}

	std::optional<Value> ParseNumber() {
		const size_t start = pos_;
		if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
		while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
		if (pos_ == start) return std::nullopt;
		const std::string token(s_.substr(start, pos_ - start));
		char* end = nullptr;
		const double n = std::strtod(token.c_str(), &end);
		if (end != token.c_str() + token.size()) return std::nullopt;
		return Value::Number(n);
	}

	static void AppendUtf8(std::string& out, unsigned cp) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool Hex4(unsigned& out) {
		if (pos_ + 4 > s_.size()) return false;
		out = 0;
		for (int i = 0; i < 4; ++i) {
			const char h = s_[pos_++];
			out <<= 4;
			if (h >= '0' && h <= '9') out |= h - '0';
			else if (h >= 'a' && h <= 'f') out |= h - 'a' + 10;
			else if (h >= 'A' && h <= 'F') out |= h - 'A' + 10;
			else return false;
		}
		return true;
	}

	std::optional<std::string> ParseString() {
		++pos_;  // opening quote
		std::string out;
		while (pos_ < s_.size()) {
			const char c = s_[pos_++];
			if (c == '"') return out;
			if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos_ >= s_.size()) return std::nullopt;
			const char e = s_[pos_++];
			switch (e) {
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'u': {
					unsigned cp = 0;
					if (!Hex4(cp)) return std::nullopt;
					if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
						unsigned low = 0;
						if (!Literal("\\u") || !Hex4(low) || low < 0xDC00 || low > 0xDFFF) return std::nullopt;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					}
					AppendUtf8(out, cp);
					break;
				}
				default: return std::nullopt;
			}
		}
		return std::nullopt;
	}

	std::optional<Value> ParseArray(int depth) {
		++pos_;
		std::vector<Value> items;
		SkipSpace();
		if (pos_ < s_.size() && s_[pos_] == ']') {
			++pos_;
			return Value::Array(std::move(items));
		}
		while (true) {
			auto v = ParseValue(depth + 1);
			if (!v) return std::nullopt;
			items.push_back(std::move(*v));
			SkipSpace();
			if (pos_ >= s_.size()) return std::nullopt;
			if (s_[pos_] == ',') { ++pos_; continue; }
			if (s_[pos_] == ']') { ++pos_; return Value::Array(std::move(items)); }
			return std::nullopt;
		}
	}

	std::optional<Value> ParseObject(int depth) {
		++pos_;
		std::map<std::string, Value> fields;
		SkipSpace();
		if (pos_ < s_.size() && s_[pos_] == '}') {
			++pos_;
			return Value::Object(std::move(fields));
		}
		while (true) {
			SkipSpace();
			if (pos_ >= s_.size() || s_[pos_] != '"') return std::nullopt;
			auto key = ParseString();
			if (!key) return std::nullopt;
			SkipSpace();
			if (pos_ >= s_.size() || s_[pos_] != ':') return std::nullopt;
			++pos_;
			auto v = ParseValue(depth + 1);
			if (!v) return std::nullopt;
			fields[std::move(*key)] = std::move(*v);
			SkipSpace();
			if (pos_ >= s_.size()) return std::nullopt;
			if (s_[pos_] == ',') { ++pos_; continue; }
			if (s_[pos_] == '}') { ++pos_; return Value::Object(std::move(fields)); }
			return std::nullopt;
		}
	}
};

}  // namespace

Value Value::Bool(bool b) {
	Value v;
	v.kind_ = Kind::Bool;
	v.bool_ = b;
	return v;
}

Value Value::Number(double n) {
	Value v;
	v.kind_ = Kind::Number;
	v.number_ = n;
	return v;
}

Value Value::String(std::string s) {
	Value v;
	v.kind_ = Kind::String;
	v.string_ = std::move(s);
	return v;
}

Value Value::Array(std::vector<Value> items) {
	Value v;
	v.kind_ = Kind::Array;
	v.array_ = std::make_shared<std::vector<Value>>(std::move(items));
	return v;
}

Value Value::Object(std::map<std::string, Value> fields) {
	Value v;
	v.kind_ = Kind::Object;
	v.object_ = std::make_shared<std::map<std::string, Value, std::less<>>>(fields.begin(), fields.end());
	return v;
}

bool Value::asBool(bool fallback) const { return kind_ == Kind::Bool ? bool_ : fallback; }
double Value::asNumber(double fallback) const { return kind_ == Kind::Number ? number_ : fallback; }
int Value::asInt(int fallback) const { return kind_ == Kind::Number ? static_cast<int>(number_) : fallback; }
std::string Value::asString(std::string fallback) const { return kind_ == Kind::String ? string_ : fallback; }

const Value& Value::operator[](std::string_view key) const {
	if (kind_ != Kind::Object) return kNull;
	auto it = object_->find(key);
	return it == object_->end() ? kNull : it->second;
}

const Value& Value::operator[](size_t index) const {
	if (kind_ != Kind::Array || index >= array_->size()) return kNull;
	return (*array_)[index];
}

size_t Value::size() const { return kind_ == Kind::Array ? array_->size() : 0; }

bool Value::has(std::string_view key) const { return kind_ == Kind::Object && object_->find(key) != object_->end(); }

std::optional<Value> Parse(std::string_view text) { return Parser(text).Document(); }

std::string Quote(std::string_view text) {
	std::string out = "\"";
	for (const char c : text) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += c;
				}
		}
	}
	out += '"';
	return out;
}

}  // namespace rooms::json
