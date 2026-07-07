// SRS §6.2: calculator server with a small built-in expression evaluator
// (numbers, + - * /, parentheses, unary minus).

#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

#include <mcp/mcp.hpp>

namespace {

/// Recursive-descent evaluator for basic arithmetic.
class Evaluator {
public:
    explicit Evaluator(const std::string& text) : text_(text) {}

    double evaluate() {
        const double value = expression();
        skip_spaces();
        if (pos_ != text_.size()) {
            throw std::runtime_error("unexpected character at position " +
                                     std::to_string(pos_));
        }
        return value;
    }

private:
    double expression() {
        double value = term();
        for (;;) {
            skip_spaces();
            if (consume('+')) {
                value += term();
            } else if (consume('-')) {
                value -= term();
            } else {
                return value;
            }
        }
    }

    double term() {
        double value = factor();
        for (;;) {
            skip_spaces();
            if (consume('*')) {
                value *= factor();
            } else if (consume('/')) {
                const double divisor = factor();
                if (divisor == 0.0) {
                    throw std::runtime_error("division by zero");
                }
                value /= divisor;
            } else {
                return value;
            }
        }
    }

    double factor() {
        skip_spaces();
        if (consume('(')) {
            const double value = expression();
            skip_spaces();
            if (!consume(')')) {
                throw std::runtime_error("missing closing parenthesis");
            }
            return value;
        }
        if (consume('-')) {
            return -factor();
        }
        return number();
    }

    double number() {
        skip_spaces();
        const std::size_t start = pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
                text_[pos_] == '.')) {
            ++pos_;
        }
        if (start == pos_) {
            throw std::runtime_error("expected a number at position " +
                                     std::to_string(start));
        }
        return std::stod(text_.substr(start, pos_ - start));
    }

    void skip_spaces() {
        while (pos_ < text_.size() && text_[pos_] == ' ') {
            ++pos_;
        }
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

}  // namespace

int main() {
    mcp::Server server("calculator-server", "1.0.0");

    mcp::ToolSpec calculate;
    calculate.description = "Evaluate a mathematical expression";
    calculate.input_schema =
        mcp::json{{"type", "object"},
                  {"properties", {{"expression", {{"type", "string"}}}}},
                  {"required", mcp::json::array({"expression"})}};
    calculate.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        try {
            Evaluator evaluator(args.at("expression").get<std::string>());
            return {{mcp::text_content(std::to_string(evaluator.evaluate()))}};
        } catch (const std::exception& e) {
            return {{mcp::text_content(std::string("Evaluation failed: ") +
                                       e.what())},
                    true};
        }
    };
    server.register_tool("calculate", std::move(calculate));

    return server.run(std::make_shared<mcp::StdioTransport>());
}
