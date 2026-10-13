/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.facebook.presto.nativetests;

import com.facebook.presto.Session;
import com.facebook.presto.testing.ExpectedQueryRunner;
import com.facebook.presto.testing.QueryRunner;
import com.facebook.presto.tests.AbstractTestQueryFramework;
import org.testng.annotations.AfterClass;
import org.testng.annotations.BeforeClass;
import org.testng.annotations.DataProvider;
import org.testng.annotations.Test;

import static com.facebook.presto.common.type.TimeZoneKey.getTimeZoneKey;
import static com.facebook.presto.nativeworker.PrestoNativeQueryRunnerUtils.javaHiveQueryRunnerBuilder;
import static java.lang.Boolean.parseBoolean;

public class TestDateTimeFunctions
        extends AbstractTestQueryFramework
{
    // A non-UTC session zone makes legacy and non-legacy results differ. It has no DST, so only
    // with_timezone itself, not the session-zone CAST, has to handle DST gaps and overlaps.
    private static final String SESSION_TIME_ZONE = "Asia/Kolkata";

    private String storageFormat;
    private boolean sidecarEnabled;

    @BeforeClass
    @Override
    public void init()
            throws Exception
    {
        storageFormat = System.getProperty("storageFormat", "PARQUET");
        sidecarEnabled = parseBoolean(System.getProperty("sidecarEnabled", "true"));
        super.init();
    }

    @Override
    protected void createTables()
    {
        QueryRunner queryRunner = (QueryRunner) getExpectedQueryRunner();

        // Timestamps are stored as VARCHAR and cast in each query, so the inputs do not depend on
        // how the file format stores TIMESTAMP. Reading from a table also keeps the coordinator
        // from constant folding with_timezone, so the native worker evaluates it.
        queryRunner.execute("DROP TABLE IF EXISTS with_timezone_input");
        queryRunner.execute("CREATE TABLE with_timezone_input (id INTEGER, ts VARCHAR, zone VARCHAR)");
        queryRunner.execute("INSERT INTO with_timezone_input VALUES " +
                "(1, '2020-01-01 10:00:00.123', 'America/New_York'), " +
                "(2, '2020-01-01 10:00:00.000', 'UTC'), " +
                "(3, '2020-01-01 10:00:00.000', '+05:30'), " +
                "(4, '2020-01-01 10:00:00.000', '-08:00'), " +
                // Wall time in the America/New_York spring-forward gap.
                "(5, '2020-03-08 02:30:00.000', 'America/New_York'), " +
                // Wall time in the America/New_York fall-back overlap.
                "(6, '2020-11-01 01:30:00.000', 'America/New_York'), " +
                // Before the epoch, so epoch millis are negative.
                "(7, '1969-12-31 23:59:59.999', 'Asia/Kathmandu'), " +
                "(8, NULL, 'UTC'), " +
                "(9, '2020-01-01 10:00:00.000', NULL)");
    }

    @Override
    protected QueryRunner createQueryRunner()
            throws Exception
    {
        return NativeTestsUtils.createNativeQueryRunner(storageFormat, sidecarEnabled);
    }

    @Override
    protected ExpectedQueryRunner createExpectedQueryRunner()
            throws Exception
    {
        return javaHiveQueryRunnerBuilder()
                .setStorageFormat(storageFormat)
                .setAddStorageFormatToPath(true)
                .build();
    }

    @DataProvider(name = "legacyTimestamp")
    public static Object[][] legacyTimestamp()
    {
        return new Object[][] {{true}, {false}};
    }

    @Test(dataProvider = "legacyTimestamp")
    public void testWithTimezone(boolean legacyTimestamp)
    {
        Session session = session(legacyTimestamp);
        // Zone from a column, resolved per row.
        assertQuery(session, "SELECT id, with_timezone(CAST(ts AS TIMESTAMP), zone) FROM with_timezone_input");
        // Constant zone, resolved once per query.
        assertQuery(session, "SELECT id, with_timezone(CAST(ts AS TIMESTAMP), 'America/New_York') FROM with_timezone_input");
    }

    @Test(dataProvider = "legacyTimestamp")
    public void testWithTimezoneComposition(boolean legacyTimestamp)
    {
        Session session = session(legacyTimestamp);
        // Results feed other date and time functions the same way on both engines.
        assertQuery(session, "SELECT id, " +
                "hour(with_timezone(CAST(ts AS TIMESTAMP), zone)), " +
                "CAST(with_timezone(CAST(ts AS TIMESTAMP), zone) AS VARCHAR), " +
                "with_timezone(CAST(ts AS TIMESTAMP), zone) AT TIME ZONE 'UTC' " +
                "FROM with_timezone_input");
        assertQuery(session, "SELECT zone, count(*) FROM with_timezone_input " +
                "WHERE with_timezone(CAST(ts AS TIMESTAMP), zone) > TIMESTAMP '2020-01-01 00:00:00 UTC' " +
                "GROUP BY zone");
    }

    @Test(dataProvider = "legacyTimestamp")
    public void testWithTimezoneInvalidZone(boolean legacyTimestamp)
    {
        Session session = session(legacyTimestamp);
        assertQueryFails(session,
                "SELECT with_timezone(CAST(ts AS TIMESTAMP), 'Not/A/Zone') FROM with_timezone_input",
                ".*Unknown time zone.*Not/A/Zone.*");
        assertQueryFails(session,
                "SELECT with_timezone(CAST(ts AS TIMESTAMP), concat(zone, '_invalid')) FROM with_timezone_input",
                ".*Unknown time zone.*_invalid.*");
    }

    @AfterClass(alwaysRun = true)
    public void cleanup()
    {
        QueryRunner queryRunner = (QueryRunner) getExpectedQueryRunner();
        queryRunner.execute("DROP TABLE IF EXISTS with_timezone_input");
    }

    private Session session(boolean legacyTimestamp)
    {
        return Session.builder(getSession())
                .setTimeZoneKey(getTimeZoneKey(SESSION_TIME_ZONE))
                .setSystemProperty("legacy_timestamp", String.valueOf(legacyTimestamp))
                .build();
    }
}
