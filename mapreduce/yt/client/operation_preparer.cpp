#include "operation_preparer.h"

#include "init.h"
#include "file_writer.h"
#include "file_writer.h"
#include "operation_helpers.h"
#include "operation_tracker.h"
#include "transaction.h"
#include "yt_poller.h"

#include <mapreduce/yt/common/helpers.h>
#include <mapreduce/yt/common/retry_lib.h>

#include <mapreduce/yt/raw_client/raw_requests.h>
#include <mapreduce/yt/raw_client/raw_batch_request.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <library/cpp/digest/md5/md5.h>

#include <util/folder/path.h>

#include <util/string/builder.h>

#include <util/system/execpath.h>

namespace NYT::NDetail {

using namespace NRawClient;

////////////////////////////////////////////////////////////////////////////////

class TWaitOperationStartPollerItem
    : public IYtPollerItem
{
public:
    TWaitOperationStartPollerItem(TOperationId operationId, THolder<TPingableTransaction> transaction)
        : OperationId_(operationId)
        , Transaction_(std::move(transaction))
    { }

    void PrepareRequest(TRawBatchRequest* batchRequest) override
    {
        Future_ = batchRequest->GetOperation(
            OperationId_,
            TGetOperationOptions().AttributeFilter(
                TOperationAttributeFilter().Add(EOperationAttribute::State)));
    }

    EStatus OnRequestExecuted() override
    {
        try {
            auto attributes = Future_.GetValue();
            Y_ENSURE(attributes.State.Defined());
            bool operationHasLockedFiles =
                *attributes.State != "starting" &&
                *attributes.State != "pending" &&
                *attributes.State != "orphaned" &&
                *attributes.State != "waiting_for_agent" &&
                *attributes.State != "initializing";
            return operationHasLockedFiles ? EStatus::PollBreak : EStatus::PollContinue;
        } catch (const TErrorResponse& e) {
            LOG_ERROR("get_operation request %s failed: %s", e.GetRequestId().data(), e.GetError().GetMessage().data());
            return IsRetriable(e) ? PollContinue : PollBreak;
        } catch (const yexception& e) {
            LOG_ERROR("%s", e.what());
            return PollBreak;
        }
    }

private:
    TOperationId OperationId_;
    THolder<TPingableTransaction> Transaction_;
    NThreading::TFuture<TOperationAttributes> Future_;
};

////////////////////////////////////////////////////////////////////////////////

TOperationPreparer::TOperationPreparer(TClientPtr client, TTransactionId transactionId)
    : Client_(std::move(client))
    , TransactionId_(transactionId)
    , FileTransaction_(MakeHolder<TPingableTransaction>(
        Client_->GetRetryPolicy(),
        Client_->GetAuth(),
        TransactionId_,
        TStartTransactionOptions()))
    , ClientRetryPolicy_(Client_->GetRetryPolicy())
    , PreparationId_(CreateGuidAsString())
{ }

const TAuth& TOperationPreparer::GetAuth() const
{
    return Client_->GetAuth();
}

TTransactionId TOperationPreparer::GetTransactionId() const
{
    return TransactionId_;
}

const TString& TOperationPreparer::GetPreparationId() const
{
    return PreparationId_;
}

const IClientRetryPolicyPtr& TOperationPreparer::GetClientRetryPolicy() const
{
    return ClientRetryPolicy_;
}

TOperationId TOperationPreparer::StartOperation(
    const TString& operationType,
    const TNode& spec,
    bool useStartOperationRequest)
{
    CheckValidity();

    THttpHeader header("POST", (useStartOperationRequest ? "start_op" : operationType));
    if (useStartOperationRequest) {
        header.AddParameter("operation_type", operationType);
    }
    header.AddTransactionId(TransactionId_);
    header.AddMutationId();

    auto ysonSpec = NodeToYsonString(spec);
    auto responseInfo = RetryRequestWithPolicy(
        ClientRetryPolicy_->CreatePolicyForStartOperationRequest(),
        GetAuth(),
        header,
        ysonSpec);
    TOperationId operationId = ParseGuidFromResponse(responseInfo.Response);
    LOG_DEBUG("Operation started (OperationId: %s; PreparationId: %s)",
        GetGuidAsString(operationId).c_str(),
        GetPreparationId().c_str());

    LOG_INFO("Operation %s started (%s): %s",
        GetGuidAsString(operationId).data(),
        operationType.data(),
        GetOperationWebInterfaceUrl(GetAuth().ServerName, operationId).data());

    TOperationExecutionTimeTracker::Get()->Start(operationId);

    Client_->GetYtPoller().Watch(
        new TWaitOperationStartPollerItem(operationId, std::move(FileTransaction_)));

    return operationId;
}

void TOperationPreparer::LockFiles(TVector<TRichYPath>* paths)
{
    CheckValidity();

    TVector<NThreading::TFuture<TLockId>> lockIdFutures;
    lockIdFutures.reserve(paths->size());
    TRawBatchRequest lockRequest;
    for (const auto& path : *paths) {
        lockIdFutures.push_back(lockRequest.Lock(
            FileTransaction_->GetId(),
            path.Path_,
            ELockMode::LM_SNAPSHOT,
            TLockOptions().Waitable(true)));
    }
    ExecuteBatch(ClientRetryPolicy_->CreatePolicyForGenericRequest(), GetAuth(), lockRequest);

    TVector<NThreading::TFuture<TNode>> nodeIdFutures;
    nodeIdFutures.reserve(paths->size());
    TRawBatchRequest getNodeIdRequest;
    for (const auto& lockIdFuture : lockIdFutures) {
        nodeIdFutures.push_back(getNodeIdRequest.Get(
            FileTransaction_->GetId(),
            TStringBuilder() << '#' << GetGuidAsString(lockIdFuture.GetValue()) << "/@node_id",
            TGetOptions()));
    }
    ExecuteBatch(ClientRetryPolicy_->CreatePolicyForGenericRequest(), GetAuth(), getNodeIdRequest);

    for (size_t i = 0; i != paths->size(); ++i) {
        auto& richPath = (*paths)[i];
        richPath.OriginalPath(richPath.Path_);
        richPath.Path("#" + nodeIdFutures[i].GetValue().AsString());
        LOG_DEBUG("Locked file %s, new path is %s", richPath.OriginalPath_->data(), richPath.Path_.data());
    }
}

void TOperationPreparer::CheckValidity() const
{
    Y_ENSURE(
        FileTransaction_,
        "File transaction is already moved, are you trying to use preparer for more than one operation?");
}

////////////////////////////////////////////////////////////////////////////////

class TRetryPolicyIgnoringLockConflicts
    : public TAttemptLimitedRetryPolicy
{
public:
    using TAttemptLimitedRetryPolicy::TAttemptLimitedRetryPolicy;
    using TAttemptLimitedRetryPolicy::OnGenericError;

    TMaybe<TDuration> OnRetriableError(const TErrorResponse& e) override
    {
        if (IsAttemptLimitExceeded()) {
            return Nothing();
        }
        if (e.IsConcurrentTransactionLockConflict()) {
            return GetBackoffDuration();
        }
        return TAttemptLimitedRetryPolicy::OnRetriableError(e);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TFileToUpload
    : public IItemToUpload
{
public:
    TFileToUpload(TString fileName, TMaybe<TString> md5)
        : FileName_(std::move(fileName))
        , MD5_(std::move(md5))
    { }

    TString CalculateMD5() const override
    {
        if (MD5_) {
            return *MD5_;
        }
        constexpr size_t md5Size = 32;
        TString result;
        result.ReserveAndResize(md5Size);
        MD5::File(FileName_.data(), result.Detach());
        return result;
    }

    THolder<IInputStream> CreateInputStream() const override
    {
        return MakeHolder<TFileInput>(FileName_);
    }

    TString GetDescription() const override
    {
        return FileName_;
    }

private:
    TString FileName_;
    TMaybe<TString> MD5_;
};

class TDataToUpload
    : public IItemToUpload
{
public:
    TDataToUpload(TString data, TString description)
        : Data_(std::move(data))
        , Description_(std::move(description))
    { }

    TString CalculateMD5() const override
    {
        constexpr size_t md5Size = 32;
        TString result;
        result.ReserveAndResize(md5Size);
        MD5::Data(reinterpret_cast<const unsigned char*>(Data_.data()), Data_.size(), result.Detach());
        return result;
    }

    THolder<IInputStream> CreateInputStream() const override
    {
        return MakeHolder<TMemoryInput>(Data_.data(), Data_.size());
    }

    TString GetDescription() const override
    {
        return Description_;
    }

private:
    TString Data_;
    TString Description_;
};

////////////////////////////////////////////////////////////////////////////////

static const TString& GetPersistentExecPathMd5()
{
    static TString md5 = MD5::File(GetPersistentExecPath());
    return md5;
}

static TMaybe<TSmallJobFile> GetJobState(const IJob& job)
{
    TString result;
    {
        TStringOutput output(result);
        job.Save(output);
        output.Finish();
    }
    if (result.empty()) {
        return Nothing();
    } else {
        return TSmallJobFile{"jobstate", result};
    }
}

////////////////////////////////////////////////////////////////////////////////

TJobPreparer::TJobPreparer(
    TOperationPreparer& operationPreparer,
    const TUserJobSpec& spec,
    const IJob& job,
    size_t outputTableCount,
    const TVector<TSmallJobFile>& smallFileList,
    const TOperationOptions& options)
    : OperationPreparer_(operationPreparer)
    , Spec_(spec)
    , Options_(options)
{

    CreateStorage();
    auto cypressFileList = CanonizeYPaths(/* retryPolicy */ nullptr, OperationPreparer_.GetAuth(), spec.Files_);

    for (const auto& file : cypressFileList) {
        UseFileInCypress(file);
    }
    for (const auto& localFile : spec.GetLocalFiles()) {
        UploadLocalFile(std::get<0>(localFile), std::get<1>(localFile));
    }
    auto jobStateSmallFile = GetJobState(job);
    if (jobStateSmallFile) {
        UploadSmallFile(*jobStateSmallFile);
    }
    for (const auto& smallFile : smallFileList) {
        UploadSmallFile(smallFile);
    }

    if (auto commandJob = dynamic_cast<const ICommandJob*>(&job)) {
        ClassName_ = TJobFactory::Get()->GetJobName(&job);
        Command_ = commandJob->GetCommand();
    } else {
        PrepareJobBinary(job, outputTableCount, jobStateSmallFile.Defined());
    }

    operationPreparer.LockFiles(&CachedFiles_);
}

TVector<TRichYPath> TJobPreparer::GetFiles() const
{
    TVector<TRichYPath> allFiles = CypressFiles_;
    allFiles.insert(allFiles.end(), CachedFiles_.begin(), CachedFiles_.end());
    return allFiles;
}

const TString& TJobPreparer::GetClassName() const
{
    return ClassName_;
}

const TString& TJobPreparer::GetCommand() const
{
    return Command_;
}

const TUserJobSpec& TJobPreparer::GetSpec() const
{
    return Spec_;
}

bool TJobPreparer::ShouldMountSandbox() const
{
    return TConfig::Get()->MountSandboxInTmpfs || Options_.MountSandboxInTmpfs_;
}

ui64 TJobPreparer::GetTotalFileSize() const
{
    return TotalFileSize_;
}

TString TJobPreparer::GetFileStorage() const
{
    return Options_.FileStorage_ ?
        *Options_.FileStorage_ :
        TConfig::Get()->RemoteTempFilesDirectory;
}

TYPath TJobPreparer::GetCachePath() const
{
    return AddPathPrefix(TStringBuilder() << GetFileStorage() << "/new_cache");
}

void TJobPreparer::CreateStorage() const
{
    Create(
        OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
        OperationPreparer_.GetAuth(),
        Options_.FileStorageTransactionId_,
        GetCachePath(),
        NT_MAP,
        TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true));
}

int TJobPreparer::GetFileCacheReplicationFactor() const
{
    if (IsLocalMode()) {
        return 1;
    } else {
        return TConfig::Get()->FileCacheReplicationFactor;
    }
}

TString TJobPreparer::UploadToRandomPath(const IItemToUpload& itemToUpload) const
{
    TString uniquePath = AddPathPrefix(TStringBuilder() << GetFileStorage() << "/cpp_" << CreateGuidAsString());
    LOG_INFO("Uploading file to random cypress path (FileName: %s; CypressPath: %s; PreparationId: %s)",
        itemToUpload.GetDescription().c_str(),
        uniquePath.c_str(),
        OperationPreparer_.GetPreparationId().c_str());

    Create(
        OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
        OperationPreparer_.GetAuth(),
        Options_.FileStorageTransactionId_,
        uniquePath,
        NT_FILE,
        TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true)
    .Attributes(TNode()("replication_factor", GetFileCacheReplicationFactor()))
    );
    {
        TFileWriter writer(
            uniquePath,
            OperationPreparer_.GetClientRetryPolicy(),
            OperationPreparer_.GetAuth(),
            Options_.FileStorageTransactionId_);
        itemToUpload.CreateInputStream()->ReadAll(writer);
        writer.Finish();
    }
    return uniquePath;
}

TString TJobPreparer::UploadToCacheUsingApi(const IItemToUpload& itemToUpload) const
{
    auto md5Signature = itemToUpload.CalculateMD5();
    Y_VERIFY(md5Signature.size() == 32);

    constexpr ui32 LockConflictRetryCount = 30;
    auto retryPolicy = MakeIntrusive<TRetryPolicyIgnoringLockConflicts>(LockConflictRetryCount);
    auto maybePath = GetFileFromCache(
        retryPolicy,
        OperationPreparer_.GetAuth(),
        TTransactionId(),
        md5Signature,
        GetCachePath(),
        TGetFileFromCacheOptions());
    if (maybePath) {
        LOG_DEBUG("File is already in cache (FileName: %s)",
            itemToUpload.GetDescription().c_str(),
            maybePath->c_str());
        return *maybePath;
    }

    TString uniquePath = AddPathPrefix(TStringBuilder() << GetFileStorage() << "/cpp_" << CreateGuidAsString());
    LOG_INFO("File not found in cache; uploading to cypress (FileName: %s; CypressPath: %s; PreparationId: %s)",
        itemToUpload.GetDescription().c_str(),
        uniquePath.c_str(),
        OperationPreparer_.GetPreparationId().c_str());

    Create(
        OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
        OperationPreparer_.GetAuth(),
        TTransactionId(),
        uniquePath,
        NT_FILE,
        TCreateOptions()
            .IgnoreExisting(true)
            .Recursive(true)
    .Attributes(TNode()("replication_factor", GetFileCacheReplicationFactor()))
    );

    {
        TFileWriter writer(
            uniquePath,
            OperationPreparer_.GetClientRetryPolicy(),
            OperationPreparer_.GetAuth(),
            TTransactionId(),
            TFileWriterOptions().ComputeMD5(true));
        itemToUpload.CreateInputStream()->ReadAll(writer);
        writer.Finish();
    }

    auto cachePath = PutFileToCache(
        retryPolicy,
        OperationPreparer_.GetAuth(),
        TTransactionId(),
        uniquePath,
        md5Signature,
        GetCachePath(),
        TPutFileToCacheOptions());

    Remove(
        OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
        OperationPreparer_.GetAuth(),
        TTransactionId(),
        uniquePath,
        TRemoveOptions().Force(true));

    LockedFileSignatures_.push_back(md5Signature);
    return cachePath;
}

TString TJobPreparer::UploadToCache(const IItemToUpload& itemToUpload) const
{
    LOG_INFO("Uploading file (FileName: %s; PreparationId: %s)",
        itemToUpload.GetDescription().c_str(),
        OperationPreparer_.GetPreparationId().c_str());

    TString result;
    switch (Options_.FileCacheMode_) {
        case TOperationOptions::EFileCacheMode::ApiCommandBased:
            Y_ENSURE_EX(Options_.FileStorageTransactionId_.IsEmpty(), TApiUsageError() <<
                "Default cache mode (API command-based) doesn't allow non-default 'FileStorageTransactionId_'");
            result = UploadToCacheUsingApi(itemToUpload);
            break;
        case TOperationOptions::EFileCacheMode::CachelessRandomPathUpload:
            result = UploadToRandomPath(itemToUpload);
            break;
        default:
            Y_FAIL("Unknown file cache mode: %d", static_cast<int>(Options_.FileCacheMode_));
    }

    LOG_INFO("Complete uploading file (FileName: %s; PreparationId: %s)",
        itemToUpload.GetDescription().c_str(),
        OperationPreparer_.GetPreparationId().c_str());

    return result;
}

void TJobPreparer::UseFileInCypress(const TRichYPath& file)
{
    if (!Exists(
        OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
        OperationPreparer_.GetAuth(),
        file.TransactionId_.GetOrElse(OperationPreparer_.GetTransactionId()),
        file.Path_))
    {
        ythrow yexception() << "File " << file.Path_ << " does not exist";
    }

    if (ShouldMountSandbox()) {
        auto size = Get(
            OperationPreparer_.GetClientRetryPolicy()->CreatePolicyForGenericRequest(),
            OperationPreparer_.GetAuth(),
            file.TransactionId_.GetOrElse(OperationPreparer_.GetTransactionId()),
            file.Path_ + "/@uncompressed_data_size")
            .AsInt64();

        TotalFileSize_ += RoundUpFileSize(static_cast<ui64>(size));
    }
    CypressFiles_.push_back(file);
}

void TJobPreparer::UploadLocalFile(
    const TLocalFilePath& localPath,
    const TAddLocalFileOptions& options,
    bool isApiFile)
{
    TFsPath fsPath(localPath);
    fsPath.CheckExists();

    TFileStat stat;
    fsPath.Stat(stat);

    bool isExecutable = stat.Mode & (S_IXUSR | S_IXGRP | S_IXOTH);
    auto cachePath = UploadToCache(TFileToUpload(localPath, options.MD5CheckSum_));

    TRichYPath cypressPath;
    if (isApiFile) {
        cypressPath = TConfig::Get()->ApiFilePathOptions;
    }
    cypressPath.Path(cachePath).FileName(options.PathInJob_.GetOrElse(fsPath.Basename()));
    if (isExecutable) {
        cypressPath.Executable(true);
    }
    if (options.BypassArtifactCache_) {
        cypressPath.BypassArtifactCache(*options.BypassArtifactCache_);
    }

    if (ShouldMountSandbox()) {
        TotalFileSize_ += RoundUpFileSize(stat.Size);
    }

    CachedFiles_.push_back(cypressPath);
}

void TJobPreparer::UploadBinary(const TJobBinaryConfig& jobBinary)
{
    if (std::holds_alternative<TJobBinaryLocalPath>(jobBinary)) {
        auto binaryLocalPath = std::get<TJobBinaryLocalPath>(jobBinary);
        auto opts = TAddLocalFileOptions().PathInJob("cppbinary");
        if (binaryLocalPath.MD5CheckSum) {
            opts.MD5CheckSum(*binaryLocalPath.MD5CheckSum);
        }
        UploadLocalFile(binaryLocalPath.Path, opts, /* isApiFile */ true);
    } else if (std::holds_alternative<TJobBinaryCypressPath>(jobBinary)) {
        auto binaryCypressPath = std::get<TJobBinaryCypressPath>(jobBinary);
        TRichYPath ytPath = TConfig::Get()->ApiFilePathOptions;
        ytPath.Path(binaryCypressPath.Path);
        if (binaryCypressPath.TransactionId) {
            ytPath.TransactionId(*binaryCypressPath.TransactionId);
        }
        UseFileInCypress(ytPath.FileName("cppbinary").Executable(true));
    } else {
        Y_FAIL("%s", (TStringBuilder() << "Unexpected jobBinary tag: " << jobBinary.index()).data());
    }
}

void TJobPreparer::UploadSmallFile(const TSmallJobFile& smallFile)
{
    auto cachePath = UploadToCache(TDataToUpload(smallFile.Data, smallFile.FileName + " [generated-file]"));
    auto path = TConfig::Get()->ApiFilePathOptions;
    CachedFiles_.push_back(path.Path(cachePath).FileName(smallFile.FileName));
    if (ShouldMountSandbox()) {
        TotalFileSize_ += RoundUpFileSize(smallFile.Data.size());
    }
}

bool TJobPreparer::IsLocalMode() const
{
    return UseLocalModeOptimization(OperationPreparer_.GetAuth(), OperationPreparer_.GetClientRetryPolicy());
}

void TJobPreparer::PrepareJobBinary(const IJob& job, int outputTableCount, bool hasState)
{
    auto jobBinary = TJobBinaryConfig();
    if (!std::holds_alternative<TJobBinaryDefault>(Spec_.GetJobBinary())) {
        jobBinary = Spec_.GetJobBinary();
    }
    TString binaryPathInsideJob;
    if (std::holds_alternative<TJobBinaryDefault>(jobBinary)) {
        if (GetInitStatus() != EInitStatus::FullInitialization) {
            ythrow yexception() << "NYT::Initialize() must be called prior to any operation";
        }

        const bool isLocalMode = IsLocalMode();
        const TMaybe<TString> md5 = !isLocalMode ? MakeMaybe(GetPersistentExecPathMd5()) : Nothing();
        jobBinary = TJobBinaryLocalPath{GetPersistentExecPath(), md5};

        if (isLocalMode) {
            binaryPathInsideJob = GetExecPath();
        }
    } else if (std::holds_alternative<TJobBinaryLocalPath>(jobBinary)) {
        const bool isLocalMode = IsLocalMode();
        if (isLocalMode) {
            binaryPathInsideJob = TFsPath(std::get<TJobBinaryLocalPath>(jobBinary).Path).RealPath();
        }
    }
    Y_ASSERT(!std::holds_alternative<TJobBinaryDefault>(jobBinary));

    // binaryPathInsideJob is only set when LocalModeOptimization option is on, so upload is not needed
    if (!binaryPathInsideJob) {
        binaryPathInsideJob = "./cppbinary";
        UploadBinary(jobBinary);
    }

    TString jobCommandPrefix = Options_.JobCommandPrefix_;
    if (!Spec_.JobCommandPrefix_.empty()) {
        jobCommandPrefix = Spec_.JobCommandPrefix_;
    }

    TString jobCommandSuffix = Options_.JobCommandSuffix_;
    if (!Spec_.JobCommandSuffix_.empty()) {
        jobCommandSuffix = Spec_.JobCommandSuffix_;
    }

    ClassName_ = TJobFactory::Get()->GetJobName(&job);
    Command_ = TStringBuilder() <<
        jobCommandPrefix <<
        (TConfig::Get()->UseClientProtobuf ? "YT_USE_CLIENT_PROTOBUF=1" : "YT_USE_CLIENT_PROTOBUF=0") << " " <<
        binaryPathInsideJob << " " <<
        // This argument has no meaning, but historically is checked in job initialization.
        "--yt-map " <<
        "\"" << ClassName_ << "\" " <<
        outputTableCount << " " <<
        hasState <<
        jobCommandSuffix;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDetail