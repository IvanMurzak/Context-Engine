# context-hello — build-system smoke target: proves configure/build/run on every CI platform.

add_executable(context-hello main.cpp)
target_link_libraries(context-hello PRIVATE context_warnings)
target_compile_definitions(context-hello PRIVATE
    CONTEXT_ENGINE_NAME="Context Game Engine"
    CONTEXT_ENGINE_VERSION="${PROJECT_VERSION}")

add_test(NAME context-hello-runs COMMAND context-hello)
