#ifndef ZSH_EXECUTOR_H
#define ZSH_EXECUTOR_H

#include "utils.h"
#include "zsh_parser.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <ostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <stdexcept>

class ZshExecutor {
private:
    // Helper to convert string vector to char* array for execvp
    class ArgVector {
    public:
        std::vector<char*> args;

        ArgVector(const std::string& cmd, const std::vector<std::string>& arguments) {
            // Add command as first argument
            args.push_back(const_cast<char*>(cmd.c_str()));

            // Add other arguments
            for (const auto& arg : arguments) {
                args.push_back(const_cast<char*>(arg.c_str()));
            }

            // Null terminate the array
            args.push_back(nullptr);
        }

				std::vector<char*>& vector() { return args; }

        char** data() {
            return args.data();
        }
    };

    // Handle redirections for a command
    void handleRedirections(const std::vector<std::pair<std::string, std::string>>& redirections) {
        for (const auto& redir : redirections) {
            int fd;
            if (redir.first == ">") {
                // Truncate and write
                fd = open(redir.second.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd == -1) throw std::runtime_error("Failed to open output file");
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            else if (redir.first == ">>") {
                // Append
                fd = open(redir.second.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd == -1) throw std::runtime_error("Failed to open output file");
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            else if (redir.first == "<") {
                // Input redirection
                fd = open(redir.second.c_str(), O_RDONLY);
                if (fd == -1) throw std::runtime_error("Failed to open input file");
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
            // Add more redirection types as needed
        }
    }

    // Execute a single command
    pid_t executeCommand(const std::unique_ptr<CommandNode>& cmd, int input_fd = STDIN_FILENO, int output_fd = STDOUT_FILENO) {
        pid_t pid = fork();

        if (pid == -1) {
            throw std::runtime_error("Fork failed");
        }

        if (pid == 0) {  // Child process
            // Set up input/output pipes if provided
            if (input_fd != STDIN_FILENO) {
                dup2(input_fd, STDIN_FILENO);
                close(input_fd);
            }
            if (output_fd != STDOUT_FILENO) {
                dup2(output_fd, STDOUT_FILENO);
                close(output_fd);
            }

            // Handle redirections
            handleRedirections(cmd->redirections);

            // Prepare arguments
            ArgVector argv(cmd->command, cmd->arguments);

            // Execute command
            execvp(argv.data()[0], argv.data());

            // If execvp fails
            perror("execvp");
            exit(EXIT_FAILURE);
        }

        // Parent process
        return pid;
    }

    // Execute a pipeline of commands
    std::vector<pid_t> executePipeline(const PipelineNode* pipeline) {
        std::vector<pid_t> pids;
        int input_fd = STDIN_FILENO;
        int pipefd[2];

        for (size_t i = 0; i < pipeline->commands.size(); ++i) {
            // Last command in pipeline
            if (i == pipeline->commands.size() - 1) {
                pids.push_back(executeCommand(pipeline->commands[i], input_fd));
                break;
            }

            // Create pipe
            if (pipe(pipefd) == -1) {
                throw std::runtime_error("Pipe creation failed");
            }

            // Execute command with current input and pipe output
            pids.push_back(executeCommand(pipeline->commands[i], input_fd, pipefd[1]));

            // Close write end of pipe
            close(pipefd[1]);

            // Next command will read from this pipe
            input_fd = pipefd[0];
        }

        return pids;
    }

    // Execute a compound command (sequence, AND/OR)
    void executeCompoundCommand(const CompoundCommandNode* compound) {
        for (const auto& cmd : compound->commands) {
            // Dynamic dispatch based on node type
            if (auto pipeline = dynamic_cast<PipelineNode*>(cmd.get())) {
                std::vector<pid_t> pids = executePipeline(pipeline);
                
                // Wait for all processes in pipeline
                for (pid_t pid : pids) {
                    int status;
                    waitpid(pid, &status, 0);
                }
            }
            // Add more node type handling as needed
        }
    }

    // Execute a subshell
    pid_t executeSubshell(const SubshellNode* subshell) {
        pid_t pid = fork();

        if (pid == -1) {
            throw std::runtime_error("Fork failed for subshell");
        }

        if (pid == 0) {  // Child process
            // Execute the command inside the subshell
            if (auto compound = static_cast<CompoundCommandNode*>(subshell->command.get())) {
                executeCompoundCommand(compound);
            }
            // Add more node type handling as needed
            
            exit(EXIT_SUCCESS);
        }

        // Parent process
        return pid;
    }

public:
    // Main execution method
    void execute(const std::unique_ptr<ASTNode>& ast) {

        // Dynamic dispatch based on AST node type
				if (auto pipeline = dynamic_cast<PipelineNode*>(ast.get())) {
            std::vector<pid_t> pids = executePipeline(pipeline);
            
            // Wait for all processes in pipeline
            for (pid_t pid : pids) {
                int status;
                waitpid(pid, &status, 0);
            }
        } else if (auto compound = dynamic_cast<CompoundCommandNode*>(ast.get()))  {
            executeCompoundCommand(compound);
        } else if (auto subshell = dynamic_cast<SubshellNode*>(ast.get())) {
            pid_t pid = executeSubshell(subshell);
            
            // Wait for subshell
            int status;
            waitpid(pid, &status, 0);
        } else {
            throw std::runtime_error("Unsupported AST node type for execution");
        }
    }
};

// Convenience function to execute a Zsh command
void executeZshCommand(std::string& input, const alias_container& aliases) {
    // Lexer and Parser steps
    ZshLexer lexer(input, aliases);
    std::vector<Token> tokens = lexer.tokenize();

		// for(auto t:tokens)
		// 		printf("t: %s - %s\n", t.type_as_string().c_str(), t.value.c_str());
    
    ZshParser parser(tokens);
    std::unique_ptr<ASTNode> ast = parser.parse();

    // Executor
    ZshExecutor executor;
    executor.execute(ast);
}

#endif // ZSH_EXECUTOR_H
