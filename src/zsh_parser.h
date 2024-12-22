#ifndef ZSH_PARSER_H
#define ZSH_PARSER_H

#include "zsh_lexer.backup.h"
#include <memory>
#include <stdexcept>
#include <iostream>

// Abstract base class for AST nodes
class ASTNode {
public:
    virtual ~ASTNode() = default;
    virtual void print(int indent = 0) const = 0;
};

// Represents a simple command with arguments
class CommandNode : public ASTNode {
public:
    std::string command;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> redirections;
    bool background = false;

    void print(int indent = 0) const override {
        std::string padding(indent, ' ');
        std::cout << padding << "Command: " << command << std::endl;
        
        if (!arguments.empty()) {
            std::cout << padding << "  Arguments:" << std::endl;
            for (const auto& arg : arguments) {
                std::cout << padding << "    - " << arg << std::endl;
            }
        }
        
        if (!redirections.empty()) {
            std::cout << padding << "  Redirections:" << std::endl;
            for (const auto& redir : redirections) {
                std::cout << padding << "    - " 
                          << redir.first << " " << redir.second << std::endl;
            }
        }
        
        if (background) {
            std::cout << padding << "  Background: true" << std::endl;
        }
    }
};

// Represents a pipeline of commands
class PipelineNode : public ASTNode {
public:
    std::vector<std::unique_ptr<CommandNode>> commands;
    bool background = false;

    void print(int indent = 0) const override {
        std::string padding(indent, ' ');
        std::cout << padding << "Pipeline:" << std::endl;
        
        for (const auto& cmd : commands) {
            cmd->print(indent + 2);
        }
        
        if (background) {
            std::cout << padding << "  Background: true" << std::endl;
        }
    }
};

// Represents a compound command (sequence, AND/OR logic)
class CompoundCommandNode : public ASTNode {
public:
    enum class Type {
        SEQUENCE,   // commands separated by ;
        AND,        // && 
        OR          // ||
    };

    Type type;
    std::vector<std::unique_ptr<ASTNode>> commands;

    void print(int indent = 0) const override {
        std::string padding(indent, ' ');
        std::string typeStr;
        switch (type) {
            case Type::SEQUENCE: typeStr = "Sequence"; break;
            case Type::AND: typeStr = "AND"; break;
            case Type::OR: typeStr = "OR"; break;
        }
        
        std::cout << padding << "Compound Command (" << typeStr << "):" << std::endl;
        
        for (const auto& cmd : commands) {
            cmd->print(indent + 2);
        }
    }
};

// Represents subshell execution
class SubshellNode : public ASTNode {
public:
    std::unique_ptr<ASTNode> command;
    bool background = false;

    void print(int indent = 0) const override {
        std::string padding(indent, ' ');
        std::cout << padding << "Subshell:" << std::endl;
        
        if (command) {
            command->print(indent + 2);
        }
        
        if (background) {
            std::cout << padding << "  Background: true" << std::endl;
        }
    }
};

class ZshParser {
private:
    const std::vector<Token> &tokens;
    size_t current = 0;

    // Utility parsing methods
    bool isAtEnd() const {
        return current >= tokens.size() || 
               tokens[current].type == TokenType::EOL;
    }

    Token peek() const {
        return isAtEnd() ? Token{TokenType::EOL, "", 0, 0} : tokens[current];
    }

    Token previous() const {
        return current > 0 ? tokens[current - 1] : Token{TokenType::EOL, "", 0, 0};
    }

    Token consume(TokenType type, const std::string& message) {
        if (peek().type == type) {
            advance();
            return previous();
        }
        throw std::runtime_error(message);
    }

    void advance() {
        if (!isAtEnd()) current++;
    }

    bool match(TokenType type) {
        if (peek().type == type) {
            advance();
            return true;
        }
        return false;
    }

    // Core parsing methods
    std::unique_ptr<CommandNode> parseCommand() {
        auto cmd = std::make_unique<CommandNode>();
        
        // First token is the command
        if (peek().type == TokenType::WORD) {
            cmd->command = peek().value;
            advance();
        }

        // Parse arguments and redirections
        while (!isAtEnd()) {
            if (peek().type == TokenType::WORD) {
                cmd->arguments.push_back(peek().value);
                advance();
            }
            else if (peek().type == TokenType::REDIRECTION) {
                // Handle redirections like >, <, >>, etc.
                std::string redir = peek().value;
                advance();
                
                if (peek().type == TokenType::WORD) {
                    cmd->redirections.push_back({redir, peek().value});
                    advance();
                }
                else {
                    throw std::runtime_error("Expected filename after redirection");
                }
            }
            else {
                break;
            }
        }

        // Check for background execution
        if (match(TokenType::BACKGROUND)) {
            cmd->background = true;
        }

        return cmd;
    }

    std::unique_ptr<PipelineNode> parsePipeline() {
        auto pipeline = std::make_unique<PipelineNode>();
        
        // Parse first command
        pipeline->commands.push_back(parseCommand());

        // Parse additional commands in pipeline
        while (match(TokenType::PIPE)) {
            pipeline->commands.push_back(parseCommand());
        }

        // Check for background execution
        if (match(TokenType::BACKGROUND)) {
            pipeline->background = true;
        }

        return pipeline;
    }

    std::unique_ptr<CompoundCommandNode> parseCompoundCommand() {
        auto compound = std::make_unique<CompoundCommandNode>();
        
        // Default to sequence
        compound->type = CompoundCommandNode::Type::SEQUENCE;

        compound->commands.push_back(parsePipeline());

        // Parse additional commands with different types
        while (!isAtEnd()) {
            if (match(TokenType::SEMICOLON)) {
                compound->type = CompoundCommandNode::Type::SEQUENCE;
                compound->commands.push_back(parsePipeline());
            }
            else {
                break;
            }
        }

        return compound;
    }

    std::unique_ptr<SubshellNode> parseSubshell() {
        auto subshell = std::make_unique<SubshellNode>();
        
        // Consume opening parenthesis
        consume(TokenType::SUBSHELL_START, "Expected '(' to start subshell");

        // Parse command inside subshell
        subshell->command = parseCompoundCommand();

        // Consume closing parenthesis
        consume(TokenType::SUBSHELL_END, "Expected ')' to end subshell");

        // Check for background execution
        if (match(TokenType::BACKGROUND)) {
            subshell->background = true;
        }

        return subshell;
    }

public:
    ZshParser(const std::vector<Token>& tokens) : tokens(tokens) {}

    std::unique_ptr<ASTNode> parse() {
        // Reset current position
        current = 0;

        // Start parsing
        if (peek().type == TokenType::SUBSHELL_START) {
            return parseSubshell();
        }
        
        return parseCompoundCommand();
    }
};

#endif // ZSH_PARSER_H
