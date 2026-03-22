/*
    SPDX-FileCopyrightText: 2025 Struktured Labs

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SESSIONMANAGERPANELTEST_H
#define SESSIONMANAGERPANELTEST_H

#include <QObject>

namespace Konsolai
{

class SessionManagerPanelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    // Metadata filtering
    void testAllSessionsEmpty();
    void testAllSessionsLoaded();
    void testPinnedSessionsFilter();
    void testArchivedSessionsFilter();
    void testPinnedExcludesArchived();

    // Pin/Unpin
    void testPinSession();
    void testUnpinSession();
    void testPinNonexistentSession();
    void testPinSession_ImmediateTreeUpdate();

    // Archive
    void testArchiveSession();
    void testArchiveNonexistentSession();

    // Close (new feature)
    void testCloseSessionNotArchived();
    void testCloseNonexistentSession();

    // Mark expired
    void testMarkExpired();
    void testMarkExpiredUnknownSession();

    // Collapsed state
    void testCollapsedToggle();
    void testCollapsedSignal();
    void testCollapsedIdempotent();

    // Dismiss / Restore / Purge lifecycle
    void testDismissSession();
    void testDismissNonexistentSession();
    void testRestoreSession();
    void testRestoreNonexistentSession();
    void testPurgeSession();
    void testPurgeNonexistentSession();
    void testPurgeDismissed();
    void testDismissRestorePurgeRoundTrip();

    // Metadata persistence round-trip
    void testMetadataPersistence();
    void testMetadataYoloPersistence();
    void testMetadataSshFields();
    void testMetadataBudgetPersistence();
    void testMetadataCorruptedJson();
    void testMetadataMissingFields();
    void testMetadataApprovalCountPersistence();

    // Full round-trip with ALL fields
    void testMetadataAllFieldsRoundTrip();
    void testMetadataApprovalLogRoundTrip();
    void testMetadataMultipleSessionsRoundTrip();
    void testMetadataSaveLoadIdempotent();

    // Subagent/subprocess metadata persistence
    void testMetadataSubagentPersistence();
    void testMetadataSubprocessPersistence();
    void testMetadataPromptLabelsPersistence();
    void testMetadataSubagentEmptyNotSerialized();
    void testMetadataSubagentRoundTrip();

    // Remote session registration and restoration
    void testRegisterSessionCapturesRemoteFields();
    void testUnarchiveEmitsRemoteFields();
    void testUnarchiveLocalSessionEmitsNoRemoteFields();
    void testRegisterRemoteSessionRoundTrip();

    // Bulk operations
    void testBulkArchiveMultipleSessions();
    void testBulkDismissMultipleSessions();
    void testBulkDismissOlderThan();
    void testBulkCloseMultipleSessions();

    // Tree widget rendering — subagent/team subnodes
    void testTreeSubagentItemsRendered();
    void testTreeSubprocessItemsRendered();
    void testTreeMultiRoundPromptGroups();
    void testTreeTaskGrouping();
    void testTreeHideCompletedAgents();
    void testTreeSubagentStateIcons();
    void testTreePersistedAgentsForcedNotRunning();

    // Timer pause/resume (window activation)
    void testPauseResumeIdempotent();
    void testPauseSuppressesTreeUpdates();
    void testPauseSuppressesMetadataSaves();
    void testResumeFlushesDeferred();

    // Register fast-path (tab switch)
    void testRegisterSessionFastPath();

    // Auto-archive old closed sessions
    void testAutoArchiveClosedSessions();
    void testAutoArchiveSkipsPinned();
    void testAutoArchiveSkipsRecent();

    // Session folders
    void testCreateFolder();
    void testRenameFolderEmpty();
    void testDeleteFolderUngroupsSessions();
    void testMoveSessionToFolder();
    void testRemoveSessionFromFolder();
    void testFolderIdPersistence();
    void testFoldersPersistence();
    void testTreeRoutesGroupedSessionsToFolder();
};

}

#endif // SESSIONMANAGERPANELTEST_H
