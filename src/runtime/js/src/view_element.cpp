// Backend-agnostic helpers for the R-LANG-009 zero-copy view protocol. Compiled into BOTH the V8
// and the stub builds (it names no v8:: type), so callers can do the byte-offset/length arithmetic
// for a ViewBinding on any toolchain — including the local Strawberry-GCC `dev` gate where the V8
// backend is a stub.

#include "context/runtime/js/js_engine.h"

namespace context::runtime::js
{

std::size_t view_element_width(ViewElement e) noexcept
{
    switch (e)
    {
    case ViewElement::u8:
    case ViewElement::i8:
        return 1;
    case ViewElement::u16:
    case ViewElement::i16:
        return 2;
    case ViewElement::u32:
    case ViewElement::i32:
    case ViewElement::f32:
        return 4;
    case ViewElement::u64:
    case ViewElement::i64:
    case ViewElement::f64:
        return 8;
    }
    return 1; // unreachable — every enumerator is handled above
}

} // namespace context::runtime::js
