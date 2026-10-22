// A small JSON reader and string escaper for the board's answers. Only what
// the plugin needs: parse a document into a tree, look values up, quote text.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rooms::json {

class Value {
public:
	enum class Kind { Null, Bool, Number, String, Array, Object };

	Value() = default;
	static Value Bool(bool b);
	static Value Number(double n);
	static Value String(std::string s);
	static Value Array(std::vector<Value> items);
	static Value Object(std::map<std::string, Value> fields);

	Kind kind() const { return kind_; }
	bool isNull() const { return kind_ == Kind::Null; }

	// Typed reads with a fallback for a missing or differently typed value.
	bool asBool(bool fallback = false) const;
	double asNumber(double fallback = 0) const;
	int asInt(int fallback = 0) const;
	std::string asString(std::string fallback = {}) const;

	// Object member or array element; a null Value when absent.
	const Value& operator[](std::string_view key) const;
	const Value& operator[](size_t index) const;
	size_t size() const;  // array length, 0 for anything else
	bool has(std::string_view key) const;

private:
	Kind kind_ = Kind::Null;
	bool bool_ = false;
	double number_ = 0;
	std::string string_;
	std::shared_ptr<std::vector<Value>> array_;
	std::shared_ptr<std::map<std::string, Value, std::less<>>> object_;
};

// nullopt on malformed input (or trailing garbage, or nesting deeper than 64).
std::optional<Value> Parse(std::string_view text);

// `text` as a JSON string literal, quotes included.
std::string Quote(std::string_view text);

}  // namespace rooms::json
