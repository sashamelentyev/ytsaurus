from original_tests.yt.yt.tests.integration.tests.controller.test_merge_operation \
    import TestSchedulerMergeCommands as BaseTestMergeCommands


class TestMergeCommandsCompatUpToCA(BaseTestMergeCommands):
    ARTIFACT_COMPONENTS = {
        "21_3": ["master", "node", "job-proxy", "exec", "tools"],
        "trunk": ["scheduler", "controller-agent", "proxy", "http-proxy"],
    }