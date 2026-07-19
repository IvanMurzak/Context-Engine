// The build-time client-schema generator: writes the projected client schema to the path given as
// argv[1]. Driven by a CMake custom command so the artifact is produced by the BUILD (never by hand)
// and installed alongside context_client for downstream generators (e05's JS client).

#include "context/editor/client/schema.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: context_client_schema_gen <output-path>\n";
        return 2;
    }
    const std::string path = argv[1];
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::cerr << "could not open '" << path << "' for writing\n";
        return 1;
    }
    out << context::editor::client::client_schema_text();
    if (!out)
    {
        std::cerr << "could not write '" << path << "'\n";
        return 1;
    }
    return 0;
}
