#include "client_impl.h"

#include "box.h"
#include "config.h"
#include "connection.h"
#include "private.h"
#include "rpc_helpers.h"
#include "transaction.h"
#include "default_type_handler.h"
#include "replicated_table_replica_type_handler.h"
#include "replication_card_type_handler.h"
#include "replication_card_replica_type_handler.h"

#include <yt/yt/client/tablet_client/public.h>
#include <yt/yt/client/tablet_client/table_mount_cache.h>

#include <yt/yt/ytlib/api/native/tablet_helpers.h>

#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/yt/ytlib/chunk_client/proto/medium_directory.pb.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/yt/ytlib/hive/cell_directory.h>
#include <yt/yt/ytlib/hive/cell_directory_synchronizer.h>
#include <yt/yt/ytlib/hive/cluster_directory.h>
#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>

#include <yt/yt/ytlib/node_tracker_client/channel.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/table_client/config.h>

#include <yt/yt/ytlib/transaction_client/action.h>
#include <yt/yt/ytlib/transaction_client/helpers.h>
#include <yt/yt/ytlib/transaction_client/transaction_manager.h>

#include <yt/yt/ytlib/hydra/peer_channel.h>

#include <yt/yt/ytlib/query_client/functions_cache.h>

#include <yt/yt/client/security_client/helpers.h>

#include <yt/yt/core/concurrency/async_semaphore.h>
#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/rpc/helpers.h>

#include <yt/yt/core/ytree/convert.h>

namespace NYT::NApi::NNative {

using namespace NConcurrency;
using namespace NYPath;
using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NRpc;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NHiveClient;
using namespace NScheduler;
using namespace NHydra;

using NNodeTrackerClient::CreateNodeChannelFactory;
using NNodeTrackerClient::INodeChannelFactoryPtr;

////////////////////////////////////////////////////////////////////////////////

IClientPtr CreateClient(
    IConnectionPtr connection,
    const TClientOptions& options)
{
    YT_VERIFY(connection);

    return New<TClient>(std::move(connection), options);
}

////////////////////////////////////////////////////////////////////////////////

TClient::TClient(
    IConnectionPtr connection,
    const TClientOptions& options)
    : Connection_(std::move(connection))
    , Options_(options)
    , Logger(ApiLogger.WithTag("ClientId: %v", TGuid::Create()))
    , TypeHandlers_{
        CreateReplicatedTableReplicaTypeHandler(this),
        CreateReplicationCardTypeHandler(this),
        CreateReplicationCardReplicaTypeHandler(this),
        CreateDefaultTypeHandler(this)
    }
    , FunctionImplCache_(BIND(CreateFunctionImplCache,
        Connection_->GetConfig()->FunctionImplCache,
        MakeWeak(this)))
    , FunctionRegistry_ (BIND(CreateFunctionRegistryCache,
        Connection_->GetConfig()->FunctionRegistryCache,
        MakeWeak(this),
        Connection_->GetInvoker()))
{
    if (!Options_.User) {
        THROW_ERROR_EXCEPTION("Native connection requires non-null \"user\" parameter");
    }

    auto wrapChannel = [&] (IChannelPtr channel) {
        channel = CreateAuthenticatedChannel(std::move(channel), options.GetAuthenticationIdentity());
        return channel;
    };
    auto wrapChannelFactory = [&] (IChannelFactoryPtr factory) {
        factory = CreateAuthenticatedChannelFactory(std::move(factory), options.GetAuthenticationIdentity());
        return factory;
    };

    auto initMasterChannel = [&] (EMasterChannelKind kind, TCellTag cellTag) {
        MasterChannels_[kind][cellTag] = wrapChannel(Connection_->GetMasterChannelOrThrow(kind, cellTag));
    };
    for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
        initMasterChannel(kind, Connection_->GetPrimaryMasterCellTag());
        for (auto cellTag : Connection_->GetSecondaryMasterCellTags()) {
            initMasterChannel(kind, cellTag);
        }
    }

    SchedulerChannel_ = wrapChannel(Connection_->GetSchedulerChannel());

    ChannelFactory_ = CreateNodeChannelFactory(
        wrapChannelFactory(Connection_->GetChannelFactory()),
        Connection_->GetNetworks());

    SchedulerProxy_ = std::make_unique<TSchedulerServiceProxy>(GetSchedulerChannel());
    JobProberProxy_ = std::make_unique<TJobProberServiceProxy>(GetSchedulerChannel());

    TransactionManager_ = New<TTransactionManager>(
        Connection_,
        Options_.GetAuthenticatedUser());
}

NApi::IConnectionPtr TClient::GetConnection()
{
    return Connection_;
}

const ITableMountCachePtr& TClient::GetTableMountCache()
{
    return Connection_->GetTableMountCache();
}

const NChaosClient::IReplicationCardCachePtr& TClient::GetReplicationCardCache()
{
    return Connection_->GetReplicationCardCache();
}

const ITimestampProviderPtr& TClient::GetTimestampProvider()
{
    return Connection_->GetTimestampProvider();
}

const IConnectionPtr& TClient::GetNativeConnection()
{
    return Connection_;
}

IFunctionRegistryPtr TClient::GetFunctionRegistry()
{
    return FunctionRegistry_.Get();
}

TFunctionImplCachePtr TClient::GetFunctionImplCache()
{
    return FunctionImplCache_.Get();
}

const TClientOptions& TClient::GetOptions()
{
    return Options_;
}

IChannelPtr TClient::GetMasterChannelOrThrow(
    EMasterChannelKind kind,
    TCellTag cellTag)
{
    const auto& channels = MasterChannels_[kind];
    auto it = channels.find(cellTag == PrimaryMasterCellTagSentinel ? Connection_->GetPrimaryMasterCellTag() : cellTag);
    if (it == channels.end()) {
        YT_ABORT();
        THROW_ERROR_EXCEPTION("Unknown master cell tag %v",
            cellTag);
    }
    return it->second;
}

IChannelPtr TClient::GetCellChannelOrThrow(TCellId cellId)
{
    const auto& cellDirectory = Connection_->GetCellDirectory();
    auto channel = cellDirectory->GetChannelByCellIdOrThrow(cellId);
    return CreateAuthenticatedChannel(std::move(channel), Options_.GetAuthenticationIdentity());
}

IChannelPtr TClient::GetSchedulerChannel()
{
    return SchedulerChannel_;
}

const INodeChannelFactoryPtr& TClient::GetChannelFactory()
{
    return ChannelFactory_;
}

void TClient::Terminate()
{
    TransactionManager_->AbortAll();

    auto error = TError("Client terminated");

    for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
        for (const auto& [cellTag, channel]  : MasterChannels_[kind]) {
            channel->Terminate(error);
        }
    }
    SchedulerChannel_->Terminate(error);
}

const IChannelPtr& TClient::GetOperationArchiveChannel(EMasterChannelKind kind)
{
    {
        auto guard = Guard(OperationsArchiveChannelsLock_);
        if (OperationsArchiveChannels_) {
            return (*OperationsArchiveChannels_)[kind];
        }
    }

    TEnumIndexedVector<EMasterChannelKind, NRpc::IChannelPtr> channels;
    for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
        // NOTE(asaitgalin): Cache is tied to user so to utilize cache properly all Cypress
        // requests for operations archive should be performed under the same user.
        channels[kind] = CreateAuthenticatedChannel(
            Connection_->GetMasterChannelOrThrow(kind, PrimaryMasterCellTagSentinel),
            NRpc::TAuthenticationIdentity(NSecurityClient::OperationsClientUserName));
    }

    {
        auto guard = Guard(OperationsArchiveChannelsLock_);
        if (!OperationsArchiveChannels_) {
            OperationsArchiveChannels_ = std::move(channels);
        }
        return (*OperationsArchiveChannels_)[kind];
    }
}

template <class T>
TFuture<T> TClient::Execute(
    TStringBuf commandName,
    const TTimeoutOptions& options,
    TCallback<T()> callback)
{
    auto promise = NewPromise<T>();
    BIND([
        commandName,
        promise,
        callback = std::move(callback),
        this,
        this_ = MakeWeak(this)
    ] () mutable {
        auto client = this_.Lock();
        if (!client) {
            return;
        }

        if (promise.IsCanceled()) {
            return;
        }

        auto canceler = NConcurrency::GetCurrentFiberCanceler();
        if (canceler) {
            promise.OnCanceled(std::move(canceler));
        }

        try {
            YT_LOG_DEBUG("Command started (Command: %v)", commandName);
            TBox<T> result(callback);
            YT_LOG_DEBUG("Command completed (Command: %v)", commandName);
            result.SetPromise(promise);
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG(ex, "Command failed (Command: %v)", commandName);
            promise.Set(TError(ex));
        }
    })
        .Via(Connection_->GetInvoker())
        .Run();

    return promise
        .ToFuture()
        .WithTimeout(options.Timeout);
}

void TClient::SetMutationId(const IClientRequestPtr& request, const TMutatingOptions& options)
{
    NRpc::SetMutationId(request, options.GetOrGenerateMutationId(), options.Retry);
}

TTransactionId TClient::GetTransactionId(const TTransactionalOptions& options, bool allowNullTransaction)
{
    if (!options.TransactionId) {
        if (!allowNullTransaction) {
            THROW_ERROR_EXCEPTION("A valid master transaction is required");
        }
        return {};
    }

    if (options.Ping) {
        // XXX(babenko): this is just to make a ping; shall we even support this?
        TTransactionAttachOptions attachOptions;
        attachOptions.Ping = options.Ping;
        attachOptions.PingAncestors = options.PingAncestors;
        TransactionManager_->Attach(options.TransactionId, attachOptions);
    }

    return options.TransactionId;
}

void TClient::SetTransactionId(
    const IClientRequestPtr& request,
    const TTransactionalOptions& options,
    bool allowNullTransaction)
{
    NCypressClient::SetTransactionId(request, GetTransactionId(options, allowNullTransaction));
}

void TClient::SetPrerequisites(
    const IClientRequestPtr& request,
    const TPrerequisiteOptions& options)
{
    NTransactionClient::SetPrerequisites(request, options);
}

void TClient::SetSuppressAccessTracking(
    const IClientRequestPtr& request,
    const TSuppressableAccessTrackingOptions& commandOptions)
{
    if (commandOptions.SuppressAccessTracking) {
        NCypressClient::SetSuppressAccessTracking(request, true);
    }
    if (commandOptions.SuppressModificationTracking) {
        NCypressClient::SetSuppressModificationTracking(request, true);
    }
    if (commandOptions.SuppressExpirationTimeoutRenewal) {
        NCypressClient::SetSuppressExpirationTimeoutRenewal(request, true);
    }
}

void TClient::SetCachingHeader(
    const IClientRequestPtr& request,
    const TMasterReadOptions& options)
{
    NApi::NNative::SetCachingHeader(request, Connection_->GetConfig(), options);
}

void TClient::SetBalancingHeader(
    const TObjectServiceProxy::TReqExecuteBatchPtr& request,
    const TMasterReadOptions& options)
{
    NApi::NNative::SetBalancingHeader(request, Connection_->GetConfig(), options);
}

template <class TProxy>
std::unique_ptr<TProxy> TClient::CreateReadProxy(
    const TMasterReadOptions& options,
    TCellTag cellTag)
{
    auto channel = GetMasterChannelOrThrow(options.ReadFrom, cellTag);
    return std::make_unique<TProxy>(channel, Connection_->GetStickyGroupSizeCache());
}

template std::unique_ptr<TObjectServiceProxy> TClient::CreateReadProxy<TObjectServiceProxy>(
    const TMasterReadOptions& options,
    TCellTag cellTag);

template <class TProxy>
std::unique_ptr<TProxy> TClient::CreateWriteProxy(
    TCellTag cellTag)
{
    auto channel = GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
    return std::make_unique<TProxy>(channel);
}

template std::unique_ptr<TObjectServiceProxy> TClient::CreateWriteProxy<TObjectServiceProxy>(TCellTag cellTag);
template std::unique_ptr<TChunkServiceProxy> TClient::CreateWriteProxy<TChunkServiceProxy>(TCellTag cellTag);

IChannelPtr TClient::GetReadCellChannelOrThrow(TTabletCellId cellId)
{
    const auto& cellDirectory = Connection_->GetCellDirectory();
    const auto& cellDescriptor = cellDirectory->GetDescriptorOrThrow(cellId);
    const auto& primaryPeerDescriptor = GetPrimaryTabletPeerDescriptor(cellDescriptor, NHydra::EPeerKind::Leader);
    return ChannelFactory_->CreateChannel(primaryPeerDescriptor.GetAddressWithNetworkOrThrow(Connection_->GetNetworks()));
}

IChannelPtr TClient::GetLeaderCellChannelOrThrow(TCellId cellId)
{
    WaitFor(Connection_->GetCellDirectorySynchronizer()->Sync())
        .ThrowOnError();

    const auto& cellDirectory = Connection_->GetCellDirectory();
    if (cellDirectory->IsCellRegistered(cellId)) {
        return cellDirectory->GetChannelByCellIdOrThrow(cellId);
    }

    auto config = Connection_->GetConfig()->ClockServers;
    if (config && config->CellId == cellId) {
        if (!config->Addresses) {
            THROW_ERROR_EXCEPTION("Clock server addresses are empty");
        }
        return CreatePeerChannel(config, Connection_->GetChannelFactory(), EPeerKind::Leader);
    }

    THROW_ERROR_EXCEPTION("Unknown cell %v",
        cellId);
}

TCellDescriptor TClient::GetCellDescriptorOrThrow(TCellId cellId)
{
    const auto& cellDirectory = Connection_->GetCellDirectory();
    if (auto cellDescriptor = cellDirectory->FindDescriptor(cellId)) {
        return *cellDescriptor;
    }

    WaitFor(Connection_->GetCellDirectorySynchronizer()->Sync())
        .ThrowOnError();

    return cellDirectory->GetDescriptorOrThrow(cellId);
}

std::vector<TString> TClient::GetCellAddressesOrThrow(NObjectClient::TCellId cellId)
{
    const auto& cellDirectory = Connection_->GetCellDirectory();
    if (cellDirectory->IsCellRegistered(cellId)) {
        std::vector<TString> addresses;
        auto cellDescriptor = GetCellDescriptorOrThrow(cellId);
        for (const auto& peerDescriptor : cellDescriptor.Peers) {
            addresses.push_back(peerDescriptor.GetDefaultAddress());
        }
        return addresses;
    }

    auto config = Connection_->GetConfig()->ClockServers;
    if (config && config->CellId == cellId) {
        if (!config->Addresses) {
            THROW_ERROR_EXCEPTION("Clock server addresses are empty");
        }
        return *config->Addresses;
    }

    THROW_ERROR_EXCEPTION("Unknown cell %v",
        cellId);
}

void TClient::ValidateSuperuserPermissions()
{
    if (Options_.User == NSecurityClient::RootUserName) {
        return;
    }

    auto pathToGroupYsonList = NSecurityClient::GetUserPath(*Options_.User) + "/@member_of_closure";
    TGetNodeOptions options;
    options.SuppressTransactionCoordinatorSync = true;
    auto groupYsonList = WaitFor(GetNode(pathToGroupYsonList, options))
        .ValueOrThrow();

    auto groups = ConvertTo<THashSet<TString>>(groupYsonList);
    YT_LOG_DEBUG("User group membership info received (Name: %v, Groups: %v)",
        Options_.User,
        groups);

    if (!groups.contains(NSecurityClient::SuperusersGroupName)) {
        THROW_ERROR_EXCEPTION("Superuser permissions required");
    }
}

TClusterMeta TClient::DoGetClusterMeta(
    const TGetClusterMetaOptions& options)
{
    auto proxy = CreateReadProxy<TObjectServiceProxy>(options);
    auto batchReq = proxy->ExecuteBatch();
    batchReq->SetSuppressTransactionCoordinatorSync(true);
    SetBalancingHeader(batchReq, options);

    auto req = TMasterYPathProxy::GetClusterMeta();
    req->set_populate_node_directory(options.PopulateNodeDirectory);
    req->set_populate_cluster_directory(options.PopulateClusterDirectory);
    req->set_populate_medium_directory(options.PopulateMediumDirectory);
    req->set_populate_master_cache_node_addresses(options.PopulateMasterCacheNodeAddresses);
    req->set_populate_timestamp_provider_node_addresses(options.PopulateTimestampProviderAddresses);
    req->set_populate_features(options.PopulateFeatures);
    SetCachingHeader(req, options);
    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    auto rsp = batchRsp->GetResponse<TMasterYPathProxy::TRspGetClusterMeta>(0)
        .ValueOrThrow();

    TClusterMeta meta;
    if (options.PopulateNodeDirectory) {
        meta.NodeDirectory = std::make_shared<NNodeTrackerClient::NProto::TNodeDirectory>();
        meta.NodeDirectory->Swap(rsp->mutable_node_directory());
    }
    if (options.PopulateClusterDirectory) {
        meta.ClusterDirectory = std::make_shared<NHiveClient::NProto::TClusterDirectory>();
        meta.ClusterDirectory->Swap(rsp->mutable_cluster_directory());
    }
    if (options.PopulateMediumDirectory) {
        meta.MediumDirectory = std::make_shared<NChunkClient::NProto::TMediumDirectory>();
        meta.MediumDirectory->Swap(rsp->mutable_medium_directory());
    }
    if (options.PopulateMasterCacheNodeAddresses) {
        meta.MasterCacheNodeAddresses = FromProto<std::vector<TString>>(rsp->master_cache_node_addresses());
    }
    if (options.PopulateTimestampProviderAddresses) {
        meta.TimestampProviderAddresses = FromProto<std::vector<TString>>(rsp->timestamp_provider_node_addresses());
    }
    if (options.PopulateFeatures && rsp->has_features()) {
        meta.Features = ConvertTo<IMapNodePtr>(TYsonStringBuf(rsp->features()));
    }
    return meta;
}

void TClient::DoCheckClusterLiveness(
    const TCheckClusterLivenessOptions& options)
{
    if (options.IsCheckTrivial()) {
        THROW_ERROR_EXCEPTION("No liveness check methods specified");
    }

    std::vector<TFuture<TObjectServiceProxy::TRspExecuteBatchPtr>> futures;
    auto makeRequest = [&] (auto proxy) {
        auto batchReq = proxy->ExecuteBatch();

        auto req = TYPathProxy::List("/");
        req->set_limit(1);
        batchReq->AddRequest(req);

        futures.push_back(batchReq->Invoke());
    };

    TMasterReadOptions masterReadOptions;
    if (options.CheckCypressRoot) {
        makeRequest(CreateReadProxy<TObjectServiceProxy>(masterReadOptions));
    }
    if (options.CheckSecondaryMasterCells) {
        for (auto secondaryCellTag : Connection_->GetSecondaryMasterCellTags()) {
            makeRequest(CreateReadProxy<TObjectServiceProxy>(masterReadOptions, secondaryCellTag));
        }
    }

    auto batchResponses = WaitFor(AllSucceeded(std::move(futures))
        .WithTimeout(Connection_->GetConfig()->ClusterLivenessCheckTimeout))
        .ValueOrThrow();
    for (const auto& batchResponse : batchResponses) {
        batchResponse->GetResponse<TYPathProxy::TRspList>(0)
            .ThrowOnError();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative