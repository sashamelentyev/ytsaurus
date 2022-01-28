#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/misc/error_code.h>

#include <library/cpp/yt/string/format.h>

#include <ostream>

////////////////////////////////////////////////////////////////////////////////

DEFINE_ERROR_ENUM(EErrorCode,
    ((Global1) (-5))
    ((Global2) (-6))
);

namespace NExternalWorld {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ERROR_ENUM(EErrorCode,
    ((X) (-11))
    ((Y) (-22))
    ((Z) (-33))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NExternalWorld

namespace NYT {

void PrintTo(const TErrorCodeRegistry::TErrorCodeInfo& errorCodeInfo, std::ostream* os)
{
    *os << Format("%v::EErrorCode::%v", errorCodeInfo.Namespace, errorCodeInfo.Name);
}

namespace NInternalLittleWorld {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ERROR_ENUM(EErrorCode,
    ((A) (-1))
    ((B) (-2))
    ((C) (-3))
    ((D) (-4))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NMyOwnLittleWorld

namespace {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ERROR_ENUM(EErrorCode,
    ((Kek)     (-57))
    ((Haha)    (-179))
    ((Muahaha) (-1543))
    ((Kukarek) (-2007))
);

TEST(TErrorCodeRegistryTest, Basic)
{
    EXPECT_EQ(
        TErrorCodeRegistry::Get()->Get(-1543),
        (TErrorCodeRegistry::TErrorCodeInfo{"NYT::(anonymous namespace)", "Muahaha"}));
    EXPECT_EQ(
        TErrorCodeRegistry::Get()->Get(-3),
        (TErrorCodeRegistry::TErrorCodeInfo{"NYT::NInternalLittleWorld", "C"}));
    EXPECT_EQ(
        TErrorCodeRegistry::Get()->Get(-33),
        (TErrorCodeRegistry::TErrorCodeInfo{"NExternalWorld", "Z"}));
    EXPECT_EQ(
        TErrorCodeRegistry::Get()->Get(-5),
        (TErrorCodeRegistry::TErrorCodeInfo{"", "Global1"}));
    EXPECT_EQ(
        TErrorCodeRegistry::Get()->Get(-111),
        (TErrorCodeRegistry::TErrorCodeInfo{"NUnknown", "ErrorCode-111"}));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT