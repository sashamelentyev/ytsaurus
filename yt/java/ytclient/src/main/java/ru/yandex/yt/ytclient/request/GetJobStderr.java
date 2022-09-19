package ru.yandex.yt.ytclient.request;

import java.util.Objects;

import javax.annotation.Nullable;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TReqGetJobStderr;
import ru.yandex.yt.ytclient.proxy.request.HighLevelRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcUtil;

@NonNullApi
@NonNullFields
public class GetJobStderr extends RequestBase implements HighLevelRequest<TReqGetJobStderr.Builder> {
    private final GUID operationId;
    private final GUID jobId;

    GetJobStderr(Builder builder) {
        super(builder);
        this.jobId = Objects.requireNonNull(builder.jobId);
        this.operationId = Objects.requireNonNull(builder.operationId);
    }

    public GetJobStderr(GUID operationId, GUID jobId) {
        this(builder().setOperationId(operationId).setJobId(jobId));
    }

    public static Builder builder() {
        return new Builder();
    }

    public Builder toBuilder() {
        Builder builder = builder()
                .setOperationId(operationId)
                .setJobId(jobId)
                .setTimeout(timeout)
                .setRequestId(requestId)
                .setUserAgent(userAgent)
                .setTraceId(traceId, traceSampled)
                .setAdditionalData(additionalData);
        return builder;
    }

    @Override
    public void writeTo(RpcClientRequestBuilder<TReqGetJobStderr.Builder, ?> requestBuilder) {
        requestBuilder.body()
                .setOperationId(RpcUtil.toProto(operationId))
                .setJobId(RpcUtil.toProto(jobId));
    }

    @Override
    protected void writeArgumentsLogString(StringBuilder sb) {
        sb.append("OperationId: ").append(operationId).append("; ");
        sb.append("JobId: ").append(jobId).append("; ");
        super.writeArgumentsLogString(sb);
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends RequestBase.Builder<Builder> {
        @Nullable
        private GUID operationId;
        @Nullable
        private GUID jobId;

        Builder() {
        }

        Builder setOperationId(GUID operationId) {
            this.operationId = operationId;
            return self();
        }

        Builder setJobId(GUID jobId) {
            this.jobId = jobId;
            return self();
        }

        @Override
        protected Builder self() {
            return this;
        }
    }
}