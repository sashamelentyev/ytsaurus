#include <mapreduce/yt/interface/client.h>

#include <library/cpp/string_utils/levenshtein_diff/levenshtein_diff.h>

#include <util/stream/output.h>
#include <util/system/user.h>

using namespace NYT;

class TFilterMapper
    : public IMapper<TTableReader<TNode>, TTableWriter<TNode>>
{
public:
    Y_SAVELOAD_JOB(Pattern_, MaxDistance_);

    static TIntrusivePtr<IMapper> FromNode(const TNode& node)
    {
        auto result = MakeIntrusive<TFilterMapper>();
        if (node.HasKey("pattern")) {
            result->Pattern_ = node["pattern"].AsString();
        }
        if (node.HasKey("max_distance")) {
            result->MaxDistance_ = node["max_distance"].AsInt64();
        }
        return result;
    }

    void Do(TReader* reader, TWriter* writer) override {
        for (auto& cursor : *reader) {
            const auto& row = cursor.GetRow();
            const auto& name = row["name"].AsString();
            if (NLevenshtein::Distance(name, Pattern_) <= MaxDistance_) {
                writer->AddRow(row);
            }
        }
    }

private:
    TString Pattern_;
    size_t MaxDistance_ = 0;
};
REGISTER_MAPPER(TFilterMapper);