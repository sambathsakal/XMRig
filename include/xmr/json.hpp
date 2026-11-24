#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xmr { namespace json {

// A deliberately small JSON DOM. Numbers keep their original text so 64-bit
// job heights and difficulty values never go through a double.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    std::string text;                              // string value, or raw number
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    bool isNull()   const { return type == Type::Null; }
    bool isBool()   const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    const Value *find(const std::string &key) const;

    std::string stringValue(const std::string &fallback = {}) const;
    bool boolValue(bool fallback = false) const;
    std::int64_t intValue(std::int64_t fallback = 0) const;
    std::uint64_t uintValue(std::uint64_t fallback = 0) const;
    double doubleValue(double fallback = 0.0) const;

    static Value null();
    static Value booleanValue(bool v);
    static Value number(std::int64_t v);
    static Value number(std::uint64_t v);
    static Value string(std::string v);
    static Value array();
    static Value object();

    void add(Value v) { items.emplace_back(std::move(v)); type = Type::Array; }
    void set(std::string key, Value v);
};

Value parse(const std::string &input, std::string &error);
std::string dump(const Value &value, int indent = 0);

}} // namespace xmr::json
