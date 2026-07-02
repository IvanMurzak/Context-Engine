// context-hello: the M0 build-system smoke executable. Prints the engine name + version.

#include <cstdio>

int main()
{
    std::printf("%s v%s\n", CONTEXT_ENGINE_NAME, CONTEXT_ENGINE_VERSION);
    return 0;
}
