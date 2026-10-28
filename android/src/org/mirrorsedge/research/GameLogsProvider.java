package org.mirrorsedge.research;

/**
 * LogsProvider hosted in the game process (manifest: android:process=":game"). The pause menu
 * shares through it: while the game runs, the launcher process is in the background and may be
 * frozen by the system, so a read through the launcher-hosted provider can return nothing
 * (0-byte file in the receiving app).
 */
public final class GameLogsProvider extends LogsProvider {
    static final String AUTHORITY = "org.mirrorsedge.research.logs.game";
}
