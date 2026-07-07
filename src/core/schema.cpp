#include <mcp/detail/schema.hpp>

#include <string>

namespace mcp::detail {

namespace {

bool matches_type(const json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "boolean") return value.is_boolean();
    if (type == "null") return value.is_null();
    return true;  // Unknown type names are not enforced.
}

Error invalid(const std::string& path, const std::string& what) {
    const std::string at = path.empty() ? "value" : path;
    return Error(ErrorCode::InvalidParams, at + " " + what);
}

Result<void> validate_at(const json& value, const json& schema,
                         const std::string& path) {
    if (!schema.is_object()) {
        return Result<void>::ok();  // true/empty schemas accept everything
    }

    if (auto it = schema.find("type"); it != schema.end()) {
        bool ok = false;
        if (it->is_string()) {
            ok = matches_type(value, it->get<std::string>());
        } else if (it->is_array()) {
            for (const auto& t : *it) {
                if (t.is_string() && matches_type(value, t.get<std::string>())) {
                    ok = true;
                    break;
                }
            }
        } else {
            ok = true;
        }
        if (!ok) {
            return invalid(path, "has wrong type (expected " + it->dump() + ")");
        }
    }

    if (auto it = schema.find("enum"); it != schema.end() && it->is_array()) {
        bool found = false;
        for (const auto& candidate : *it) {
            if (candidate == value) {
                found = true;
                break;
            }
        }
        if (!found) {
            return invalid(path, "is not one of the allowed values");
        }
    }

    if (value.is_object()) {
        const auto props_it = schema.find("properties");
        const bool has_props = props_it != schema.end() && props_it->is_object();

        if (auto req = schema.find("required");
            req != schema.end() && req->is_array()) {
            for (const auto& name : *req) {
                if (name.is_string() && !value.contains(name.get<std::string>())) {
                    return invalid(path, "is missing required property '" +
                                             name.get<std::string>() + "'");
                }
            }
        }

        if (has_props) {
            for (auto it = props_it->begin(); it != props_it->end(); ++it) {
                if (value.contains(it.key())) {
                    auto nested = validate_at(value.at(it.key()), it.value(),
                                              path + "/" + it.key());
                    if (!nested) {
                        return nested;
                    }
                }
            }
        }

        if (auto ap = schema.find("additionalProperties");
            ap != schema.end() && ap->is_boolean() && !ap->get<bool>()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!has_props || !props_it->contains(it.key())) {
                    return invalid(path,
                                   "has unexpected property '" + it.key() + "'");
                }
            }
        }
    }

    if (value.is_array()) {
        if (auto items = schema.find("items");
            items != schema.end() && items->is_object()) {
            std::size_t index = 0;
            for (const auto& element : value) {
                auto nested = validate_at(element, *items,
                                          path + "/" + std::to_string(index));
                if (!nested) {
                    return nested;
                }
                ++index;
            }
        }
    }

    return Result<void>::ok();
}

}  // namespace

Result<void> validate_schema(const json& value, const json& schema) {
    return validate_at(value, schema, "");
}

}  // namespace mcp::detail
