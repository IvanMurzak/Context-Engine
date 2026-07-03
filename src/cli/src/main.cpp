// context — the CLI entry point. Thin: adapt argc/argv and defer to context::cli::main_cli, which
// parses the R-CLI-007 grammar, dispatches against the single registry, prints the R-CLI-008
// envelope, and returns the exit code.

#include "context/cli/app.h"

int main(int argc, char** argv)
{
    return context::cli::main_cli(argc, argv);
}
