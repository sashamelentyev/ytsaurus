#include <yt/yt/tests/cpp/test_base/api_test_base.h>
#include <yt/yt/tests/cpp/test_base/private.h>

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/transaction.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/comparator.h>

#include <yt/yt/client/queue_client/partition_reader.h>
#include <yt/yt/client/queue_client/consumer_client.h>

#include <library/cpp/yt/string/format.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NCppTests {
namespace {

using namespace NApi;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NQueueClient;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TString MakeValueRow(const std::vector<TString>& values)
{
    TString result;
    for (int i = 0; i < std::ssize(values); ++i) {
        result += Format("%v<id=%v> %v;", (i == 0 ? "" : " "), i, values[i]);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

class TQueueTestBase
    : public TDynamicTablesTestBase
{
public:
    static void SetUpTestCase()
    {
        TDynamicTablesTestBase::SetUpTestCase();

        // TODO(achulkov2): Separate useful teardown methods from TDynamicTablesTestBase and stop inheriting from it altogether.
        CreateTable(
            "//tmp/fake", // tablePath
            "[" // schema
            "{name=key;type=uint64;sort_order=ascending};"
            "{name=value;type=uint64}]");
    }

    class TDynamicTable
    {
    public:
        explicit TDynamicTable(
            TYPath path,
            TTableSchemaPtr schema,
            const IAttributeDictionaryPtr& extraAttributes = CreateEphemeralAttributes())
            : Path_(std::move(path))
            , Schema_(std::move(schema))
        {
            TCreateNodeOptions options;
            options.Attributes = extraAttributes;
            options.Attributes->Set("dynamic", true);
            options.Attributes->Set("schema", Schema_);

            WaitFor(Client_->CreateNode(Path_, EObjectType::Table, options))
                .ThrowOnError();

            SyncMountTable(Path_);
        }

        ~TDynamicTable()
        {
            SyncUnmountTable(Path_);

            WaitFor(Client_->RemoveNode(Path_))
                .ThrowOnError();
        }

        const TTableSchemaPtr& GetSchema() const
        {
            return Schema_;
        }

        const TYPath& GetPath() const
        {
            return Path_;
        }

    private:
        TYPath Path_;
        TTableSchemaPtr Schema_;
    };

    static void WriteSharedRange(const TYPath& path, const TNameTablePtr& nameTable, const TSharedRange<TUnversionedRow>& range)
    {
        auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
            .ValueOrThrow();
        transaction->WriteRows(path, nameTable, range);

        WaitFor(transaction->Commit())
            .ThrowOnError();
    }

    static void WriteSingleRow(const TYPath& path, const TNameTablePtr& nameTable, const TUnversionedRow& row)
    {
        auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
            .ValueOrThrow();

        TUnversionedRowsBuilder rowsBuilder;
        rowsBuilder.AddRow(row);
        transaction->WriteRows(path, nameTable, rowsBuilder.Build());

        WaitFor(transaction->Commit())
            .ThrowOnError();
    }

    static void WriteSingleRow(const TYPath& path, const TNameTablePtr& nameTable, const std::vector<TString>& values)
    {
        auto owningRow = YsonToSchemalessRow(MakeValueRow(values));
        WriteSingleRow(path, nameTable, owningRow);
    }

    static void UnmountAndMount(const TYPath& path)
    {
        SyncUnmountTable(path);
        SyncMountTable(path);
    }

    static void WaitForRowCount(const TYPath& path, i64 rowCount)
    {
        WaitForPredicate([rowCount, path] {
            auto allRowsResult = WaitFor(Client_->SelectRows(Format("* from [%v]", path)))
                .ValueOrThrow();

            return std::ssize(allRowsResult.Rowset->GetRows()) == rowCount;
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_W(TQueueTestBase, Simple)
{
    auto queue = TDynamicTable("//tmp/queue_simple", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}));
    auto consumer = TDynamicTable("//tmp/consumer_simple", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"42u", "hello"});
    UnmountAndMount(queue.GetPath());

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    auto partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();

    auto queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    auto aColumnIndex = queueRowset->GetNameTable()->GetIdOrThrow("a");

    ASSERT_EQ(queueRowset->GetRows().size(), 1u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 0u);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 1u);
    EXPECT_EQ(queueRowset->GetRows()[0][aColumnIndex].Data.Uint64, 42u);

    auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    auto consumerClient = CreateConsumerClient(consumer.GetPath(), *consumer.GetSchema());
    auto partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].NextRowIndex, 1);
}

TEST_W(TQueueTestBase, HintBiggerThanMaxDataWeight)
{
    auto queue = TDynamicTable("//tmp/queue_hint_bigger_than_max_data_weight", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}));
    auto consumer = TDynamicTable("//tmp/consumer_hint_bigger_than_max_data_weight", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"42u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"43u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"44u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"45u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    UnmountAndMount(queue.GetPath());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"46u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"47u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"48u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"49u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    UnmountAndMount(queue.GetPath());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"50u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"51u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"52u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"53u", "longlonglonglonglonglonglonglonglonglonglonglong"});
    // No flush, just so some rows might be in the dynamic store.

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    // We expect to fetch only one row.
    partitionReaderConfig->MaxDataWeight = 10;
    auto partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();

    auto queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    auto aColumnIndex = queueRowset->GetNameTable()->GetIdOrThrow("a");

    ASSERT_EQ(queueRowset->GetRows().size(), 1u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 0u);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 1u);
    EXPECT_EQ(queueRowset->GetRows()[0][aColumnIndex].Data.Uint64, 42u);

    auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    auto consumerClient = CreateConsumerClient(consumer.GetPath(), *consumer.GetSchema());
    auto partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].NextRowIndex, 1);

    partitionReaderConfig = New<TPartitionReaderConfig>();
    // Now we should get more than one row.
    partitionReaderConfig->MaxDataWeight = 1_MB;
    partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();

    queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();

    ASSERT_GT(queueRowset->GetRows().size(), 1u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 1u);
    EXPECT_GT(queueRowset->GetFinishOffset(), 2u);
    EXPECT_EQ(queueRowset->GetRows()[0][aColumnIndex].Data.Uint64, 43u);

    transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_GT(partitions[0].NextRowIndex, 1);
}

TEST_W(TQueueTestBase, MultiplePartitions)
{
    auto queueAttributes = CreateEphemeralAttributes();
    queueAttributes->Set("tablet_count", 3);
    auto queue = TDynamicTable("//tmp/queue_multiple_partitions", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}), queueAttributes);
    auto consumer = TDynamicTable("//tmp/consumer_multiple_partitions", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema()->ToWrite());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "42u", "s"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "43u", "h"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "44u", "o"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "45u", "r"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "46u", "t"});

    TString veryLongString;
    for (int i = 0; i < 500; ++i) {
        veryLongString += "abacaba";
    }

    WriteSingleRow(queue.GetPath(), queueNameTable,{"1", "47u", veryLongString});
    WriteSingleRow(queue.GetPath(), queueNameTable,{"1", "48u", veryLongString});
    WriteSingleRow(queue.GetPath(), queueNameTable,{"1", "480u", veryLongString});
    WriteSingleRow(queue.GetPath(), queueNameTable,{"1", "481u", veryLongString});
    WriteSingleRow(queue.GetPath(), queueNameTable,{"1", "482u", veryLongString});

    WriteSingleRow(queue.GetPath(), queueNameTable, {"2", "49u", "hello"});

    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "50u", "s"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "51u", "t"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "52u", "r"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "53u", "i"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "54u", "n"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"0", "55u", "g"});

    WriteSingleRow(queue.GetPath(), queueNameTable, {"2", "56u", "darkness"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"2", "57u", "my"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"2", "58u", "old"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"2", "59u", "friend"});

    UnmountAndMount(queue.GetPath());

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    // Average data weight per row for all tablets is quite big due to large rows.
    // However, it should be computed per-tablet, and it is ~10 for partition 0.
    // So setting MaxDataWeight = 150 for partition 0 should read significantly more than one row.
    partitionReaderConfig->MaxDataWeight = 150;
    auto partitionReader0 = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader0->Open())
        .ThrowOnError();

    auto queueRowset0 = WaitFor(partitionReader0->Read())
        .ValueOrThrow();
    auto aColumnIndex = queueRowset0->GetNameTable()->GetIdOrThrow("a");

    ASSERT_GT(queueRowset0->GetRows().size(), 5u);
    EXPECT_EQ(queueRowset0->GetStartOffset(), 0u);
    EXPECT_GT(queueRowset0->GetFinishOffset(), 5u);
    EXPECT_EQ(queueRowset0->GetRows()[0][aColumnIndex].Data.Uint64, 42u);

    auto partitionReaderConfig1 = New<TPartitionReaderConfig>();
    // Setting MaxDataWeight = 150 for partition 1 should read only one row.
    partitionReaderConfig1->MaxDataWeight = 150;
    auto partitionReader1 = CreatePartitionReader(partitionReaderConfig1, Client_, consumer.GetPath(), /*partitionIndex*/ 1);
    WaitFor(partitionReader1->Open())
        .ThrowOnError();

    auto queueRowset1 = WaitFor(partitionReader1->Read())
        .ValueOrThrow();

    ASSERT_EQ(queueRowset1->GetRows().size(), 1u);
    EXPECT_EQ(queueRowset1->GetStartOffset(), 0u);
    EXPECT_EQ(queueRowset1->GetFinishOffset(), 1u);
    EXPECT_EQ(queueRowset1->GetRows()[0][aColumnIndex].Data.Uint64, 47u);

    auto partitionReaderConfig2 = New<TPartitionReaderConfig>();
    auto partitionReader2 = CreatePartitionReader(partitionReaderConfig2, Client_, consumer.GetPath(), /*partitionIndex*/ 2);
    WaitFor(partitionReader2->Open())
        .ThrowOnError();

    auto queueRowset2 = WaitFor(partitionReader2->Read())
        .ValueOrThrow();

    ASSERT_EQ(queueRowset2->GetRows().size(), 5u);
    EXPECT_EQ(queueRowset2->GetStartOffset(), 0u);
    EXPECT_EQ(queueRowset2->GetFinishOffset(), 5u);
    EXPECT_EQ(queueRowset2->GetRows()[0][aColumnIndex].Data.Uint64, 49u);

    auto transaction02 = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset0->Commit(transaction02);
    queueRowset2->Commit(transaction02);
    WaitFor(transaction02->Commit())
        .ThrowOnError();

    auto transaction1 = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset1->Commit(transaction1);
    WaitFor(transaction1->Commit())
        .ThrowOnError();

    auto consumerClient = CreateConsumerClient(consumer.GetPath(), *consumer.GetSchema());
    auto partitions = WaitFor(consumerClient->CollectPartitions(Client_, 3))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 3u);
    EXPECT_GT(partitions[0].NextRowIndex, 5);
    EXPECT_EQ(partitions[1].NextRowIndex, 1);
    EXPECT_EQ(partitions[2].NextRowIndex, 5);
}

TEST_W(TQueueTestBase, BatchSizesAreReasonable)
{
    auto queue = TDynamicTable("//tmp/queue_batch_sizes_are_reasonable", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}));
    auto consumer = TDynamicTable("//tmp/consumer_batch_sizes_are_reasonable", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema());

    std::unique_ptr<TUnversionedRowsBuilder> rowsBuilder = std::make_unique<TUnversionedRowsBuilder>();
    int rowCount = 100'000;
    for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        TUnversionedRowBuilder rowBuilder;

        rowBuilder.AddValue(MakeUnversionedUint64Value(rowIndex, 0));

        TString value;
        value.assign("a", 1_KB + RandomNumber(24u));
        rowBuilder.AddValue(MakeUnversionedStringValue(value, 1));

        rowsBuilder->AddRow(rowBuilder.GetRow());

        if (RandomNumber(2000u) == 0 || rowIndex + 1 == rowCount) {
            WriteSharedRange(queue.GetPath(), queueNameTable, rowsBuilder->Build());
            rowsBuilder = std::make_unique<TUnversionedRowsBuilder>();
        }

        if (RandomNumber(3000u) == 0) {
            UnmountAndMount(queue.GetPath());
        }
    }

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    partitionReaderConfig->MaxRowCount = 5000;
    // We expect to fetch only one row.
    partitionReaderConfig->MaxDataWeight = 1_MB;
    auto partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();


    i64 rowsRead = 0;
    int badBatches = 0;

    while (true) {
        auto queueRowset = WaitFor(partitionReader->Read())
            .ValueOrThrow();
        auto aColumnIndex = queueRowset->GetNameTable()->GetIdOrThrow("a");

        if (queueRowset->GetRows().empty()) {
            break;
        }

        EXPECT_EQ(queueRowset->GetRows()[0][aColumnIndex].Data.Uint64, static_cast<ui64>(rowsRead));
        ASSERT_EQ(queueRowset->GetStartOffset(), rowsRead);
        auto batchDataWeight = GetDataWeight(queueRowset->GetRows());
        if (batchDataWeight < 1_MB / 2 || batchDataWeight > 3_MB / 2) {
            ++badBatches;
        }

        auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
            .ValueOrThrow();
        queueRowset->Commit(transaction);
        WaitFor(transaction->Commit())
            .ThrowOnError();

        rowsRead += std::ssize(queueRowset->GetRows());
    }

    // Account for the potentially small last batch.
    EXPECT_LE(badBatches, 1);
    EXPECT_EQ(rowsRead, rowCount);
}

TEST_W(TQueueTestBase, ReaderCatchingUp)
{
    auto queue = TDynamicTable("//tmp/queue_reader_catching_up", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}));
    auto consumer = TDynamicTable("//tmp/consumer_reader_catching_up", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"42u", "hello"});
    UnmountAndMount(queue.GetPath());

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    auto partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();

    WriteSingleRow(queue.GetPath(), queueNameTable, {"43u", "darkness"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"44u", "my"});
    UnmountAndMount(queue.GetPath());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"45u", "old"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"46u", "friend"});

    Client_->TrimTable(queue.GetPath(), 0, 2);
    WaitForRowCount(queue.GetPath(), 3);

    auto queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    auto aColumnIndex = queueRowset->GetNameTable()->GetIdOrThrow("a");

    ASSERT_EQ(queueRowset->GetRows().size(), 3u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 2u);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 5u);
    EXPECT_EQ(queueRowset->GetRows()[0][aColumnIndex].Data.Uint64, 44u);

    auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    auto consumerClient = CreateConsumerClient(consumer.GetPath(), *consumer.GetSchema());
    auto partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].NextRowIndex, 5);
}

TEST_W(TQueueTestBase, EmptyQueue)
{
    auto queue = TDynamicTable("//tmp/queue_empty_queue", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("a", EValueType::Uint64),
        TColumnSchema("b", EValueType::String)}));
    auto consumer = TDynamicTable("//tmp/consumer_empty_queue", New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("ShardId", EValueType::Uint64, ESortOrder::Ascending),
        TColumnSchema("Offset", EValueType::Uint64),
    }, /*strict*/ true, /*uniqueKeys*/ true));
    WaitFor(Client_->SetNode(consumer.GetPath() + "/@target_queue", ConvertToYsonString("primary:" + queue.GetPath())))
        .ThrowOnError();

    auto partitionReaderConfig = New<TPartitionReaderConfig>();
    auto partitionReader = CreatePartitionReader(partitionReaderConfig, Client_, consumer.GetPath(), /*partitionIndex*/ 0);

    WaitFor(partitionReader->Open())
        .ThrowOnError();

    auto queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    ASSERT_EQ(queueRowset->GetRows().size(), 0u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 0);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 0);

    auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    auto consumerClient = CreateConsumerClient(consumer.GetPath(), *consumer.GetSchema());
    auto partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].NextRowIndex, 0);

    auto queueNameTable = TNameTable::FromSchema(*queue.GetSchema());

    WriteSingleRow(queue.GetPath(), queueNameTable, {"43u", "darkness"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"44u", "my"});
    UnmountAndMount(queue.GetPath());

    Client_->TrimTable(queue.GetPath(), 0, 2);
    WaitForRowCount(queue.GetPath(), 0);

    queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    ASSERT_EQ(queueRowset->GetRows().size(), 0u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 0);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 0);

    WriteSingleRow(queue.GetPath(), queueNameTable, {"45u", "darkness"});
    WriteSingleRow(queue.GetPath(), queueNameTable, {"46u", "my"});

    queueRowset = WaitFor(partitionReader->Read())
        .ValueOrThrow();
    ASSERT_EQ(queueRowset->GetRows().size(), 2u);
    EXPECT_EQ(queueRowset->GetStartOffset(), 2);
    EXPECT_EQ(queueRowset->GetFinishOffset(), 4);

    transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
        .ValueOrThrow();
    queueRowset->Commit(transaction);
    WaitFor(transaction->Commit())
        .ThrowOnError();

    partitions = WaitFor(consumerClient->CollectPartitions(Client_, 1))
        .ValueOrThrow();

    ASSERT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].NextRowIndex, 4);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NCppTests