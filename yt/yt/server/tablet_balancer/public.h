#pragma once

#include <yt/yt/client/table_client/public.h>

#include <yt/yt/client/tablet_client/public.h>

namespace NYT::NTabletBalancer {

////////////////////////////////////////////////////////////////////////////////

using NTableClient::TTableId;
using NTabletClient::TTabletCellId;
using NTabletClient::TTabletId;
using NTabletClient::TTabletActionId;
using NTabletClient::ETabletActionKind;
using NTabletClient::ETabletActionState;

struct IBootstrap;

DECLARE_REFCOUNTED_CLASS(TStandaloneTabletBalancerConfig)
DECLARE_REFCOUNTED_CLASS(TTabletBalancerDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TTabletBalancerServerConfig)

DECLARE_REFCOUNTED_STRUCT(ITabletBalancer)

DECLARE_REFCOUNTED_STRUCT(TBundleProfilingCounters)
DECLARE_REFCOUNTED_CLASS(TBundleState)

DECLARE_REFCOUNTED_CLASS(TTabletAction)
DECLARE_REFCOUNTED_STRUCT(IActionManager)

DECLARE_REFCOUNTED_CLASS(TDynamicConfigManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletBalancer
