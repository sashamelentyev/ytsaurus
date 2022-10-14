package ru.yandex.yt.ytclient.request;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.operations.SortSpec;

public class SortOperation extends BaseOperation<SortSpec> {
    SortOperation(Builder builder) {
        super(builder);
    }

    public Builder toBuilder() {
        return builder()
                .setSpec(getSpec())
                .setMutatingOptions(getMutatingOptions())
                .setTransactionalOptions(getTransactionalOptions().orElse(null));
    }

    public static Builder builder() {
        return new Builder();
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends BuilderBase<Builder, SortSpec> {
        public SortOperation build() {
            return new SortOperation(this);
        }

        protected Builder self() {
            return this;
        }
    }
}