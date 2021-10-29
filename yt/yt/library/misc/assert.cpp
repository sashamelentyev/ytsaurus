#include "assert.h"

namespace NYT::NDetail {

////////////////////////////////////////////////////////////////////////////////

Y_WEAK void AssertTrapImpl(
    const char* /*trapType*/,
    const char* /*expr*/,
    const char* /*file*/,
    int /*line*/)
{
    YT_BUILTIN_TRAP();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDetail