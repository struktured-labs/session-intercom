/*
    SPDX-FileCopyrightText: 2025 Struktured Labs
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SESSIONMANAGERPANEL_H
#define SESSIONMANAGERPANEL_H

#include "konsoleprivate_export.h"

#include "ClaudeSession.h"
#include "ClaudeSessionRegistry.h"

#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace Konsolai
{

class ClaudeSessionRegistry;

/**
 * Session metadata stored persistently
 */
struct KONSOLEPRIVATE_EXPORT SessionMetadata {
    QString sessionId;
    QString sessionName;
    QString profileName;
    QString workingDirectory;
    bool isPinned = false;
    bool isArchived = false;
    bool isExpired = false;
    bool isDismissed = false;
    QDateTime lastAccessed;
    QDateTime createdAt;

    // SSH remote session fields
    bool isRemote = false;
    QString sshHost;
    QString sshUsername;
    int sshPort = 22;

    // Per-session yolo mode settings (persisted across restarts)
    bool yoloMode = false;
    bool doubleYoloMode = false;
    bool tripleYoloMode = false;

    // Approval counts (persisted across restarts)
    int yoloApprovalCount = 0;
    int doubleYoloApprovalCount = 0;
    int tripleYoloApprovalCount = 0;

    // Approval log entries (persisted across restarts)
    QVector<ApprovalLogEntry> approvalLog;

    // Budget settings (persisted across restarts)
    int budgetTimeLimitMinutes = 0;
    double budgetCostCeilingUSD = 0.0;
    quint64 budgetTokenCeiling = 0;

    // Claude conversation UUID for --resume across close/reopen cycles
    QString lastResumeSessionId;

    // Human-readable description (first prompt or user-set label)
    QString description;

    // Agent linkage: non-empty if this session was created from agent attach
    QString agentId;

    // Persisted subagent/subprocess snapshots (survive restart)
    QVector<SubagentInfo> subagents;
    QVector<SubprocessInfo> subprocesses;
    QMap<int, QString> promptGroupLabels;
    int currentPromptRound = 0;

    // Session folder membership (empty = ungrouped)
    QString folderId;
};

/**
 * Named folder for grouping related sessions (e.g. agent fleets, projects)
 */
struct KONSOLEPRIVATE_EXPORT SessionFolderInfo {
    QString folderId;
    QString name;
    QColor color;
    QDateTime createdAt;
    int sortOrder = 0;
};

/**
 * SessionManagerPanel provides a collapsible sidebar for managing all Claude sessions.
 *
 * Features:
 * - Shows all sessions: pinned, active, and archived
 * - Pin sessions to keep them at the top
 * - Archive sessions (kills tmux but preserves metadata for later restoration)
 * - Only active sessions appear as tabs
 * - Double-click to attach/open a session
 */
class KONSOLEPRIVATE_EXPORT SessionManagerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SessionManagerPanel(QWidget *parent = nullptr);
    ~SessionManagerPanel() override;

    /**
     * Register an active session
     */
    void registerSession(ClaudeSession *session);

    /**
     * Unregister a session (when tab is closed)
     */
    void unregisterSession(ClaudeSession *session);

    /**
     * Get all session metadata
     */
    QList<SessionMetadata> allSessions() const;

    /**
     * Get metadata for a specific session (const).
     * Returns nullptr if not found.
     */
    const SessionMetadata *sessionMetadata(const QString &sessionId) const;

    /**
     * Check if a session is currently active (has an open tab)
     */
    bool isSessionActive(const QString &sessionId) const;

    /**
     * Get pinned sessions
     */
    QList<SessionMetadata> pinnedSessions() const;

    /**
     * Get archived sessions
     */
    QList<SessionMetadata> archivedSessions() const;

    /**
     * Check if panel is collapsed
     */
    bool isCollapsed() const
    {
        return m_collapsed;
    }

    /**
     * Sum of estimated cost across all sessions for the current week (Mon-Sun)
     */
    double weeklySpentUSD() const;

    /**
     * Sum of estimated cost across all sessions for the current calendar month
     */
    double monthlySpentUSD() const;

public Q_SLOTS:
    /**
     * Toggle panel collapsed state
     */
    void setCollapsed(bool collapsed);

    /**
     * Refresh the session list
     */
    void refresh();

    /**
     * Pin a session
     */
    void pinSession(const QString &sessionId);

    /**
     * Unpin a session
     */
    void unpinSession(const QString &sessionId);

    /**
     * Archive a session (kills tmux, preserves metadata)
     */
    void archiveSession(const QString &sessionId);

    /**
     * Close a session (kills tmux, moves to Closed category without archiving)
     */
    void closeSession(const QString &sessionId);

    /**
     * Unarchive a session (restarts Claude with same session ID)
     */
    void unarchiveSession(const QString &sessionId);

    /**
     * Mark a session as expired (dead tmux backend) and auto-archive it
     */
    void markExpired(const QString &sessionName);

    /**
     * Find session ID by tmux session name.
     * Returns empty string if not found.
     */
    QString sessionIdForName(const QString &sessionName) const;

    /**
     * Update a session's description in persisted metadata (called from tab context menu)
     */
    void updateSessionDescription(const QString &sessionId, const QString &desc);

    /**
     * Dismiss a session (soft delete — hides from normal view, metadata retained)
     */
    void dismissSession(const QString &sessionId);

    /**
     * Restore a dismissed session back to Archived
     */
    void restoreSession(const QString &sessionId);

    /**
     * Purge a session (permanently remove metadata, project files untouched)
     */
    void purgeSession(const QString &sessionId);

    /**
     * Purge all dismissed sessions
     */
    void purgeDismissed();

    /**
     * Set the agentId on a session's metadata (for agent-originated sessions)
     */
    void setSessionAgentId(const QString &sessionId, const QString &agentId);

    /**
     * Create a new session folder
     */
    void createFolder(const QString &name, const QColor &color = QColor());

    /**
     * Rename a session folder
     */
    void renameFolder(const QString &folderId, const QString &newName);

    /**
     * Change a session folder's color
     */
    void setFolderColor(const QString &folderId, const QColor &color);

    /**
     * Delete a session folder (ungroups all member sessions)
     */
    void deleteFolder(const QString &folderId);

    /**
     * Move a session into a folder
     */
    void moveSessionToFolder(const QString &sessionId, const QString &folderId);

    /**
     * Remove a session from its folder (back to ungrouped)
     */
    void removeSessionFromFolder(const QString &sessionId);

    /**
     * Get all session folders
     */
    QList<SessionFolderInfo> allFolders() const;

    /**
     * Auto-archive closed sessions older than 7 days.
     * Runs periodically on a timer; can also be called manually.
     */
    void autoArchiveOldClosedSessions();

    /**
     * Pause non-essential background timers (called when window becomes inactive)
     */
    void pauseBackgroundTimers();

    /**
     * Resume background timers (called when window becomes active again)
     */
    void resumeBackgroundTimers();

    /**
     * Force a synchronous tree rebuild using cached live session data.
     * Used by tests to avoid async tmux query timing issues.
     */
    void rebuildTreeSync()
    {
        updateTreeWidgetWithLiveSessions(m_cachedLiveNames);
    }

Q_SIGNALS:
    /**
     * Emitted when user wants to attach to a session
     */
    void attachRequested(const QString &sessionName);

    /**
     * Emitted when user wants to reattach to a remote SSH tmux session
     */
    void remoteAttachRequested(const QString &sshHost, const QString &sshUsername, int sshPort, const QString &workDir, const QString &tmuxSessionName);

    /**
     * Emitted when user wants to focus/select the tab for an active session
     */
    void focusSessionRequested(ClaudeSession *session);

    /**
     * Emitted when user wants to create a new session
     */
    void newSessionRequested();

    /**
     * Emitted when user wants to unarchive and start a session
     */
    void unarchiveRequested(const QString &sessionId, const QString &workingDirectory, bool isRemote, const QString &sshHost, const QString &sshUsername, int sshPort);

    /**
     * Emitted when user wants to open a remote SSH session
     */
    void remoteSessionRequested(const QString &sshHost, const QString &sshUsername, int sshPort, const QString &workDir);

    /**
     * Emitted when user wants to create a worktree session from an existing session
     */
    void worktreeSessionRequested(const QString &sourceWorkingDir);

    /**
     * Emitted when user wants to resume a specific conversation in a directory.
     * Works for both local and remote sessions.
     */
    void resumeConversationRequested(const QString &workingDirectory, const QString &conversationId,
                                      const QString &sshHost, const QString &sshUsername, int sshPort);

    /**
     * Emitted when user wants to navigate to an agent in the agent panel
     */
    void showAgentRequested(const QString &agentId);

    /**
     * Emitted when collapsed state changes
     */
    void collapsedChanged(bool collapsed);

    /**
     * Emitted when aggregated usage may have changed (after metadata save)
     */
    void usageAggregateChanged();

    /**
     * Emitted when session folders are created, modified, or deleted
     */
    void foldersChanged();

private Q_SLOTS:
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onContextMenu(const QPoint &pos);
    void onNewSessionClicked();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void setupUi();
    void deferredInit();
    void showLoadingState();
    void showReadyState();
    void loadMetadata();
    void saveMetadata(bool sync = false);
    void loadFolders();
    void saveFolders();
    void scheduleTreeUpdate(); // debounced — coalesces rapid-fire calls
    void scheduleMetadataSave(); // debounced — coalesces rapid-fire saves
    void updateTreeWidget();
    void updateTreeWidgetWithLiveSessions(const QSet<QString> &liveNames);
    void addSessionToTree(const SessionMetadata &meta, QTreeWidgetItem *parent, bool hasSiblings = false);
    void showApprovalLog(ClaudeSession *session);
    void showSessionActivity(const QString &jsonlPath, const QString &workDir);
    void showSubagentTranscript(const SubagentInfo &info);
    void showSubagentDetails(const SubagentInfo &info);
    void showSubprocessOutput(const SubprocessInfo &info);
    void editSessionDescription(const QString &sessionId);
    void editSessionBudget(ClaudeSession *session, const QString &sessionId);
    void cleanupStaleSockets();
    void ensureHooksConfigured(ClaudeSession *session);
    SessionMetadata *findMetadata(const QString &sessionId);
    QTreeWidgetItem *findTreeItem(const QString &sessionId);
    void applyFilter(const QString &text);
    void updateDurationLabels(); // In-place duration label updates without full rebuild

    // Tree expansion state preservation
    void saveTreeState();
    void restoreTreeState();
    QString compositeKeyForItem(QTreeWidgetItem *item) const;
    bool shouldAutoExpand(const QString &key, const QString &sessionId, bool hasActiveChildren) const;
    void pruneStaleKeys();
    bool isTreeInteractionActive() const;

    QTreeWidget *m_treeWidget = nullptr;
    QLabel *m_emptyStateLabel = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QPushButton *m_newSessionButton = nullptr;
    QPushButton *m_collapseButton = nullptr;
    QTreeWidgetItem *m_pinnedCategory = nullptr;
    QTreeWidgetItem *m_activeCategory = nullptr;
    QTreeWidgetItem *m_detachedCategory = nullptr;
    QTreeWidgetItem *m_closedCategory = nullptr;
    QTreeWidgetItem *m_archivedCategory = nullptr;
    QTreeWidgetItem *m_dismissedCategory = nullptr;
    QTreeWidgetItem *m_discoveredCategory = nullptr;

    QProgressBar *m_loadingBar = nullptr;
    bool m_initialized = false;
    QVector<QPointer<ClaudeSession>> m_pendingRegistrations;

    QMap<QString, SessionMetadata> m_metadata;
    QMap<QString, QPointer<ClaudeSession>> m_activeSessions;
    ClaudeSessionRegistry *m_registry = nullptr;
    bool m_collapsed = false;

    // Session folders
    QMap<QString, SessionFolderInfo> m_folders;
    QMap<QString, QTreeWidgetItem *> m_folderTreeItems; // folderId → tree item

    // Debounce timer for updateTreeWidget — coalesces rapid-fire signals
    QTimer *m_updateDebounce = nullptr;

    // Debounce timer for saveMetadata — coalesces rapid-fire approval events
    QTimer *m_saveDebounce = nullptr;

    // Periodic timer to update elapsed duration for subagent items
    QTimer *m_durationTimer = nullptr;

    // Guard against overlapping async tmux queries
    bool m_asyncQueryInFlight = false;
    bool m_asyncQueryPending = false;

    // Cached live session names from last async tmux query
    QSet<QString> m_cachedLiveNames;

    // Cached remote live session names (queried via SSH, refreshed less frequently)
    QSet<QString> m_cachedRemoteLiveNames;
    QTimer *m_remoteTmuxTimer = nullptr;
    void refreshRemoteTmuxSessions();
    void refreshCachesAsync(); // background thread for discoverSessions + readClaudeConversations

    // Cache conversations per working directory to avoid disk I/O during tree rebuilds
    QHash<QString, QList<ClaudeConversation>> m_conversationCache; // workDir → conversations

    // Cache git branch names per working directory (TTL-based, refreshed every 60s)
    QHash<QString, QString> m_gitBranchCache; // workDir → branch name

    // TTL timers for cache invalidation
    QTimer *m_gitCacheTimer = nullptr; // 60s TTL for git branch cache
    QTimer *m_convCacheTimer = nullptr; // 120s TTL for conversation + discovered + GSD caches

    // Cached discoverSessions() results (invalidated on 120s timer and session register/unregister)
    QList<ClaudeSessionState> m_cachedDiscoveredSessions;
    bool m_discoveredCacheValid = false;

    // Smart signal filtering: skip rebuilds when state hasn't visually changed
    QHash<QString, ClaudeProcess::State> m_lastKnownState;
    QHash<QString, int> m_lastKnownApprovalCount;

    // GSD badge cache (workDir → has .planning/ or ROADMAP.md)
    QHash<QString, bool> m_gsdBadgeCache;

    // Per-session toggle: sessions in this set hide completed (NotRunning) agents
    QSet<QString> m_hideCompletedAgents;

    // Sessions explicitly closed via closeSession() — forced to "Closed" category
    // even if tmux hasn't died yet (async kill race)
    QSet<QString> m_explicitlyClosed;

    // --- Tree expansion state preservation ---

    // Composite key → isExpanded, captured before each tree rebuild
    QHash<QString, bool> m_expansionState;

    // Composite keys seen in at least one rebuild (distinguishes new items from existing)
    QSet<QString> m_knownItems;

    // Sessions the user has muted (suppresses auto-expand). Ephemeral, not persisted.
    QSet<QString> m_mutedSessions;

    // Saved scroll position and selection key for restoration after rebuild
    int m_savedScrollPosition = 0;
    QString m_savedSelectedKey;

    // Whether a tree update is pending due to interaction deferral
    bool m_pendingUpdate = false;
    QTimer *m_deferRetryTimer = nullptr;

    // Track pending async tmux kill operations for wait cursor management.
    // setOverrideCursor pushes to a stack — we use a counter so batch operations
    // (Archive All, Close All) only push/pop once.
    int m_pendingAsyncKills = 0;

    // Auto-archive closed sessions older than threshold
    QTimer *m_autoArchiveTimer = nullptr;

    // Whether background timers are paused (window inactive)
    bool m_timersPaused = false;

    // Whether an async cache refresh is in flight (prevents overlapping refreshes)
    bool m_cacheRefreshInFlight = false;

    // Whether a metadata save was deferred during timer pause
    bool m_pendingSave = false;
};

} // namespace Konsolai

#endif // SESSIONMANAGERPANEL_H
