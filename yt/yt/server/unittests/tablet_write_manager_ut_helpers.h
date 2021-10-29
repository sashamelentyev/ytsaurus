#pragma once

#include <yt/yt/core/test_framework/framework.h>

#include "sorted_store_manager_ut_helpers.h"
#include "simple_hydra_manager_mock.h"
#include "simple_transaction_supervisor.h"
#include "simple_tablet_manager.h"

#include <yt/yt/server/node/tablet_node/automaton.h>
#include <yt/yt/server/node/tablet_node/bootstrap.h>
#include <yt/yt/server/node/tablet_node/tablet.h>
#include <yt/yt/server/node/tablet_node/tablet_slot.h>
#include <yt/yt/server/node/tablet_node/transaction.h>
#include <yt/yt/server/node/tablet_node/transaction_manager.h>
#include <yt/yt/server/node/tablet_node/serialize.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/server/lib/hive/transaction_lease_tracker.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/client/transaction_client/helpers.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/client/table_client/helpers.h>

namespace NYT::NTabletNode {
namespace {

// TODO(max42): split into .cpp and .h.

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NSecurityClient;
using namespace NTransactionClient;
using namespace NHydra;
using namespace NHiveServer;
using namespace NRpc;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

static const TLogger Logger("Test");

////////////////////////////////////////////////////////////////////////////////

class TSimpleTabletSlot
    : public ITransactionManagerHost
{
public:
    explicit TSimpleTabletSlot(TTabletOptions options)
    {
        AutomatonQueue_ = New<TActionQueue>("Automaton");
        AutomatonInvoker_ = AutomatonQueue_->GetInvoker();
        Automaton_ = New<TTabletAutomaton>(/*asyncSnapshotInvoker*/ AutomatonInvoker_, CellId);
        HydraManager_ = New<TSimpleHydraManagerMock>(Automaton_, AutomatonInvoker_, NTabletNode::GetCurrentReign());
        TransactionManager_ = New<TTransactionManager>(New<TTransactionManagerConfig>(), /*transactionManagerHost*/ this, CreateNullTransactionLeaseTracker());
        TransactionSupervisor_ = New<TSimpleTransactionSupervisor>(TransactionManager_, HydraManager_, Automaton_, AutomatonInvoker_);
        TabletManager_ = New<TSimpleTabletManager>(TransactionManager_, HydraManager_, Automaton_, AutomatonInvoker_);
        TabletWriteManager_ = CreateTabletWriteManager(TabletManager_.Get(), HydraManager_, Automaton_, TMemoryUsageTrackerGuard(), AutomatonInvoker_);

        TabletManager_->InitializeTablet(options);
        TabletWriteManager_->Initialize();
    }

    NHydra::ISimpleHydraManagerPtr GetSimpleHydraManager() override
    {
        return HydraManager_;
    }

    const NHydra::TCompositeAutomatonPtr& GetAutomaton() override
    {
        return Automaton_;
    }

    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue /*queue*/ = EAutomatonThreadQueue::Default) override
    {
        return AutomatonInvoker_;
    }

    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue /*queue*/ = EAutomatonThreadQueue::Default) override
    {
        return AutomatonInvoker_;
    }

    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue /*queue*/ = EAutomatonThreadQueue::Default) override
    {
        return AutomatonInvoker_;
    }

    void Shutdown()
    {
        AutomatonQueue_->Shutdown();
    }

    const NHiveServer::ITransactionSupervisorPtr& GetTransactionSupervisor() override
    {
        // Lease checking is disabled, so transaction supervisor is not needed.
        YT_UNIMPLEMENTED();
    }

    const TRuntimeTabletCellDataPtr& GetRuntimeData() override
    {
        static TRuntimeTabletCellDataPtr RuntimeTabletCellData = nullptr;
        return RuntimeTabletCellData;
    }

    NTransactionClient::TTimestamp GetLatestTimestamp() override
    {
        return LatestTimestamp_;
    }

    NObjectClient::TCellTag GetNativeCellTag() override
    {
        return TCellTag();
    }

    NHydra::TCellId GetCellId() override
    {
        return CellId;
    }

    void SetLatestTimestamp(TTimestamp timestamp)
    {
        LatestTimestamp_ = timestamp;
    }

    const TSimpleTabletManagerPtr& TabletManager()
    {
        return TabletManager_;
    }

    const ITabletWriteManagerPtr& TabletWriteManager()
    {
        return TabletWriteManager_;
    }

    const TSimpleHydraManagerMockPtr& HydraManager()
    {
        return HydraManager_;
    }

    const TTransactionManagerPtr& TransactionManager()
    {
        return TransactionManager_;
    }

    const TSimpleTransactionSupervisorPtr& TransactionSupervisor()
    {
        return TransactionSupervisor_;
    }

private:
    TSimpleHydraManagerMockPtr HydraManager_;
    TActionQueuePtr AutomatonQueue_;
    IInvokerPtr AutomatonInvoker_;
    TCompositeAutomatonPtr Automaton_;
    TTransactionManagerPtr TransactionManager_;
    TSimpleTransactionSupervisorPtr TransactionSupervisor_;
    TSimpleTabletManagerPtr TabletManager_;
    ITabletWriteManagerPtr TabletWriteManager_;

    TTimestamp LatestTimestamp_ = 4242;

    static constexpr TCellId CellId = {0, 42};
};

DECLARE_REFCOUNTED_CLASS(TSimpleTabletSlot)
DEFINE_REFCOUNTED_TYPE(TSimpleTabletSlot)

////////////////////////////////////////////////////////////////////////////////

class TTabletWriteManagerTestBase
    : public testing::Test
{
protected:
    TSimpleTabletSlotPtr TabletSlot_;

    virtual TTabletOptions GetOptions() const = 0;

    void SetUp() override
    {
        TabletSlot_ = New<TSimpleTabletSlot>(GetOptions());
    }

    void TearDown() override
    {
        TabletSlot_->Shutdown();
    }

    const ITabletWriteManagerPtr& TabletWriteManager()
    {
        return TabletSlot_->TabletWriteManager();
    }

    IInvokerPtr AutomatonInvoker()
    {
        return TabletSlot_->GetAutomatonInvoker();
    }

    TSimpleHydraManagerMockPtr HydraManager()
    {
        return TabletSlot_->HydraManager();
    }

    TTransactionManagerPtr TransactionManager()
    {
        return TabletSlot_->TransactionManager();
    }

    TSimpleTransactionSupervisorPtr TransactionSupervisor()
    {
        return TabletSlot_->TransactionSupervisor();
    }

    TUnversionedOwningRow BuildRow(const TString& yson, bool treatMissingAsNull = true)
    {
        return NTableClient::YsonToSchemafulRow(yson, *TabletSlot_->TabletManager()->Tablet()->GetPhysicalSchema(), treatMissingAsNull);
    }

    TVersionedOwningRow VersionedLookupRow(const TLegacyOwningKey& key, int minDataVersions = 100, TTimestamp timestamp = AsyncLastCommittedTimestamp)
    {
        return VersionedLookupRowImpl(TabletSlot_->TabletManager()->Tablet(), key, minDataVersions, timestamp);
    }

    // Recall that  may wait on blocked row.

    TFuture<void> WriteUnversionedRows(TTransactionId transactionId, std::vector<TUnversionedOwningRow> rows, TTransactionSignature signature = -1)
    {
        auto* tablet = TabletSlot_->TabletManager()->Tablet();
        auto tabletSnapshot = tablet->BuildSnapshot(nullptr);
        auto asyncResult = BIND([transactionId, rows = std::move(rows), signature, tabletWriteManager = TabletWriteManager(), tabletSnapshot] {
            TWireProtocolWriter writer;
            i64 dataWeight = 0;
            for (const auto& row : rows) {
                writer.WriteCommand(EWireProtocolCommand::WriteRow);
                writer.WriteUnversionedRow(row);
                dataWeight += GetDataWeight(row);
            }
            auto wireData = writer.Finish();
            struct TTag {};
            TWireProtocolReader reader(MergeRefsToRef<TTag>(wireData));
            TFuture<void> asyncResult;
            tabletWriteManager->Write(
                tabletSnapshot,
                transactionId,
                TimestampFromTransactionId(transactionId),
                TDuration::Max(),
                signature,
                rows.size(),
                dataWeight,
                /*versioned*/ false,
                TSyncReplicaIdList(),
                &reader,
                &asyncResult);
            return asyncResult;
        })
            .AsyncVia(AutomatonInvoker())
            .Run();
        return asyncResult;
    }

    TFuture<void> WriteVersionedRows(TTransactionId transactionId, std::vector<TVersionedOwningRow> rows, TTransactionSignature signature = -1)
    {
        auto* tablet = TabletSlot_->TabletManager()->Tablet();
        auto tabletSnapshot = tablet->BuildSnapshot(nullptr);
        auto asyncResult = BIND([transactionId, rows = std::move(rows), signature, tabletWriteManager = TabletWriteManager(), tabletSnapshot] {
            TWireProtocolWriter writer;
            i64 dataWeight = 0;
            for (const auto& row : rows) {
                writer.WriteCommand(EWireProtocolCommand::VersionedWriteRow);
                writer.WriteVersionedRow(row);
                dataWeight += GetDataWeight(row);
            }
            auto wireData = writer.Finish();
            struct TTag {};
            TWireProtocolReader reader(MergeRefsToRef<TTag>(wireData));
            TFuture<void> asyncResult;

            TAuthenticationIdentity identity(ReplicatorUserName);
            TCurrentAuthenticationIdentityGuard guard(&identity);

            tabletWriteManager->Write(
                tabletSnapshot,
                transactionId,
                TimestampFromTransactionId(transactionId),
                TDuration::Max(),
                signature,
                rows.size(),
                dataWeight,
                /*versioned*/ true,
                TSyncReplicaIdList(),
                &reader,
                &asyncResult);
            return asyncResult;
        })
            .AsyncVia(AutomatonInvoker())
            .Run();
        return asyncResult;
    }

    auto RunInAutomaton(auto callable)
    {
        auto result = WaitFor(
            BIND(callable)
                .AsyncVia(AutomatonInvoker())
                .Run());
        if constexpr (!std::is_same_v<std::decay_t<decltype(result)>, TError>) {
            return result
                .ValueOrThrow();
        } else {
            result
                .ThrowOnError();
            return;
        }
    }

    void PrepareTransactionCommit(TTransactionId transactionId, bool persistent, TTimestamp prepareTimestamp)
    {
        RunInAutomaton([&] {
            TransactionSupervisor()->PrepareTransactionCommit(
                transactionId,
                persistent,
                prepareTimestamp);
        });
    }

    void CommitTransaction(TTransactionId transactionId, TTimestamp commitTimestamp)
    {
        RunInAutomaton([&] {
            TransactionSupervisor()->CommitTransaction(
                transactionId,
                commitTimestamp);
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

}
} // namespace NYT::NTabletNode