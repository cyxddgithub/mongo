/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/thread.hpp>
#include <string>

#include "mongo/db/field_parser.h"
#include "mongo/db/range_deleter.h"
#include "mongo/db/range_deleter_mock_env.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/repl_coordinator_mock.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;

    using mongo::BSONObj;
    using mongo::CursorId;
    using mongo::DeletedRange;
    using mongo::FieldParser;
    using mongo::KeyRange;
    using mongo::Notification;
    using mongo::RangeDeleter;
    using mongo::RangeDeleterMockEnv;
    using mongo::RangeDeleterOptions;
    using mongo::OperationContext;

    OperationContext* const noTxn = NULL; // MockEnv doesn't need txn XXX SERVER-13931

    // Capped sleep interval is 640 mSec, Nyquist frequency is 1280 mSec => round up to 2 sec.
    const int MAX_IMMEDIATE_DELETE_WAIT_SECS = 2;

    const mongo::repl::ReplSettings replSettings;

    // Should not be able to queue deletes if deleter workers were not started.
    TEST(QueueDelete, CantAfterStop) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        deleter.startWorkers();
        deleter.stopWorkers();

        string errMsg;
        ASSERT_FALSE(deleter.queueDelete(RangeDeleterOptions(KeyRange("test.user",
                                                                      BSON("x" << 120),
                                                                      BSON("x" << 200),
                                                                      BSON("x" << 1))),
                                         NULL /* notifier not needed */,
                                         &errMsg));
        ASSERT_FALSE(errMsg.empty());
        ASSERT_FALSE(env->deleteOccured());
    }

    // Should not start delete if the set of cursors that were open when the
    // delete was queued is still open.
    TEST(QueuedDelete, ShouldWaitCursor) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        const string ns("test.user");
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        env->addCursorId(ns, 345);

        Notification notifyDone;
        RangeDeleterOptions deleterOptions(KeyRange(ns,
                                                    BSON("x" << 0),
                                                    BSON("x" << 10),
                                                    BSON("x" << 1)));
        deleterOptions.waitForOpenCursors = true;

        ASSERT_TRUE(deleter.queueDelete(deleterOptions,
                                        &notifyDone,
                                        NULL /* errMsg not needed */));

        env->waitForNthGetCursor(1u);

        ASSERT_EQUALS(1U, deleter.getPendingDeletes());
        ASSERT_FALSE(env->deleteOccured());

        // Set the open cursors to a totally different sets of cursorIDs.
        env->addCursorId(ns, 200);
        env->removeCursorId(ns, 345);

        notifyDone.waitToBeNotified();

        ASSERT_TRUE(env->deleteOccured());
        const DeletedRange deletedChunk(env->getLastDelete());

        ASSERT_EQUALS(ns, deletedChunk.ns);
        ASSERT_TRUE(deletedChunk.min.equal(BSON("x" << 0)));
        ASSERT_TRUE(deletedChunk.max.equal(BSON("x" << 10)));

        deleter.stopWorkers();
    }

    // Should terminate when stop is requested.
    TEST(QueuedDelete, StopWhileWaitingCursor) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        const string ns("test.user");
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        env->addCursorId(ns, 345);

        Notification notifyDone;
        RangeDeleterOptions deleterOptions(KeyRange(ns,
                                                    BSON("x" << 0),
                                                    BSON("x" << 10),
                                                    BSON("x" << 1)));
        deleterOptions.waitForOpenCursors = true;
        ASSERT_TRUE(deleter.queueDelete(deleterOptions,
                                        &notifyDone,
                                        NULL /* errMsg not needed */));


        env->waitForNthGetCursor(1u);

        deleter.stopWorkers();
        ASSERT_FALSE(env->deleteOccured());
    }

    static void rangeDeleterDeleteNow(RangeDeleter* deleter,
                                      OperationContext* txn,
                                      const RangeDeleterOptions& deleterOptions,
                                      std::string* errMsg) {
        deleter->deleteNow(txn, deleterOptions, errMsg);
    }

    // Should not start delete if the set of cursors that were open when the
    // deleteNow method is called is still open.
    TEST(ImmediateDelete, ShouldWaitCursor) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        const string ns("test.user");
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        env->addCursorId(ns, 345);

        string errMsg;
        RangeDeleterOptions deleterOption(KeyRange(ns,
                                                   BSON("x" << 0),
                                                   BSON("x" << 10),
                                                   BSON("x" << 1)));
        deleterOption.waitForOpenCursors = true;
        boost::thread deleterThread = boost::thread(mongo::stdx::bind(
                                                            rangeDeleterDeleteNow,
                                                            &deleter,
                                                            noTxn,
                                                            deleterOption,
                                                            &errMsg));

        env->waitForNthGetCursor(1u);

        // Note: immediate deletes has no pending state, it goes directly to inProgress
        // even while waiting for cursors.
        ASSERT_EQUALS(1U, deleter.getDeletesInProgress());

        ASSERT_FALSE(env->deleteOccured());

        // Set the open cursors to a totally different sets of cursorIDs.
        env->addCursorId(ns, 200);
        env->removeCursorId(ns, 345);

        ASSERT_TRUE(deleterThread.timed_join(
                boost::posix_time::seconds(MAX_IMMEDIATE_DELETE_WAIT_SECS)));

        ASSERT_TRUE(env->deleteOccured());
        const DeletedRange deletedChunk(env->getLastDelete());

        ASSERT_EQUALS(ns, deletedChunk.ns);
        ASSERT_TRUE(deletedChunk.min.equal(BSON("x" << 0)));
        ASSERT_TRUE(deletedChunk.max.equal(BSON("x" << 10)));
        ASSERT_TRUE(deletedChunk.shardKeyPattern.equal(BSON("x" << 1)));

        deleter.stopWorkers();
    }

    // Should terminate when stop is requested.
    TEST(ImmediateDelete, StopWhileWaitingCursor) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        const string ns("test.user");
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        env->addCursorId(ns, 345);

        string errMsg;
        RangeDeleterOptions deleterOption(KeyRange(ns,
                                                   BSON("x" << 0),
                                                   BSON("x" << 10),
                                                   BSON("x" << 1)));
        deleterOption.waitForOpenCursors = true;
        boost::thread deleterThread = boost::thread(mongo::stdx::bind(
                                                            rangeDeleterDeleteNow,
                                                            &deleter,
                                                            noTxn,
                                                            deleterOption,
                                                            &errMsg));

        env->waitForNthGetCursor(1u);

        // Note: immediate deletes has no pending state, it goes directly to inProgress
        // even while waiting for cursors.
        ASSERT_EQUALS(1U, deleter.getDeletesInProgress());

        ASSERT_FALSE(env->deleteOccured());

        deleter.stopWorkers();

        ASSERT_TRUE(deleterThread.timed_join(
                boost::posix_time::seconds(MAX_IMMEDIATE_DELETE_WAIT_SECS)));

        ASSERT_FALSE(env->deleteOccured());
    }

    // Tests the interaction of multiple deletes queued with different states.
    // Starts by adding a new delete task, waits for the worker to work on it,
    // and then adds 2 more task, one of which is ready to be deleted, while the
    // other one is waiting for an open cursor. The test then makes sure that the
    // deletes are performed in the right order.
    TEST(MixedDeletes, MultipleDeletes) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        const string blockedNS("foo.bar");
        const string ns("test.user");

        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        env->addCursorId(blockedNS, 345);
        env->pauseDeletes();

        Notification notifyDone1;
        RangeDeleterOptions deleterOption1(KeyRange(ns,
                                                    BSON("x" << 10),
                                                    BSON("x" << 20),
                                                    BSON("x" << 1)));
        deleterOption1.waitForOpenCursors = true;
        ASSERT_TRUE(deleter.queueDelete(deleterOption1,
                                        &notifyDone1,
                                        NULL /* don't care errMsg */));

        env->waitForNthPausedDelete(1u);

        // Make sure that the delete is already in progress before proceeding.
        ASSERT_EQUALS(1U, deleter.getDeletesInProgress());

        Notification notifyDone2;
        RangeDeleterOptions deleterOption2(KeyRange(blockedNS,
                                                    BSON("x" << 20),
                                                    BSON("x" << 30),
                                                    BSON("x" << 1)));
        deleterOption2.waitForOpenCursors = true;
        ASSERT_TRUE(deleter.queueDelete(deleterOption2,
                                        &notifyDone2,
                                        NULL /* don't care errMsg */));

        Notification notifyDone3;
        RangeDeleterOptions deleterOption3(KeyRange(ns,
                                                    BSON("x" << 30),
                                                    BSON("x" << 40),
                                                    BSON("x" << 1)));
        deleterOption3.waitForOpenCursors = true;
        ASSERT_TRUE(deleter.queueDelete(deleterOption3,
                                        &notifyDone3,
                                        NULL /* don't care errMsg */));

        // Now, the setup is:
        // { x: 10 } => { x: 20 } in progress.
        // { x: 20 } => { x: 30 } waiting for cursor id 345.
        // { x: 30 } => { x: 40 } waiting to be picked up by worker.

        // Make sure that the current state matches the setup.
        ASSERT_EQUALS(3U, deleter.getTotalDeletes());
        ASSERT_EQUALS(2U, deleter.getPendingDeletes());
        ASSERT_EQUALS(1U, deleter.getDeletesInProgress());

        // Let the first delete proceed.
        env->resumeOneDelete();
        notifyDone1.waitToBeNotified();

        ASSERT_TRUE(env->deleteOccured());

        // { x: 10 } => { x: 20 } should be the first one since it is already in
        // progress before the others are queued.
        DeletedRange deleted1(env->getLastDelete());

        ASSERT_EQUALS(ns, deleted1.ns);
        ASSERT_TRUE(deleted1.min.equal(BSON("x" << 10)));
        ASSERT_TRUE(deleted1.max.equal(BSON("x" << 20)));
        ASSERT_TRUE(deleted1.shardKeyPattern.equal(BSON("x" << 1)));

        // Let the second delete proceed.
        env->resumeOneDelete();
        notifyDone3.waitToBeNotified();

        DeletedRange deleted2(env->getLastDelete());

        // { x: 30 } => { x: 40 } should be next since there are still
        // cursors open for blockedNS.

        ASSERT_EQUALS(ns, deleted2.ns);
        ASSERT_TRUE(deleted2.min.equal(BSON("x" << 30)));
        ASSERT_TRUE(deleted2.max.equal(BSON("x" << 40)));
        ASSERT_TRUE(deleted2.shardKeyPattern.equal(BSON("x" << 1)));

        env->removeCursorId(blockedNS, 345);
        // Let the last delete proceed.
        env->resumeOneDelete();
        notifyDone2.waitToBeNotified();

        DeletedRange deleted3(env->getLastDelete());

        ASSERT_EQUALS(blockedNS, deleted3.ns);
        ASSERT_TRUE(deleted3.min.equal(BSON("x" << 20)));
        ASSERT_TRUE(deleted3.max.equal(BSON("x" << 30)));
        ASSERT_TRUE(deleted3.shardKeyPattern.equal(BSON("x" << 1)));

        deleter.stopWorkers();
    }

    // Should not be able to delete ranges that overlaps with a black listed range.
    TEST(BlackList, CantDeleteBlackListed) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        deleter.startWorkers();

        const string ns("test.user");

        string errMsg;
        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 100), BSON("x" << 200), &errMsg));
        ASSERT_TRUE(errMsg.empty());

        errMsg.clear();
        ASSERT_FALSE(deleter.queueDelete(RangeDeleterOptions(KeyRange(ns,
                                                                      BSON("x" << 120),
                                                                      BSON("x" << 140),
                                                                      BSON("x" << 1))),
                                         NULL /* notifier not needed */,
                                         &errMsg));
        ASSERT_FALSE(errMsg.empty());

        errMsg.clear();
        ASSERT_FALSE(deleter.deleteNow(noTxn,
                                       RangeDeleterOptions(KeyRange(ns,
                                                                    BSON("x" << 120),
                                                                    BSON("x" << 140),
                                                                    BSON("x" << 1))),
                                       &errMsg));
        ASSERT_FALSE(errMsg.empty());

        ASSERT_FALSE(env->deleteOccured());

        deleter.stopWorkers();
    }

    // Should not be able to black list a range that overlaps with a range that is
    // already blacklisted.
    TEST(BlackList, CantDoubleBlackList) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        const string ns("test.user");

        string errMsg;
        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 100), BSON("x" << 200), &errMsg));
        ASSERT_TRUE(errMsg.empty());

        errMsg.clear();
        ASSERT_FALSE(deleter.addToBlackList(ns, BSON("x" << 100), BSON("x" << 200), &errMsg));
        ASSERT_FALSE(errMsg.empty());

        errMsg.clear();
        ASSERT_FALSE(deleter.addToBlackList(ns, BSON("x" << 80), BSON("x" << 120), &errMsg));
        ASSERT_FALSE(errMsg.empty());

        deleter.stopWorkers();
    }

    // Should not be able to black list a range that overlaps with a range that is already
    // queued for deletion.
    TEST(BlackList, CantBlackListQueued) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        const string ns("test.user");
        deleter.startWorkers();

        // Set cursors on NS so deletes cannot be processed immediately.
        env->addCursorId(ns, 58);

        Notification notifyDone;
        deleter.queueDelete(RangeDeleterOptions(KeyRange(ns,
                                                         BSON("x" << 0),
                                                         BSON("x" << 10),
                                                         BSON("x" << 1))),
                            &notifyDone,
                            NULL /* errMsg not needed */);

        string errMsg;
        ASSERT_FALSE(deleter.addToBlackList(ns, BSON("x" << 5), BSON("x" << 15), &errMsg));
        ASSERT_FALSE(errMsg.empty());

        env->removeCursorId(ns, 58);
        notifyDone.waitToBeNotified();

        // But should be able to black list again once removed from the queue.
        errMsg.clear();
        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 5), BSON("x" << 15), &errMsg));
        ASSERT_TRUE(errMsg.empty());

        deleter.stopWorkers();
    }

    // Should not be able to black list a range that overlaps the range of an
    // immediate delete that is currently in progress.
    TEST(BlackList, CantBlackListImmediateInProgress) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        const string ns("test.user");

        env->pauseDeletes();

        string delErrMsg;
        boost::thread deleterThread =
                boost::thread(mongo::stdx::bind(rangeDeleterDeleteNow,
                                                &deleter,
                                                noTxn,
                                                RangeDeleterOptions(KeyRange(ns,
                                                                             BSON("x" << 64),
                                                                             BSON("x" << 70),
                                                                             BSON("x" << 1))),
                                                &delErrMsg));

        env->waitForNthPausedDelete(1u);

        string blErrMsg;
        ASSERT_FALSE(deleter.addToBlackList(ns, BSON("x" << 10), BSON("x" << 90), &blErrMsg));
        ASSERT_FALSE(blErrMsg.empty());

        env->resumeOneDelete();
        deleterThread.join();
        ASSERT_TRUE(delErrMsg.empty());

        // Can blacklist again after delete completed.
        blErrMsg.clear();
        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 10), BSON("x" << 90), &blErrMsg));
        ASSERT_TRUE(blErrMsg.empty());

        deleter.stopWorkers();
    }

    // Undo black list should only work if the range given exactly match with an
    // existing black listed range.
    TEST(BlackList, UndoShouldBeExact) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        const string ns("test.user");

        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 1234), BSON("x" << 8952),
                                           NULL /* errMsg not needed */));

        ASSERT_FALSE(deleter.removeFromBlackList(ns, BSON("x" << 1234), BSON("x" << 9000)));

        // Range should still be blacklisted
        ASSERT_FALSE(deleter.deleteNow(noTxn,
                                       RangeDeleterOptions(KeyRange(ns,
                                                                    BSON("x" << 2000),
                                                                    BSON("x" << 4000),
                                                                    BSON("x" << 1))),
                                       NULL /* errMsg not needed */));

        deleter.stopWorkers();
    }

    // Should be able to delete the range again once the black list has been undone.
    TEST(BlackList, UndoBlackList) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);
        const string ns("test.user");

        string errMsg;
        ASSERT_TRUE(deleter.addToBlackList(ns, BSON("x" << 500), BSON("x" << 801), &errMsg));
        ASSERT_TRUE(errMsg.empty());

        errMsg.clear();
        ASSERT_FALSE(deleter.deleteNow(noTxn,
                                       RangeDeleterOptions(KeyRange(ns,
                                                                    BSON("x" << 600),
                                                                    BSON("x" << 700),
                                                                    BSON("x" << 1))),
                                       &errMsg));
        ASSERT_FALSE(errMsg.empty());

        ASSERT_TRUE(deleter.removeFromBlackList(ns, BSON("x" << 500), BSON("x" << 801)));

        errMsg.clear();
        ASSERT_TRUE(deleter.deleteNow(noTxn,
                                      RangeDeleterOptions(KeyRange(ns,
                                                                   BSON("x" << 600),
                                                                   BSON("x" << 700),
                                                                   BSON("x" << 1))),
                                      &errMsg));
        ASSERT_TRUE(errMsg.empty());

        deleter.stopWorkers();
    }

    // Black listing should only affect the specified namespace.
    TEST(BlackList, NSIsolation) {
        mongo::repl::setGlobalReplicationCoordinator(
                new mongo::repl::ReplicationCoordinatorMock(replSettings));
        RangeDeleterMockEnv* env = new RangeDeleterMockEnv();
        RangeDeleter deleter(env);

        deleter.addToBlackList("foo.bar", BSON("x" << 100), BSON("x" << 200),
                               NULL /* errMsg not needed */);

        ASSERT_TRUE(deleter.deleteNow(noTxn,
                                      RangeDeleterOptions(KeyRange("test.user",
                                                                   BSON("x" << 120),
                                                                   BSON("x" << 140),
                                                                   BSON("x" << 1))),
                                      NULL /* errMsg not needed */));

        deleter.stopWorkers();
    }

} // unnamed namespace
