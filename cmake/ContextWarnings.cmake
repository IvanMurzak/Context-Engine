# context_warnings — the warnings-as-errors baseline every Context target links.
# Controlled by the CONTEXT_WARNINGS_AS_ERRORS option (default ON).

add_library(context_warnings INTERFACE)

if(MSVC)
    target_compile_options(context_warnings INTERFACE
        /W4
        $<$<BOOL:${CONTEXT_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(context_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${CONTEXT_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
