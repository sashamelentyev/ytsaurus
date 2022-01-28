#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/thread_pool.h>

namespace NYT {
namespace {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TEST(TestCancelableRunWithBoundedConcurrency, TestSimple)
{
    int x = 0;

    auto future = CancelableRunWithBoundedConcurrency<void>(
        {
            BIND([&] {
                ++x;
                return VoidFuture;
            })
        },
        /*concurrencyLimit*/ 1);
    WaitFor(future)
        .ThrowOnError();

    EXPECT_EQ(x, 1);
}

TEST(TestCancelableRunWithBoundedConcurrency, TestManyCallbacks)
{
    auto threadPool = New<TThreadPool>(4, "ThreadPool");

    std::atomic<int> x = 0;

    const int callbackCount = 10000;
    std::vector<TCallback<TFuture<void>()>> callbacks;
    callbacks.reserve(callbackCount);
    for (int i = 0; i < callbackCount; ++i) {
        callbacks.push_back(BIND([&] {
            ++x;
        })
        .AsyncVia(threadPool->GetInvoker()));
    }

    auto future = CancelableRunWithBoundedConcurrency(
        std::move(callbacks),
        /*concurrencyLimit*/ 10);
    WaitFor(future)
        .ThrowOnError();

    EXPECT_EQ(x, callbackCount);
}

TEST(TestCancelableRunWithBoundedConcurrency, TestCancelation)
{
    auto threadPool = New<TThreadPool>(4, "ThreadPool");

    std::atomic<int> x = 0;
    std::atomic<int> canceledCount = 0;

    std::vector<TCallback<TFuture<void>()>> callbacks;
    for (int i = 0; i < 20; ++i) {
        callbacks.push_back(BIND([&] {
            if (x++ < 5) {
                return VoidFuture;
            }

            auto promise = NewPromise<void>();
            promise.OnCanceled(BIND([&, promise] (const TError& /*error*/) {
                ++canceledCount;
            }));

            return promise.ToFuture();
        }));
    }

    auto future = CancelableRunWithBoundedConcurrency<void>(
        std::move(callbacks),
        /*concurrencyLimit*/ 5);

    while (x < 10) {
        Sleep(TDuration::MilliSeconds(10));
    }

    future.Cancel(TError("Canceled"));

    EXPECT_EQ(x, 10);
    EXPECT_EQ(canceledCount, 5);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT