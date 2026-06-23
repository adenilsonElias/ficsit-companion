#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include "domain/core/json.hpp"

#include <array>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("JSON values report their active type", "[json]")
{
    const Json::Value null;
    const Json::Value object(Json::Object{ { "key", 1 } });
    const Json::Value array(Json::Array{ 1, 2 });
    const Json::Value string("text");
    const Json::Value boolean(true);
    const Json::Value signed_integer(-7);
    const Json::Value unsigned_integer(7u);
    const Json::Value number(2.5);

    REQUIRE(null.is_null());
    REQUIRE_FALSE(null.is_string());
    REQUIRE_FALSE(null.is_object());
    REQUIRE_FALSE(null.is_array());
    REQUIRE_FALSE(null.is_bool());
    REQUIRE_FALSE(null.is_integer());
    REQUIRE_FALSE(null.is_number());

    REQUIRE(object.is_object());
    REQUIRE(object.is<Json::Object>());
    REQUIRE_FALSE(object.is_array());

    REQUIRE(array.is_array());
    REQUIRE(array.is<Json::Array>());
    REQUIRE_FALSE(array.is_object());

    REQUIRE(string.is_string());
    REQUIRE(string.is<std::string>());
    REQUIRE_FALSE(string.is_bool());

    REQUIRE(boolean.is_bool());
    REQUIRE(boolean.is<bool>());
    REQUIRE_FALSE(boolean.is_number());

    REQUIRE(signed_integer.is_integer());
    REQUIRE(signed_integer.is_number());
    REQUIRE(signed_integer.is<long long int>());

    REQUIRE(unsigned_integer.is_integer());
    REQUIRE(unsigned_integer.is_number());
    REQUIRE(unsigned_integer.is<unsigned long long int>());

    REQUIRE(number.is_number());
    REQUIRE_FALSE(number.is_integer());
    REQUIRE(number.is<double>());
}

TEST_CASE("JSON getters expose mutable and const typed values", "[json]")
{
    Json::Value object(Json::Object{ { "count", 3 } });
    Json::Value array(Json::Array{ "first", "second" });
    Json::Value string(std::string("mutable"));
    const Json::Value boolean(false);
    const Json::Value signed_integer(-42);
    const Json::Value unsigned_integer(42u);
    const Json::Value number(12.5);

    object.get_object()["extra"] = true;
    array.get_array().push_back("third");
    string.get_string() += " string";

    REQUIRE(object.get<Json::Object>().size() == 2);
    REQUIRE(object.get_object().at("extra").get<bool>());
    REQUIRE(array.get<Json::Array>().size() == 3);
    REQUIRE(array.get_array().at(2).get_string() == "third");
    REQUIRE(string.get<std::string>() == "mutable string");
    REQUIRE(string.get_string() == "mutable string");
    REQUIRE_FALSE(boolean.get<bool>());
    REQUIRE(signed_integer.get<long long int>() == -42);
    REQUIRE(unsigned_integer.get<unsigned long long int>() == 42);
    REQUIRE(number.get<double>() == 12.5);
    REQUIRE(number.get_number<int>() == 12);
    REQUIRE(signed_integer.get_number<double>() == -42.0);
    REQUIRE(unsigned_integer.get_number<unsigned int>() == 42u);

    const Json::Value& const_object = object;
    const Json::Value& const_array = array;
    const Json::Value& const_string = string;
    REQUIRE(const_object.get_object().size() == 2);
    REQUIRE(const_array.get_array().size() == 3);
    REQUIRE(const_string.get_string() == "mutable string");
}

TEST_CASE("JSON container constructors and initializer lists retain structure", "[json]")
{
    const std::vector<int> vector{ 1, 2, 3 };
    const std::array<std::string, 2> array{ "a", "b" };
    const std::map<std::string, bool> map{
        { "disabled", false },
        { "enabled", true },
    };

    const Json::Value vector_value(vector);
    const Json::Value array_value(array);
    const Json::Value map_value(map);
    const Json::Value object_init{
        { "alpha", 1 },
        { "beta", 2 },
    };
    const Json::Value array_init{ 1, "two", false };
    const Json::Value pair_init{ "single", 9 };

    REQUIRE(vector_value.size() == 3);
    REQUIRE(vector_value[2].get_number<int>() == 3);
    REQUIRE(array_value.size() == 2);
    REQUIRE(array_value[0].get_string() == "a");
    REQUIRE(map_value.size() == 2);
    REQUIRE(map_value["enabled"].get<bool>());
    REQUIRE(object_init.is_object());
    REQUIRE(object_init["beta"].get_number<int>() == 2);
    REQUIRE(array_init.is_array());
    REQUIRE(array_init.size() == 3);
    REQUIRE(pair_init.is_object());
    REQUIRE(pair_init["single"].get_number<int>() == 9);
}

TEST_CASE("JSON indexing contains size and push_back update containers", "[json]")
{
    Json::Value object;
    object["created"] = 4;
    object["nested"]["ok"] = true;

    REQUIRE(object.is_object());
    REQUIRE(object.contains("created"));
    REQUIRE_FALSE(object.contains("missing"));
    REQUIRE(object.size() == 2);
    REQUIRE(object["nested"]["ok"].get<bool>());

    Json::Value array;
    const Json::Value copied("copied");
    array.push_back(copied);
    array.push_back(Json::Value(8));
    array[0] = "changed";

    REQUIRE(array.is_array());
    REQUIRE(array.size() == 2);
    REQUIRE(array[0].get_string() == "changed");
    REQUIRE(array[1].get_number<int>() == 8);

    const Json::Value& const_object = object;
    const Json::Value& const_array = array;
    REQUIRE(const_object["created"].get_number<int>() == 4);
    REQUIRE(const_array[1].get_number<int>() == 8);
    REQUIRE(Json::Value().size() == 0);
}

TEST_CASE("JSON getters and container operations reject wrong types", "[json]")
{
    Json::Value primitive("text");
    const Json::Value const_primitive("text");
    const Json::Value const_object(Json::Object{ { "present", 1 } });
    const Json::Value const_array(Json::Array{ 1 });

    REQUIRE_THROWS_AS(primitive.get_object(), std::runtime_error);
    REQUIRE_THROWS_AS(primitive.get_array(), std::runtime_error);
    REQUIRE_THROWS_AS(primitive.get<bool>(), std::runtime_error);
    REQUIRE_THROWS_AS(primitive.get_number<int>(), std::runtime_error);
    REQUIRE_THROWS_AS(primitive["key"], std::runtime_error);
    REQUIRE_THROWS_AS(primitive[0], std::runtime_error);
    REQUIRE_THROWS_AS(primitive.push_back(1), std::runtime_error);
    REQUIRE_THROWS_AS(primitive.size(), std::runtime_error);

    REQUIRE_THROWS_AS(const_primitive["key"], std::runtime_error);
    REQUIRE_THROWS_AS(const_primitive[0], std::runtime_error);
    REQUIRE_THROWS_AS(const_object["missing"], std::out_of_range);
    REQUIRE_THROWS_AS(const_array[1], std::out_of_range);
}

TEST_CASE("JSON Dump emits compact and indented deterministic text", "[json]")
{
    const Json::Value value(Json::Object{
        { "empty_array", Json::Array{} },
        { "empty_object", Json::Object{} },
        { "items", Json::Array{ 1, true, nullptr } },
        { "whole_double", 2.0 },
    });

    const std::string compact =
        R"({"empty_array":[],"empty_object":{},"items":[1,true,null],"whole_double":2.0})";
    const std::string indented =
        "{\n"
        "..\"empty_array\": [],\n"
        "..\"empty_object\": {},\n"
        "..\"items\": [\n"
        "....1,\n"
        "....true,\n"
        "....null\n"
        "..],\n"
        "..\"whole_double\": 2.0\n"
        "}";

    REQUIRE(value.Dump() == compact);
    REQUIRE(value.Dump(2, '.') == indented);
    REQUIRE(Json::Parse(value.Dump()).Dump() == compact);
    REQUIRE(Json::Parse(value.Dump(4)).Dump() == compact);
}

TEST_CASE("JSON strings dump escapes and parse escaped controls and unicode", "[json]")
{
    const Json::Value escaped(std::string("\"\\\b\f\n\r\t"));
    REQUIRE(escaped.Dump() == R"("\"\\\b\f\n\r\t")");

    const Json::Value parsed = Json::Parse(
        R"("slash:\/ backslash:\\ controls:\b\f\n\r\t unicode:\u0041\u00e9\u20ac")");
    const std::string expected =
        std::string("slash:/ backslash:\\ controls:\b\f\n\r\t unicode:A")
        + "\xC3\xA9\xE2\x82\xAC";

    REQUIRE(parsed.is_string());
    REQUIRE(parsed.get_string() == expected);
}

TEST_CASE("JSON parses escaped quotes and round-trips them losslessly", "[json]")
{
    // Parsing \" must decode to a single double-quote, not preserve the backslash.
    const Json::Value parsed = Json::Parse(R"("say \"hi\"")");
    REQUIRE(parsed.is_string());
    REQUIRE(parsed.get_string() == "say \"hi\"");

    // A value containing a quote must survive a dump -> parse round-trip unchanged
    // (regression: the parser used to keep the escape backslash, growing the
    // string by one '\' on every save/load cycle).
    const Json::Value original(std::string("label with \" quote"));
    const Json::Value round_tripped = Json::Parse(original.Dump());
    REQUIRE(round_tripped.get_string() == original.get_string());
}

TEST_CASE("JSON Parse reads literals numbers arrays and objects", "[json]")
{
    const Json::Value value = Json::Parse(
        R"({"array":[null,true,false,-42,18446744073709551615,1.25,6.02e2],"text":"ok"})");

    REQUIRE(value.is_object());
    REQUIRE(value["text"].get_string() == "ok");
    REQUIRE(value["array"].size() == 7);
    REQUIRE(value["array"][0].is_null());
    REQUIRE(value["array"][1].get<bool>());
    REQUIRE_FALSE(value["array"][2].get<bool>());
    REQUIRE(value["array"][3].get<long long int>() == -42);
    REQUIRE(
        value["array"][4].get<unsigned long long int>()
        == std::numeric_limits<unsigned long long int>::max());
    REQUIRE(value["array"][5].get_number<double>() == 1.25);
    REQUIRE(value["array"][6].get_number<double>() == 602.0);

    REQUIRE(Json::Parse("-9223372036854775808").get<long long int>()
        == std::numeric_limits<long long int>::min());
    REQUIRE(Json::Parse("18446744073709551616").is<double>());
    REQUIRE(Json::Parse("-9223372036854775809").is<double>());
    REQUIRE(Json::Parse("").is_null());
}

TEST_CASE("JSON Parse supports streams and bounded string views", "[json]")
{
    std::istringstream stream(R"({"streamed":[1,2]})");
    Json::Value streamed;
    stream >> streamed;

    REQUIRE(streamed["streamed"].size() == 2);

    const std::string source = "true trailing";
    const std::string_view view(source);
    const Json::Value bounded = Json::Parse(view.begin(), 4);
    REQUIRE(bounded.get<bool>());
    REQUIRE_THROWS_AS(Json::Parse(view.begin(), view.size()), std::runtime_error);
    REQUIRE(Json::Parse(view.begin(), view.size(), true).is_null());
}

TEST_CASE("JSON Parse rejects malformed input or returns null in no-exception mode",
          "[json]")
{
    const std::array<std::string, 15> malformed{
        "null trailing",
        R"("\q")",
        R"("unterminated)",
        R"({"key" 1})",
        R"({"key":1 "next":2})",
        "[1 2]",
        "truX",
        "falsX",
        "nulX",
        "01",
        "1.2.3",
        "1e2e3",
        "-",
        R"("\uD800")",
        std::string("\"line\nbreak\""),
    };

    for (const std::string& input : malformed)
    {
        CAPTURE(input);
        REQUIRE_THROWS(Json::Parse(input));
        REQUIRE(Json::Parse(input, true).is_null());
    }
}
