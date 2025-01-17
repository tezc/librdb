#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "test_common.h"

void dummyLogger(RdbLogLevel l, const char *msg) { UNUSED(l, msg); }

static int setupTest(void **state) {
    UNUSED(state);
    sendRedisCmd("FLUSHALL", REDIS_REPLY_STATUS, NULL);
    sendRedisCmd("SAVE", REDIS_REPLY_STATUS, NULL);
    return 0;
}

void rdb_to_tcp(const char *rdbfile, int pipelineDepth, int isRestore, char *respFileName) {
    RdbxRespToRedisLoader *r2r;
    RdbxToResp *rdbToResp1, *rdbToResp2;
    RdbStatus status;

    RdbxToRespConf rdb2respConf = {
        .supportRestore = isRestore,
        .dstRedisVersion = getTargetRedisVersion(NULL, NULL),
        .supportRestoreModuleAux = isSupportRestoreModuleAux()
    };

    RdbParser *parser = RDB_createParserRdb(NULL);
    RDB_setLogLevel(parser, RDB_LOG_ERR);
    assert_non_null(RDBX_createReaderFile(parser, rdbfile));
    assert_non_null(rdbToResp1 = RDBX_createHandlersToResp(parser, &rdb2respConf));
    assert_non_null(r2r = RDBX_createRespToRedisTcp(parser, rdbToResp1, NULL, "127.0.0.1", getRedisPort()));
    if (respFileName) {
        assert_non_null(rdbToResp2 = RDBX_createHandlersToResp(parser, &rdb2respConf));
        assert_non_null(RDBX_createRespToFileWriter(parser, rdbToResp2, respFileName));
    }
    RDBX_setPipelineDepth(r2r, pipelineDepth);
    RDB_setLogLevel(parser, RDB_LOG_ERR);
    while ((status = RDB_parse(parser)) == RDB_STATUS_WAIT_MORE_DATA);
    assert_int_equal(status, RDB_STATUS_OK);
    RDB_deleteParser(parser);
}

static void rdb_to_json(const char *rdbfile, const char *outfile) {
    RdbStatus status;
    RdbxToJsonConf rdb2jsonConf = {
            .level = RDB_LEVEL_DATA,
            .encoding = RDBX_CONV_JSON_ENC_PLAIN,
            .includeAuxField = 0,
            .includeFunc = 0,
            .flatten = 1,
            .includeStreamMeta = 0,
    };

    RdbParser *parser = RDB_createParserRdb(NULL);
    RDB_setLogLevel(parser, RDB_LOG_ERR);
    assert_non_null(RDBX_createReaderFile(parser, rdbfile));
    assert_non_null(RDBX_createHandlersToJson(parser, outfile, &rdb2jsonConf));
    while ((status = RDB_parse(parser)) == RDB_STATUS_WAIT_MORE_DATA);
    assert_int_equal(status, RDB_STATUS_OK);
    RDB_deleteParser(parser);
}

/*
 * Testing RESP against live server:
 * 1. Convert RDB to Json (out1.json)
 * 2. Upload RDB against Redis and save DUMP-RDB
 * 3. From DUMP-RDB generate Json (out2.json)
 * 4. assert_json_equal(out1.json , out2.json)
 *
 * The test will run twice against:
 * A. old Redis target (no RESTORE)
 * B. new Redis target (RESTORE)
 *
 * Note: This test cannot tell if the parser really run RESTORE command in
 * the background. test_rdb_to_resp.c verifies that RESTORE command is used
 * only when it should.
 */
static void test_rdb_to_redis_common(const char *rdbfile, int ignoreListOrder, char *expRespCmd, const char *expJsonFile) {

    /* test one time without RESTORE, Playing against old version.
     * and one time with RESTORE, Playing against new version. */
    for (int isRestore = 0 ; isRestore <= 1 ; ++isRestore) {

        sendRedisCmd("FLUSHALL", REDIS_REPLY_STATUS, NULL);

        /* 1. Convert RDB to Json (out1.json) */
        rdb_to_json(rdbfile, TMP_FOLDER("out1.json"));

        /* 2. Upload RDB against Redis and save DUMP-RDB */
        rdb_to_tcp(rdbfile, 1, isRestore, TMP_FOLDER("cmd.resp"));
        sendRedisCmd("SAVE", REDIS_REPLY_STATUS, NULL);

        if (expRespCmd && !isRestore) {
            /* Verify corresponding RESP commands includes `expRespCmd` */
             assert_file_payload(TMP_FOLDER("cmd.resp"),
                                 expRespCmd,
                                 strlen(expRespCmd),
                                 M_SUBSTR, 1);
        }

        /* 3. From DUMP-RDB generate Json (out2.json) */
        rdb_to_json(TMP_FOLDER("dump.rdb"), TMP_FOLDER("out2.json"));

        /* 4. Verify that dumped RDB and converted to json is as expected  */
        if (expJsonFile)
            assert_json_equal(expJsonFile, TMP_FOLDER("out2.json"), 0);
        else
            assert_json_equal(TMP_FOLDER("out1.json"), TMP_FOLDER("out2.json"), ignoreListOrder);
    }
}

static void test_rdb_to_redis_single_string(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("single_key.rdb"), 0, "SET", NULL);
}

static void test_rdb_to_redis_single_list(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("quicklist2_v11.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_multiple_lists_strings(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("multiple_lists_strings.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_multiple_lists_strings_pipeline_depth_1(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("multiple_lists_strings.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_plain_list(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("plain_list_v6.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_quicklist(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("quicklist.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_single_ziplist(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("ziplist_v3.rdb"), 0, "$5\r\nRPUSH", NULL);
}

static void test_rdb_to_redis_plain_hash(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("plain_hash_v3.rdb"), 0, "$4\r\nHSET", NULL);
}

static void test_rdb_to_redis_hash_zl(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("hash_zl_v6.rdb"), 0, "$4\r\nHSET", NULL);
}

static void test_rdb_to_redis_hash_lp(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("hash_lp_v11.rdb"), 0, "$4\r\nHSET", NULL);
}

static void test_rdb_to_redis_hash_zm(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("hash_zm_v2.rdb"), 0, "$4\r\nHSET", NULL);
}

static void test_rdb_to_redis_plain_set(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("plain_set_v6.rdb"), 1, "$4\r\nSADD", NULL);
}

static void test_rdb_to_redis_set_is(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("set_is_v11.rdb"), 1, "$4\r\nSADD", NULL);
}

static void test_rdb_to_redis_set_lp(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("set_lp_v11.rdb"), 1, "$4\r\nSADD", NULL);
}

static void test_rdb_to_redis_plain_zset(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("plain_zset_v6.rdb"), 1, "$4\r\nZADD", NULL);
}

static void test_rdb_to_redis_plain_zset_2(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("plain_zset_2_v11.rdb"), 1, "$4\r\nZADD", NULL);
}

static void test_rdb_to_redis_zset_lp(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("zset_lp_v11.rdb"), 1, "$4\r\nZADD", NULL);
}

static void test_rdb_to_redis_zset_zl(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("zset_zl_v6.rdb"), 1, "$4\r\nZADD", NULL);
}

static void test_rdb_to_redis_multiple_dbs(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("multiple_dbs.rdb"), 1, NULL, NULL);
}

static void test_rdb_to_redis_set_expired(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("set_expired_v11.rdb"), 1, "$9\r\nPEXPIREAT",
                             DUMP_FOLDER("set_expired.json"));
}

static void test_rdb_to_redis_set_not_expired(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("set_not_expired_v11.rdb"), 1,"$9\r\nPEXPIREAT",
                             DUMP_FOLDER("set_not_expired.json"));
}

static void test_rdb_to_redis_policy_lfu(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("mem_policy_lfu.rdb"), 1, NULL,
                             DUMP_FOLDER("mem_policy_lfu.json"));
}

static void test_rdb_to_redis_policy_lru(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("mem_policy_lru.rdb"), 1, NULL,
                             DUMP_FOLDER("mem_policy_lru.json"));
}

static void test_rdb_to_redis_function(void **state) {
    UNUSED(state);
    int major;
    getTargetRedisVersion(&major, NULL);
    /* function available since 7.0 */
    if (major < 7)
        skip();
    test_rdb_to_redis_common(DUMP_FOLDER("function.rdb"), 1, NULL, NULL);
}

/* test relied on rdbtest module within redis repo, if available */
void test_rdb_to_redis_module(void **state) {
    UNUSED(state);

    /* Skip test if testrdb is not loaded */
    if (! strstr( sendRedisCmd("MODULE LIST", REDIS_REPLY_ARRAY, NULL), "testrdb" ) )
        skip();

    /* 1. Apply testrdb SET module command on Redis */
    sendRedisCmd("testrdb.set.key key1 value1", REDIS_REPLY_INTEGER, NULL);
    sendRedisCmd("testrdb.set.key 123456 7890", REDIS_REPLY_INTEGER, NULL);

    /* 2. Save rdb aside */
    sendRedisCmd("SAVE", REDIS_REPLY_STATUS, NULL);
    runSystemCmd("cp %s %s > /dev/null", TMP_FOLDER("dump.rdb"), TMP_FOLDER("test_rdb_to_redis_module.rdb"));

    /* 3. Flushall Redis database */
    sendRedisCmd("FLUSHALL", REDIS_REPLY_STATUS, NULL);

    /* 4. Run the parser against Redis and also let it output RESP to a file */
    rdb_to_tcp(TMP_FOLDER("test_rdb_to_redis_module.rdb"), 1, 1, TMP_FOLDER("rdb_to_tcp.resp"));

    /* 5. Verify resp file contains "RESTOREMODAUX & RESTORE key1" */
    assert_file_payload(TMP_FOLDER("rdb_to_tcp.resp"), STR_AND_SIZE("RESTORE\r\n$4\r\nkey1"), M_SUBSTR, 1);
    if (isSupportRestoreModuleAux())
        assert_file_payload(TMP_FOLDER("rdb_to_tcp.resp"), STR_AND_SIZE("RESTOREMODAUX"), M_SUBSTR, 1);


    /* 5. Now, verify testrdb GET module command returns expected values from Redis */
    sendRedisCmd("testrdb.get.key key1", REDIS_REPLY_STRING, "value1");
    sendRedisCmd("testrdb.get.key 123456", REDIS_REPLY_STRING, "7890");
}

void test_rdb_to_redis_stream(void **state) {
    UNUSED(state);
    test_rdb_to_redis_common(DUMP_FOLDER("stream_v11.rdb"), 1, NULL, NULL);
}

/* iff 'delKeyBeforeWrite' is not set, then the parser will return an error on
 * loading 100_lists.rdb ("mylist1 mylist2 ... mylist100") on key 'mylist62'
 * Because key `mylist62` created earlier with a string value.  */
static void test_rdb_to_redis_del_before_write(void **state) {
    UNUSED(state);
    RdbParser *parser;
    RdbStatus status;
    for (int delKeyBeforeWrite = 0 ; delKeyBeforeWrite <= 1 ; ++delKeyBeforeWrite) {
        RdbxToRespConf rdb2respConf = {
                .delKeyBeforeWrite = delKeyBeforeWrite,
                .supportRestore = 1,
                .dstRedisVersion = getTargetRedisVersion(NULL, NULL),
        };

        /* create key `mylist62` that goanna appear as well in the RDB file */
        sendRedisCmd("set mylist62 1", REDIS_REPLY_STATUS, NULL);

        /* RDB to TCP */
        RdbxToResp *rdbToResp;
        parser = RDB_createParserRdb(NULL);

        /* set dummy logger. Goanna have expected error */
        RDB_setLogger(parser, dummyLogger);

        assert_non_null(RDBX_createReaderFile(parser, DUMP_FOLDER("100_lists.rdb")));
        assert_non_null(rdbToResp = RDBX_createHandlersToResp(parser, &rdb2respConf));

        assert_non_null(RDBX_createRespToRedisTcp(parser,
                                                  rdbToResp,
                                                  NULL,
                                                  "127.0.0.1",
                                                  getRedisPort()));

        while ((status = RDB_parse(parser)) == RDB_STATUS_WAIT_MORE_DATA);

        if (status == RDB_STATUS_OK)
            assert_int_equal(delKeyBeforeWrite, 1);
        else {
            assert_int_equal(delKeyBeforeWrite, 0);
            /* verify returned error code. Verify error message. */
            RdbRes err = RDB_getErrorCode(parser);
            assert_int_equal(err, RDBX_ERR_RESP_WRITE);
            assert_non_null(strstr(RDB_getErrorMessage(parser), "mylist62"));
        }

        RDB_deleteParser(parser);
    }
}

/*************************** group_rdb_to_redis *******************************/
int group_rdb_to_redis(void) {

    if (!isSetRedisServer()) {
        printf("[  SKIPPED ] (Redis installation folder is not configured)\n");
        return 0;
    }

    const struct CMUnitTest tests[] = {
            /* string */
            cmocka_unit_test_setup(test_rdb_to_redis_single_string, setupTest),
            /* list */
            cmocka_unit_test_setup(test_rdb_to_redis_single_list, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_plain_list, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_quicklist, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_single_ziplist, setupTest),
            /* hash */
            cmocka_unit_test_setup(test_rdb_to_redis_plain_hash, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_hash_zl, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_hash_lp, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_hash_zm, setupTest),
            /* set */
            cmocka_unit_test_setup(test_rdb_to_redis_plain_set, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_set_is, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_set_lp, setupTest),
            /* zset */
            cmocka_unit_test_setup(test_rdb_to_redis_plain_zset, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_plain_zset_2, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_zset_lp, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_zset_zl, setupTest),

            /* module */
            cmocka_unit_test_setup(test_rdb_to_redis_module, setupTest),

            /* stream */
            cmocka_unit_test_setup(test_rdb_to_redis_stream, setupTest),

            /* expired keys */
            cmocka_unit_test_setup(test_rdb_to_redis_set_expired, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_set_not_expired, setupTest),

            /* mem policy */
            cmocka_unit_test_setup(test_rdb_to_redis_policy_lfu, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_policy_lru, setupTest),

            /* misc */
            cmocka_unit_test_setup(test_rdb_to_redis_multiple_lists_strings, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_multiple_lists_strings_pipeline_depth_1, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_del_before_write, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_multiple_dbs, setupTest),
            cmocka_unit_test_setup(test_rdb_to_redis_function, setupTest),
    };

    int res = cmocka_run_group_tests(tests, NULL, NULL);
    return res;
}
