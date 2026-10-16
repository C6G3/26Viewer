#include "McpServer.hpp"
#include "viewer/core/Json.hpp"
#include "viewer/core/RandomAccessFile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>

using namespace viewer;
namespace fs = std::filesystem;

TEST_CASE("JSON: read and written back")
{
    auto v = json::parse(R"( {"a": [1, 2.5, -3e2, true, false, null], "b": "x\"y\\z\né🚗", "c": {}} )");
    REQUIRE(v);
    CHECK(v->isObject());
    CHECK((*v)["a"].asArray().size() == 6);
    CHECK((*v)["a"].asArray()[1].asNumber() == 2.5);
    CHECK((*v)["a"].asArray()[2].asNumber() == -300);
    CHECK((*v)["a"].asArray()[5].isNull());
    CHECK((*v)["b"].asString() == "x\"y\\z\n\xC3\xA9\xF0\x9F\x9A\x97");
    CHECK((*v)["missing"].isNull());
    CHECK(json::write(*v) == R"({"a":[1,2.5,-300,true,false,null],"b":"x\"y\\z\n)"
                             "\xC3\xA9\xF0\x9F\x9A\x97"
                             R"(","c":{}})");
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "\"open", "01x", "[1] 2", "{\"a\":\"\x01\"}"}) {
        INFO(bad);
        CHECK_FALSE(json::parse(bad));
    }
    json::Value o;
    o.set("id", 7);
    o.set("id", 8); // replaced, not added
    CHECK(json::write(o) == R"({"id":8})");
}

namespace {

/// A folder with a text file and a nested one, removed afterwards.
struct Folder {
    fs::path path = fs::temp_directory_path() / "26viewer_test_mcp";
    Folder()
    {
        fs::remove_all(path);
        fs::create_directories(path / "data" / "deep");
        (void)writeFile(path / "data" / "notes.txt", asBytes(std::string_view("first line\nthe Viper ACR\nlast\n")));
        (void)writeFile(path / "data" / "deep" / "more.ini", asBytes(std::string_view("[car]\nname=Viper\n")));
    }
    ~Folder() { fs::remove_all(path); }
};

json::Value call(mcp::Server& server, int id, std::string_view method, json::Value params = {})
{
    json::Value request;
    request.set("jsonrpc", "2.0");
    request.set("id", id);
    request.set("method", method);
    if (!params.isNull())
        request.set("params", std::move(params));
    const auto reply = server.handle(json::write(request));
    REQUIRE(reply);
    CHECK(reply->find('\n') == std::string::npos); // one message per line
    auto parsed = json::parse(*reply);
    REQUIRE(parsed);
    CHECK((*parsed)["id"].asNumber() == id);
    return *parsed;
}

/// A tool's text, and whether it is an error.
std::pair<std::string, bool> tool(mcp::Server& server, std::string_view name, json::Value arguments = json::Object{})
{
    json::Value params;
    params.set("name", name);
    params.set("arguments", std::move(arguments));
    const json::Value r = call(server, 9, "tools/call", std::move(params));
    const json::Value& result = r["result"];
    return {result["content"].asArray().at(0)["text"].asString(), result["isError"].asBool()};
}

} // namespace

TEST_CASE("MCP server: handshake, tools and reading files")
{
    Folder folder;
    mcp::Server server({folder.path, false, "1.2.3"});

    json::Value init;
    init.set("protocolVersion", "2025-03-26");
    const json::Value hello = call(server, 1, "initialize", init);
    CHECK(hello["result"]["protocolVersion"].asString() == "2025-03-26");
    CHECK(hello["result"]["serverInfo"]["version"].asString() == "1.2.3");
    CHECK(hello["result"]["capabilities"]["tools"].isObject());
    init.set("protocolVersion", "1999-01-01"); // unknown: the newest one offered
    CHECK(call(server, 1, "initialize", init)["result"]["protocolVersion"].asString() == "2025-06-18");
    CHECK_FALSE(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    CHECK(call(server, 2, "ping")["result"].isObject());
    CHECK(call(server, 3, "no/such")["error"]["code"].asNumber() == -32601);
    CHECK(json::parse(*server.handle("{oops")).value()["error"]["code"].asNumber() == -32700);

    // Read-only: no tool that writes is offered.
    std::vector<std::string> names;
    for (const json::Value& t : call(server, 4, "tools/list")["result"]["tools"].asArray()) {
        names.push_back(t["name"].asString());
        CHECK(t["inputSchema"]["type"].asString() == "object");
        CHECK(t["annotations"]["readOnlyHint"].asBool());
    }
    CHECK(std::ranges::find(names, "read_file") != names.end());
    CHECK(std::ranges::find(names, "edit_text") == names.end());

    auto [listing, listError] = tool(server, "list_files", json::Object{{"filter", ".txt"}});
    CHECK_FALSE(listError);
    CHECK(listing.find("data/notes.txt") != std::string::npos);
    CHECK(listing.find("more.ini") == std::string::npos);

    auto [text, readError] = tool(server, "read_file", json::Object{{"path", "notes.txt"}, {"start_line", 2}});
    CHECK_FALSE(readError);
    CHECK(text.find("     2| the Viper ACR") != std::string::npos);
    CHECK(text.find("first line") == std::string::npos);

    auto [found, searchError] = tool(server, "search_text", json::Object{{"query", "viper"}});
    CHECK_FALSE(searchError);
    CHECK(found.find("data/notes.txt:2: the Viper ACR") != std::string::npos);
    CHECK(found.find("data/deep/more.ini:2: name=Viper") != std::string::npos);

    auto [missing, missingError] = tool(server, "read_file", json::Object{{"path", "nothing.txt"}});
    CHECK(missingError);
    CHECK(missing.find("no file") != std::string::npos);
    CHECK(call(server, 5, "tools/call", json::Object{{"name", "edit_text"}})["error"]["code"].asNumber() == -32602);
}

TEST_CASE("MCP server: edits need --allow-edits and a backup")
{
    Folder folder;
    mcp::Server server({folder.path, true, "1"});
    const json::Value edit{json::Object{{"path", "data/notes.txt"}, {"find", "Viper ACR"}, {"replace", "My Car"}}};

    auto [refused, refusedError] = tool(server, "edit_text", edit);
    CHECK(refusedError);
    CHECK(refused.find("no backup") != std::string::npos);
    CHECK(readFile(folder.path / "data" / "notes.txt").value().size() == 30); // untouched

    auto [backedUp, backupError] = tool(server, "backup", json::Object{{"path", "data/notes.txt"}});
    CHECK_FALSE(backupError);
    CHECK(fs::exists(findBackup(folder.path / "data" / "notes.txt", folder.path)));

    auto [done, doneError] = tool(server, "edit_text", edit);
    INFO(done);
    CHECK_FALSE(doneError);
    CHECK(done.find("line 2") != std::string::npos);
    const ByteBuffer now = readFile(folder.path / "data" / "notes.txt").value();
    CHECK(std::string(reinterpret_cast<const char*>(now.data()), now.size()) == "first line\nthe My Car\nlast\n");

    // Text that occurs twice must be pinned down (or `all`).
    auto [twice, twiceError] =
        tool(server, "edit_text", json::Object{{"path", "data/notes.txt"}, {"find", "l"}, {"replace", "L"}});
    CHECK(twiceError);
    CHECK(twice.find("places") != std::string::npos);
}
