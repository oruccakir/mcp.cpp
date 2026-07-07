// Integration-test server exercising the full Phase 2 surface over stdio:
// a tool, a static resource, a resource template, a prompt with completion,
// and logging.

#include <cstdio>
#include <memory>

#include <mcp/mcp.hpp>

int main() {
    std::fprintf(stderr, "toolbox-stdio started\n");

    mcp::Server server("toolbox-stdio", "0.1.0");
    server.set_instructions("Integration test server");

    mcp::ToolSpec echo;
    echo.description = "Echoes back the input";
    echo.input_schema =
        mcp::json{{"type", "object"},
                  {"properties", {{"message", {{"type", "string"}}}}},
                  {"required", mcp::json::array({"message"})}};
    echo.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        return {{mcp::text_content(args.at("message").get<std::string>())}};
    };
    server.register_tool("echo", std::move(echo));

    mcp::Resource readme;
    readme.uri = "mem://readme";
    readme.name = "Readme";
    readme.mime_type = "text/plain";
    server.resources().add_resource(
        readme, [](const std::string& uri) -> mcp::Result<mcp::ReadResourceResult> {
            mcp::ResourceContents contents;
            contents.uri = uri;
            contents.mime_type = "text/plain";
            contents.text = "toolbox readme";
            return mcp::ReadResourceResult{{contents}};
        });

    mcp::ResourceTemplate files;
    files.uri_template = "mem://files/{name}";
    files.name = "Files";
    server.resources().add_resource_template(files);
    server.resources().set_read_handler(
        [](const std::string& uri) -> mcp::Result<mcp::ReadResourceResult> {
            mcp::ResourceContents contents;
            contents.uri = uri;
            contents.text = "file body";
            return mcp::ReadResourceResult{{contents}};
        });

    mcp::Prompt greet;
    greet.name = "greet";
    mcp::PromptArgument who;
    who.name = "who";
    who.required = true;
    greet.arguments = std::vector<mcp::PromptArgument>{who};
    server.prompts().add_prompt(
        greet, [](const mcp::json& args) -> mcp::Result<mcp::GetPromptResult> {
            mcp::GetPromptResult result;
            result.messages.push_back(mcp::PromptMessage{
                mcp::Role::User,
                mcp::text_content("Say hello to " +
                                  args.at("who").get<std::string>())});
            return result;
        });
    server.prompts().set_completion("greet", "who",
                                    [](const std::string&) {
                                        mcp::CompleteResult r;
                                        r.values = {"world"};
                                        return r;
                                    });

    return server.run(std::make_shared<mcp::StdioTransport>());
}
