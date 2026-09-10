#pragma once

#include <set>
#include <stdexcept>

namespace live
{
using namespace juce;
namespace te = tracktion;

inline var object (std::initializer_list<std::pair<Identifier, var>> fields)
{
    auto* result = new DynamicObject();
    for (const auto& [key, value] : fields)
        result->setProperty (key, value);
    return result;
}

inline String text (const var& value) { return JSON::toString (value, true); }

inline void require (bool condition, const String& error)
{
    if (! condition) throw std::runtime_error (error.toStdString());
}

inline double number (const var& v, const Identifier& key, double low, double high, bool integer = false)
{
    const auto n = v[key];
    require (n.isInt() || n.isInt64() || n.isDouble(), key.toString() + " must be numeric");
    const auto d = static_cast<double> (n);
    require (std::isfinite (d) && d >= low && d <= high && (! integer || std::floor (d) == d),
             key.toString() + " out of range");
    return d;
}

inline String id (const var& v)
{
    require (v["id"].isString(), "id must be a string");
    const auto s = v["id"].toString();
    require (s.isNotEmpty() && s.length() <= 100, "invalid id");
    return s;
}

inline void knownFields (const var& value, const String& allowed)
{
    require (value.isObject(), "Expected a JSON object");
    const auto names = StringArray::fromTokens (allowed, " ", "");
    for (const auto& property : value.getDynamicObject()->getProperties())
        require (names.contains (property.name.toString()), "Unknown field: " + property.name.toString());
}

inline String stableID (ValueTree state)
{
    if (! state.hasProperty ("coComposeId"))
        state.setProperty ("coComposeId", Uuid().toString(), nullptr);
    return state["coComposeId"].toString();
}

inline void atomicWrite (const File& file, const String& contents)
{
    require (file.getParentDirectory().createDirectory().wasOk(), "Cannot create project folder");
    TemporaryFile temporary (file);
    require (temporary.getFile().replaceWithText (contents), "Cannot write " + file.getFileName());
    require (temporary.overwriteTargetFileWithTemporary(), "Cannot replace " + file.getFileName());
}
}
