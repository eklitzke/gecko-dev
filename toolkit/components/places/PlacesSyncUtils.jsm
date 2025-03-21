/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["PlacesSyncUtils"];

Cu.importGlobalProperties(["URL", "URLSearchParams"]);

ChromeUtils.import("resource://gre/modules/Services.jsm");
ChromeUtils.import("resource://gre/modules/XPCOMUtils.jsm");

ChromeUtils.defineModuleGetter(this, "Log",
                               "resource://gre/modules/Log.jsm");
ChromeUtils.defineModuleGetter(this, "PlacesUtils",
                               "resource://gre/modules/PlacesUtils.jsm");

/**
 * This module exports functions for Sync to use when applying remote
 * records. The calls are similar to those in `Bookmarks.jsm` and
 * `nsINavBookmarksService`, with special handling for smart bookmarks,
 * tags, keywords, synced annotations, and missing parents.
 */
var PlacesSyncUtils = {
  /**
   * Auxiliary generator function that yields an array in chunks
   *
   * @param  array
   * @param  chunkLength
   * @yields {Array}
   *         New Array with the next chunkLength elements of array.
   *         If the array has less than chunkLength elements, yields all of them
   */
  * chunkArray(array, chunkLength) {
    if (!array.length || chunkLength <= 0) {
      return;
    }
    let startIndex = 0;
    while (startIndex < array.length) {
      yield array.slice(startIndex, startIndex + chunkLength);
      startIndex += chunkLength;
    }
  },
};

const { SOURCE_SYNC } = Ci.nsINavBookmarksService;

const MICROSECONDS_PER_SECOND = 1000000;
const SQLITE_MAX_VARIABLE_NUMBER = 999;

const MOBILE_BOOKMARKS_PREF = "browser.bookmarks.showMobileBookmarks";

// These are defined as lazy getters to defer initializing the bookmarks
// service until it's needed.
XPCOMUtils.defineLazyGetter(this, "ROOT_RECORD_ID_TO_GUID", () => ({
  menu: PlacesUtils.bookmarks.menuGuid,
  places: PlacesUtils.bookmarks.rootGuid,
  tags: PlacesUtils.bookmarks.tagsGuid,
  toolbar: PlacesUtils.bookmarks.toolbarGuid,
  unfiled: PlacesUtils.bookmarks.unfiledGuid,
  mobile: PlacesUtils.bookmarks.mobileGuid,
}));

XPCOMUtils.defineLazyGetter(this, "ROOT_GUID_TO_RECORD_ID", () => ({
  [PlacesUtils.bookmarks.menuGuid]: "menu",
  [PlacesUtils.bookmarks.rootGuid]: "places",
  [PlacesUtils.bookmarks.tagsGuid]: "tags",
  [PlacesUtils.bookmarks.toolbarGuid]: "toolbar",
  [PlacesUtils.bookmarks.unfiledGuid]: "unfiled",
  [PlacesUtils.bookmarks.mobileGuid]: "mobile",
}));

XPCOMUtils.defineLazyGetter(this, "ROOTS", () =>
  Object.keys(ROOT_RECORD_ID_TO_GUID)
);

const HistorySyncUtils = PlacesSyncUtils.history = Object.freeze({
  SYNC_ID_META_KEY: "sync/history/syncId",
  LAST_SYNC_META_KEY: "sync/history/lastSync",

  /**
   * Returns the current history sync ID, or `""` if one isn't set.
   */
  async getSyncId() {
    let syncId = await PlacesUtils.metadata.get(
      HistorySyncUtils.SYNC_ID_META_KEY);
    return syncId || "";
  },

  /**
   * Assigns a new sync ID. This is called when we sync for the first time with
   * a new account, and when we're the first to sync after a node reassignment.
   *
   * @return {Promise} resolved once the ID has been updated.
   * @resolves to the new sync ID.
   */
  resetSyncId() {
    return PlacesUtils.withConnectionWrapper(
      "HistorySyncUtils: resetSyncId",
      function(db) {
        let newSyncId = PlacesUtils.history.makeGuid();
        return db.executeTransaction(async function() {
          await setHistorySyncId(db, newSyncId);
          return newSyncId;
        });
      }
    );
  },

  /**
   * Ensures that the existing local sync ID, if any, is up-to-date with the
   * server. This is called when we sync with an existing account.
   *
   * @param newSyncId
   *        The server's sync ID.
   * @return {Promise} resolved once the ID has been updated.
   */
  async ensureCurrentSyncId(newSyncId) {
    if (!newSyncId || typeof newSyncId != "string") {
      throw new TypeError("Invalid new history sync ID");
    }
    await PlacesUtils.withConnectionWrapper(
      "HistorySyncUtils: ensureCurrentSyncId",
      async function(db) {
        let existingSyncId = await PlacesUtils.metadata.getWithConnection(
          db, HistorySyncUtils.SYNC_ID_META_KEY);

        if (existingSyncId == newSyncId) {
          HistorySyncLog.trace("History sync ID up-to-date",
                               { existingSyncId });
          return;
        }

        HistorySyncLog.info("History sync ID changed; resetting metadata",
                            { existingSyncId, newSyncId });
        await db.executeTransaction(function() {
          return setHistorySyncId(db, newSyncId);
        });
      }
    );
  },

  /**
   * Returns the last sync time, in seconds, for the history collection, or 0
   * if history has never synced before.
   */
  async getLastSync() {
    let lastSync = await PlacesUtils.metadata.get(
      HistorySyncUtils.LAST_SYNC_META_KEY);
    return lastSync ? lastSync / 1000 : 0;
  },

  /**
   * Updates the history collection last sync time.
   *
   * @param lastSyncSeconds
   *        The collection last sync time, in seconds, as a number or string.
   */
  async setLastSync(lastSyncSeconds) {
    let lastSync = Math.floor(lastSyncSeconds * 1000);
    if (!Number.isInteger(lastSync)) {
      throw new TypeError("Invalid history last sync timestamp");
    }
    await PlacesUtils.metadata.set(HistorySyncUtils.LAST_SYNC_META_KEY,
                                   lastSync);
  },

  /**
   * Removes all history visits and pages from the database. Sync calls this
   * method when it receives a command from a remote client to wipe all stored
   * data.
   *
   * @return {Promise} resolved once all pages and visits have been removed.
   */
  async wipe() {
    await PlacesUtils.history.clear();
    await HistorySyncUtils.reset();
  },

  /**
   * Removes the sync ID and last sync time for the history collection. Unlike
   * `wipe`, this keeps all existing history pages and visits.
   *
   * @return {Promise} resolved once the metadata have been removed.
   */
  reset() {
    return PlacesUtils.metadata.delete(HistorySyncUtils.SYNC_ID_META_KEY,
      HistorySyncUtils.LAST_SYNC_META_KEY);
  },

  /**
   * Clamps a history visit date between the current date and the earliest
   * sensible date.
   *
   * @param {Date} visitDate
   *        The visit date.
   * @return {Date} The clamped visit date.
   */
  clampVisitDate(visitDate) {
    let currentDate = new Date();
    if (visitDate > currentDate) {
      return currentDate;
    }
    if (visitDate < BookmarkSyncUtils.EARLIEST_BOOKMARK_TIMESTAMP) {
      return new Date(BookmarkSyncUtils.EARLIEST_BOOKMARK_TIMESTAMP);
    }
    return visitDate;
  },

  /**
   * Fetches the frecency for the URL provided
   *
   * @param url
   * @returns {Number} The frecency of the given url
   */
  async fetchURLFrecency(url) {
    let canonicalURL = PlacesUtils.SYNC_BOOKMARK_VALIDATORS.url(url);

    let db = await PlacesUtils.promiseDBConnection();
    let rows = await db.executeCached(`
      SELECT frecency
      FROM moz_places
      WHERE url_hash = hash(:url) AND url = :url
      LIMIT 1`,
      { url: canonicalURL.href }
    );

    return rows.length ? rows[0].getResultByName("frecency") : -1;
  },

  /**
   * Filters syncable places from a collection of places guids.
   *
   * @param guids
   *
   * @returns {Array} new Array with the guids that aren't syncable
   */
  async determineNonSyncableGuids(guids) {
    // Filter out hidden pages and `TRANSITION_FRAMED_LINK` visits. These are
    // excluded when rendering the history menu, so we use the same constraints
    // for Sync. We also don't want to sync `TRANSITION_EMBED` visits, but those
    // aren't stored in the database.
    let db = await PlacesUtils.promiseDBConnection();
    let nonSyncableGuids = [];
    for (let chunk of PlacesSyncUtils.chunkArray(guids, SQLITE_MAX_VARIABLE_NUMBER)) {
      let rows = await db.execute(`
        SELECT DISTINCT p.guid FROM moz_places p
        JOIN moz_historyvisits v ON p.id = v.place_id
        WHERE p.guid IN (${new Array(chunk.length).fill("?").join(",")}) AND
            (p.hidden = 1 OR v.visit_type IN (0,
              ${PlacesUtils.history.TRANSITION_FRAMED_LINK}))
      `, chunk);
      nonSyncableGuids = nonSyncableGuids.concat(rows.map(row => row.getResultByName("guid")));
    }
    return nonSyncableGuids;
  },

  /**
   * Change the guid of the given uri
   *
   * @param uri
   * @param guid
   */
  changeGuid(uri, guid) {
      let canonicalURL = PlacesUtils.SYNC_BOOKMARK_VALIDATORS.url(uri);
      let validatedGuid = PlacesUtils.BOOKMARK_VALIDATORS.guid(guid);
      return PlacesUtils.withConnectionWrapper("PlacesSyncUtils.history: changeGuid",
        async function(db) {
          await db.executeCached(`
            UPDATE moz_places
            SET guid = :guid
            WHERE url_hash = hash(:page_url) AND url = :page_url`,
            {guid: validatedGuid, page_url: canonicalURL.href});
        });
  },

  /**
   * Fetch the last 20 visits (date and type of it) corresponding to a given url
   *
   * @param url
   * @returns {Array} Each element of the Array is an object with members: date and type
   */
  async fetchVisitsForURL(url) {
    let canonicalURL = PlacesUtils.SYNC_BOOKMARK_VALIDATORS.url(url);
    let db = await PlacesUtils.promiseDBConnection();
    let rows = await db.executeCached(`
      SELECT visit_type type, visit_date date
      FROM moz_historyvisits
      JOIN moz_places h ON h.id = place_id
      WHERE url_hash = hash(:url) AND url = :url
      ORDER BY date DESC LIMIT 20`, { url: canonicalURL.href }
    );
    return rows.map(row => {
      let visitDate = row.getResultByName("date");
      let visitType = row.getResultByName("type");
      return { date: visitDate, type: visitType };
    });
  },

  /**
   * Fetches the guid of a uri
   *
   * @param uri
   * @returns {String} The guid of the given uri
   */
  async fetchGuidForURL(url) {
      let canonicalURL = PlacesUtils.SYNC_BOOKMARK_VALIDATORS.url(url);
      let db = await PlacesUtils.promiseDBConnection();
      let rows = await db.executeCached(`
        SELECT guid
        FROM moz_places
        WHERE url_hash = hash(:page_url) AND url = :page_url`,
        { page_url: canonicalURL.href }
      );
      if (rows.length == 0) {
        return null;
      }
      return rows[0].getResultByName("guid");
  },

  /**
   * Fetch information about a guid (url, title and frecency)
   *
   * @param guid
   * @returns {Object} Object with three members: url, title and frecency of the given guid
   */
  async fetchURLInfoForGuid(guid) {
    let db = await PlacesUtils.promiseDBConnection();
    let rows = await db.executeCached(`
      SELECT url, IFNULL(title, "") AS title, frecency
      FROM moz_places
      WHERE guid = :guid`,
      { guid }
    );
    if (rows.length === 0) {
      return null;
    }
    return {
      url: rows[0].getResultByName("url"),
      title: rows[0].getResultByName("title"),
      frecency: rows[0].getResultByName("frecency"),
    };
  },

  /**
   * Get all URLs filtered by the limit and since members of the options object.
   *
   * @param options
   *        Options object with two members, since and limit. Both of them must be provided
   * @returns {Array} - Up to limit number of URLs starting from the date provided by since
   */
  async getAllURLs(options) {
    // Check that the limit property is finite number.
    if (!Number.isFinite(options.limit)) {
      throw new Error("The number provided in options.limit is not finite.");
    }
    // Check that the since property is of type Date.
    if (!options.since || Object.prototype.toString.call(options.since) != "[object Date]") {
      throw new Error("The property since of the options object must be of type Date.");
    }
    let db = await PlacesUtils.promiseDBConnection();
    let sinceInMicroseconds = PlacesUtils.toPRTime(options.since);
    let rows = await db.executeCached(`
      SELECT DISTINCT p.url
      FROM moz_places p
      JOIN moz_historyvisits v ON p.id = v.place_id
      WHERE p.last_visit_date > :cutoff_date AND
            p.hidden = 0 AND
            v.visit_type NOT IN (0,
              ${PlacesUtils.history.TRANSITION_FRAMED_LINK})
      ORDER BY frecency DESC
      LIMIT :max_results`,
      { cutoff_date: sinceInMicroseconds, max_results: options.limit }
    );
    return rows.map(row => row.getResultByName("url"));
  },
});

const BookmarkSyncUtils = PlacesSyncUtils.bookmarks = Object.freeze({
  SMART_BOOKMARKS_ANNO: "Places/SmartBookmark",
  DESCRIPTION_ANNO: "bookmarkProperties/description",
  SIDEBAR_ANNO: "bookmarkProperties/loadInSidebar",
  SYNC_PARENT_ANNO: "sync/parent",

  SYNC_ID_META_KEY: "sync/bookmarks/syncId",
  LAST_SYNC_META_KEY: "sync/bookmarks/lastSync",
  WIPE_REMOTE_META_KEY: "sync/bookmarks/wipeRemote",

  // Jan 23, 1993 in milliseconds since 1970. Corresponds roughly to the release
  // of the original NCSA Mosiac. We can safely assume that any dates before
  // this time are invalid.
  EARLIEST_BOOKMARK_TIMESTAMP: Date.UTC(1993, 0, 23),

  KINDS: {
    BOOKMARK: "bookmark",
    QUERY: "query",
    FOLDER: "folder",
    LIVEMARK: "livemark",
    SEPARATOR: "separator",
  },

  get ROOTS() {
    return ROOTS;
  },

  /**
   * Returns the current bookmarks sync ID, or `""` if one isn't set.
   */
  async getSyncId() {
    let syncId = await PlacesUtils.metadata.get(
      BookmarkSyncUtils.SYNC_ID_META_KEY);
    return syncId || "";
  },

  /**
   * Indicates if the bookmarks engine should erase all bookmarks on the server
   * and all other clients, because the user manually restored their bookmarks
   * from a backup on this client.
   */
  async shouldWipeRemote() {
    let shouldWipeRemote = await PlacesUtils.metadata.get(
      BookmarkSyncUtils.WIPE_REMOTE_META_KEY);
    return !!shouldWipeRemote;
  },

  /**
   * Assigns a new sync ID, bumps the change counter, and flags all items as
   * "NEW" for upload. This is called when we sync for the first time with a
   * new account, when we're the first to sync after a node reassignment, and
   * on the first sync after a manual restore.
   *
   * @return {Promise} resolved once the ID and all items have been updated.
   * @resolves to the new sync ID.
   */
  resetSyncId() {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: resetSyncId",
      function(db) {
        let newSyncId = PlacesUtils.history.makeGuid();
        return db.executeTransaction(async function() {
          await setBookmarksSyncId(db, newSyncId);
          await resetAllSyncStatuses(db,
            PlacesUtils.bookmarks.SYNC_STATUS.NEW);
          return newSyncId;
        });
      }
    );
  },

  /**
   * Ensures that the existing local sync ID, if any, is up-to-date with the
   * server. This is called when we sync with an existing account.
   *
   * We always take the server's sync ID. If we don't have an existing ID,
   * we're either syncing for the first time with an existing account, or Places
   * has automatically restored from a backup. If the sync IDs don't match,
   * we're likely syncing after a node reassignment, where another client
   * uploaded their bookmarks first.
   *
   * @param newSyncId
   *        The server's sync ID.
   * @return {Promise} resolved once the ID and all items have been updated.
   */
  async ensureCurrentSyncId(newSyncId) {
    if (!newSyncId || typeof newSyncId != "string") {
      throw new TypeError("Invalid new bookmarks sync ID");
    }
    await PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: ensureCurrentSyncId",
      async function(db) {
        let existingSyncId = await PlacesUtils.metadata.getWithConnection(
          db, BookmarkSyncUtils.SYNC_ID_META_KEY);

        // If we don't have a sync ID, take the server's without resetting
        // sync statuses.
        if (!existingSyncId) {
          BookmarkSyncLog.info("Taking new bookmarks sync ID", { newSyncId });
          await db.executeTransaction(() => setBookmarksSyncId(db, newSyncId));
          return;
        }

        // If the existing sync ID matches the server, great!
        if (existingSyncId == newSyncId) {
          BookmarkSyncLog.trace("Bookmarks sync ID up-to-date",
                                { existingSyncId });
          return;
        }

        // Otherwise, we have a sync ID, but it doesn't match, so we were likely
        // node reassigned. Take the server's sync ID and reset all items to
        // "UNKNOWN" so that we can merge.
        BookmarkSyncLog.info("Bookmarks sync ID changed; resetting sync " +
                             "statuses", { existingSyncId, newSyncId });
        await db.executeTransaction(async function() {
          await setBookmarksSyncId(db, newSyncId);
          await resetAllSyncStatuses(db,
            PlacesUtils.bookmarks.SYNC_STATUS.UNKNOWN);
        });
      }
    );
  },

  /**
   * Returns the last sync time, in seconds, for the bookmarks collection, or 0
   * if bookmarks have never synced before.
   */
  async getLastSync() {
    let lastSync = await PlacesUtils.metadata.get(
      BookmarkSyncUtils.LAST_SYNC_META_KEY);
    return lastSync ? lastSync / 1000 : 0;
  },

  /**
   * Updates the bookmarks collection last sync time.
   *
   * @param lastSyncSeconds
   *        The collection last sync time, in seconds, as a number or string.
   */
  async setLastSync(lastSyncSeconds) {
    let lastSync = Math.floor(lastSyncSeconds * 1000);
    if (!Number.isInteger(lastSync)) {
      throw new TypeError("Invalid bookmarks last sync timestamp");
    }
    await PlacesUtils.metadata.set(BookmarkSyncUtils.LAST_SYNC_META_KEY,
                                   lastSync);
  },

  /**
   * Resets Sync metadata for bookmarks in Places. This function behaves
   * differently depending on the change source, and may be called from
   * `PlacesSyncUtils.bookmarks.reset` or
   * `PlacesUtils.bookmarks.eraseEverything`.
   *
   * - RESTORE: The user is restoring from a backup. Drop the sync ID, last
   *   sync time, and tombstones; reset sync statuses for remaining items to
   *   "NEW"; then set a flag to wipe the server and all other clients. On the
   *   next sync, we'll replace their bookmarks with ours.
   *
   * - RESTORE_ON_STARTUP: Places is automatically restoring from a backup to
   *   recover from a corrupt database. The sync ID, last sync time, and
   *   tombstones don't exist, since we don't back them up; reset sync statuses
   *   for the roots to "UNKNOWN"; but don't wipe the server. On the next sync,
   *   we'll merge the restored bookmarks with the ones on the server.
   *
   * - SYNC: Either another client told us to erase our bookmarks
   *   (`PlacesSyncUtils.bookmarks.wipe`), or the user disconnected Sync
   *   (`PlacesSyncUtils.bookmarks.reset`). In both cases, drop the existing
   *   sync ID, last sync time, and tombstones; reset sync statuses for
   *   remaining items to "NEW"; and don't wipe the server.
   *
   * @param db
   *        the Sqlite.jsm connection handle.
   * @param source
   *        the change source constant.
   */
  async resetSyncMetadata(db, source) {
    if (![ PlacesUtils.bookmarks.SOURCES.RESTORE,
           PlacesUtils.bookmarks.SOURCES.RESTORE_ON_STARTUP,
           PlacesUtils.bookmarks.SOURCES.SYNC ].includes(source)) {
      return;
    }

    // Remove the sync ID and last sync time in all cases.
    await PlacesUtils.metadata.deleteWithConnection(db,
      BookmarkSyncUtils.SYNC_ID_META_KEY,
      BookmarkSyncUtils.LAST_SYNC_META_KEY);

    // If we're manually restoring from a backup, wipe the server and other
    // clients, so that we replace their bookmarks with the restored tree. If
    // we're automatically restoring to recover from a corrupt database, don't
    // wipe; we want to merge the restored tree with the one on the server.
    await PlacesUtils.metadata.setWithConnection(db,
      BookmarkSyncUtils.WIPE_REMOTE_META_KEY,
      source == PlacesUtils.bookmarks.SOURCES.RESTORE);

    // Reset change counters and sync statuses for roots and remaining
    // items, and drop tombstones.
    let syncStatus =
      source == PlacesUtils.bookmarks.SOURCES.RESTORE_ON_STARTUP ?
      PlacesUtils.bookmarks.SYNC_STATUS.UNKNOWN :
      PlacesUtils.bookmarks.SYNC_STATUS.NEW;
    await resetAllSyncStatuses(db, syncStatus);
  },

  /**
   * Converts a Places GUID to a Sync record ID. Record IDs are identical to
   * Places GUIDs for all items except roots.
   */
  guidToRecordId(guid) {
    return ROOT_GUID_TO_RECORD_ID[guid] || guid;
  },

  /**
   * Converts a Sync record ID to a Places GUID.
   */
  recordIdToGuid(recordId) {
    return ROOT_RECORD_ID_TO_GUID[recordId] || recordId;
  },

  /**
   * Fetches the record IDs for a folder's children, ordered by their position
   * within the folder.
   */
  fetchChildRecordIds(parentRecordId) {
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(parentRecordId);
    let parentGuid = BookmarkSyncUtils.recordIdToGuid(parentRecordId);

    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: fetchChildRecordIds", async function(db) {
        let childGuids = await fetchChildGuids(db, parentGuid);
        return childGuids.map(guid =>
          BookmarkSyncUtils.guidToRecordId(guid)
        );
      }
    );
  },

  /**
   * Returns an array of `{ recordId, syncable }` tuples for all items in
   * `requestedRecordIds`. If any requested ID is a folder, all its descendants
   * will be included. Ancestors of non-syncable items are not included; if
   * any are missing on the server, the requesting client will need to make
   * another repair request.
   *
   * Sync calls this method to respond to incoming bookmark repair requests
   * and upload items that are missing on the server.
   */
  fetchRecordIdsForRepair(requestedRecordIds) {
    let requestedGuids = requestedRecordIds.map(BookmarkSyncUtils.recordIdToGuid);
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: fetchRecordIdsForRepair", async function(db) {
        let rows = await db.executeCached(`
          WITH RECURSIVE
          syncedItems(id) AS (
            SELECT b.id FROM moz_bookmarks b
            WHERE b.guid IN ('menu________', 'toolbar_____', 'unfiled_____',
                             'mobile______')
            UNION ALL
            SELECT b.id FROM moz_bookmarks b
            JOIN syncedItems s ON b.parent = s.id
          ),
          descendants(id) AS (
            SELECT b.id FROM moz_bookmarks b
            WHERE b.guid IN (${requestedGuids.map(guid => JSON.stringify(guid)).join(",")})
            UNION ALL
            SELECT b.id FROM moz_bookmarks b
            JOIN descendants d ON d.id = b.parent
          )
          SELECT b.guid, s.id NOT NULL AS syncable
          FROM descendants d
          JOIN moz_bookmarks b ON b.id = d.id
          LEFT JOIN syncedItems s ON s.id = d.id
          `);
        return rows.map(row => {
          let recordId = BookmarkSyncUtils.guidToRecordId(row.getResultByName("guid"));
          let syncable = !!row.getResultByName("syncable");
          return { recordId, syncable };
        });
      }
    );
  },

  /**
   * Migrates an array of `{ recordId, modified }` tuples from the old JSON-based
   * tracker to the new sync change counter. `modified` is when the change was
   * added to the old tracker, in milliseconds.
   *
   * Sync calls this method before the first bookmark sync after the Places
   * schema migration.
   */
  migrateOldTrackerEntries(entries) {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: migrateOldTrackerEntries", function(db) {
        return db.executeTransaction(async function() {
          // Mark all existing bookmarks as synced, and clear their change
          // counters to avoid a full upload on the next sync. Note that
          // this means we'll miss changes made between startup and the first
          // post-migration sync, as well as changes made on a new release
          // channel that weren't synced before the user downgraded. This is
          // unfortunate, but no worse than the behavior of the old tracker.
          //
          // We also likely have bookmarks that don't exist on the server,
          // because the old tracker missed them. We'll eventually fix the
          // server once we decide on a repair strategy.
          await db.executeCached(`
            WITH RECURSIVE
            syncedItems(id) AS (
              SELECT b.id FROM moz_bookmarks b
              WHERE b.guid IN ('menu________', 'toolbar_____', 'unfiled_____',
                               'mobile______')
              UNION ALL
              SELECT b.id FROM moz_bookmarks b
              JOIN syncedItems s ON b.parent = s.id
            )
            UPDATE moz_bookmarks SET
              syncStatus = :syncStatus,
              syncChangeCounter = 0
            WHERE id IN syncedItems`,
            { syncStatus: PlacesUtils.bookmarks.SYNC_STATUS.NORMAL });

          await db.executeCached(`DELETE FROM moz_bookmarks_deleted`);

          await db.executeCached(`CREATE TEMP TABLE moz_bookmarks_tracked (
            guid TEXT PRIMARY KEY,
            time INTEGER
          )`);

          try {
            for (let { recordId, modified } of entries) {
              let guid = BookmarkSyncUtils.recordIdToGuid(recordId);
              if (!PlacesUtils.isValidGuid(guid)) {
                BookmarkSyncLog.warn(`migrateOldTrackerEntries: Ignoring ` +
                                     `change for invalid item ${guid}`);
                continue;
              }
              let time = PlacesUtils.toPRTime(Number.isFinite(modified) ?
                                              modified : Date.now());
              await db.executeCached(`
                INSERT OR IGNORE INTO moz_bookmarks_tracked (guid, time)
                VALUES (:guid, :time)`,
                { guid, time });
            }

            // Bump the change counter for existing tracked items.
            await db.executeCached(`
              INSERT OR REPLACE INTO moz_bookmarks (id, fk, type, parent,
                                                    position, title,
                                                    dateAdded, lastModified,
                                                    guid, syncChangeCounter,
                                                    syncStatus)
              SELECT b.id, b.fk, b.type, b.parent, b.position, b.title,
                     b.dateAdded, MAX(b.lastModified, t.time), b.guid,
                     b.syncChangeCounter + 1, b.syncStatus
              FROM moz_bookmarks b
              JOIN moz_bookmarks_tracked t ON b.guid = t.guid`);

            // Insert tombstones for nonexistent tracked items, using the most
            // recent deletion date for more accurate reconciliation. We assume
            // the tracked item belongs to a synced root.
            await db.executeCached(`
              INSERT OR REPLACE INTO moz_bookmarks_deleted (guid, dateRemoved)
              SELECT t.guid, MAX(IFNULL((SELECT dateRemoved FROM moz_bookmarks_deleted
                                         WHERE guid = t.guid), 0), t.time)
              FROM moz_bookmarks_tracked t
              LEFT JOIN moz_bookmarks b ON t.guid = b.guid
              WHERE b.guid IS NULL`);
          } finally {
            await db.executeCached(`DROP TABLE moz_bookmarks_tracked`);
          }
        });
      }
    );
  },

  /**
   * Reorders a folder's children, based on their order in the array of sync
   * IDs.
   *
   * Sync uses this method to reorder all synced children after applying all
   * incoming records.
   *
   * @return {Promise} resolved when reordering is complete.
   * @rejects if an error happens while reordering.
   * @throws if the arguments are invalid.
   */
  order(parentRecordId, childRecordIds) {
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(parentRecordId);
    if (!childRecordIds.length) {
      return undefined;
    }
    let parentGuid = BookmarkSyncUtils.recordIdToGuid(parentRecordId);
    if (parentGuid == PlacesUtils.bookmarks.rootGuid) {
      // Reordering roots doesn't make sense, but Sync will do this on the
      // first sync.
      return undefined;
    }
    let orderedChildrenGuids = childRecordIds.map(BookmarkSyncUtils.recordIdToGuid);
    return PlacesUtils.bookmarks.reorder(parentGuid, orderedChildrenGuids,
                                         { source: SOURCE_SYNC });
  },

  /**
   * Resolves to true if there are known sync changes.
   */
  havePendingChanges() {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: havePendingChanges", async function(db) {
        let rows = await db.executeCached(`
          WITH RECURSIVE
          syncedItems(id, guid, syncChangeCounter) AS (
            SELECT b.id, b.guid, b.syncChangeCounter
             FROM moz_bookmarks b
             WHERE b.guid IN ('menu________', 'toolbar_____', 'unfiled_____',
                              'mobile______')
            UNION ALL
            SELECT b.id, b.guid, b.syncChangeCounter
            FROM moz_bookmarks b
            JOIN syncedItems s ON b.parent = s.id
          ),
          changedItems(guid) AS (
            SELECT guid FROM syncedItems
            WHERE syncChangeCounter >= 1
            UNION ALL
            SELECT guid FROM moz_bookmarks_deleted
          )
          SELECT EXISTS(SELECT guid FROM changedItems) AS haveChanges`);
        return !!rows[0].getResultByName("haveChanges");
      }
    );
  },

  /**
   * Returns a changeset containing local bookmark changes since the last sync.
   *
   * @return {Promise} resolved once all items have been fetched.
   * @resolves to an object containing records for changed bookmarks, keyed by
   *           the record ID.
   * @see pullSyncChanges for the implementation, and markChangesAsSyncing for
   *      an explanation of why we update the sync status.
   */
  pullChanges() {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: pullChanges", pullSyncChanges);
  },

  /**
   * Updates the sync status of all "NEW" bookmarks to "NORMAL", so that Sync
   * can recover correctly after an interrupted sync.
   *
   * @param changeRecords
   *        A changeset containing sync change records, as returned by
   *        `pullChanges`.
   * @return {Promise} resolved once all records have been updated.
   */
  markChangesAsSyncing(changeRecords) {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: markChangesAsSyncing",
      db => markChangesAsSyncing(db, changeRecords)
    );
  },

  /**
   * Decrements the sync change counter, updates the sync status, and cleans up
   * tombstones for successfully synced items. Sync calls this method at the
   * end of each bookmark sync.
   *
   * @param changeRecords
   *        A changeset containing sync change records, as returned by
   *        `pullChanges`.
   * @return {Promise} resolved once all records have been updated.
   */
  pushChanges(changeRecords) {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: pushChanges", async function(db) {
        let skippedCount = 0;
        let weakCount = 0;
        let updateParams = [];
        let tombstoneGuidsToRemove = [];

        for (let recordId in changeRecords) {
          // Validate change records to catch coding errors.
          let changeRecord = validateChangeRecord(
            "BookmarkSyncUtils: pushChanges",
            changeRecords[recordId], {
              tombstone: { required: true },
              counter: { required: true },
              synced: { required: true },
            }
          );

          // Skip weakly uploaded records.
          if (!changeRecord.counter) {
            weakCount++;
            continue;
          }

          // Sync sets the `synced` flag for reconciled or successfully
          // uploaded items. If upload failed, ignore the change; we'll
          // try again on the next sync.
          if (!changeRecord.synced) {
            skippedCount++;
            continue;
          }

          let guid = BookmarkSyncUtils.recordIdToGuid(recordId);
          if (changeRecord.tombstone) {
            tombstoneGuidsToRemove.push(guid);
          } else {
            updateParams.push({
              guid,
              syncChangeDelta: changeRecord.counter,
              syncStatus: PlacesUtils.bookmarks.SYNC_STATUS.NORMAL,
            });
          }
        }

        // Reduce the change counter and update the sync status for
        // reconciled and uploaded items. If the bookmark was updated
        // during the sync, its change counter will still be > 0 for the
        // next sync.
        if (updateParams.length || tombstoneGuidsToRemove.length) {
          await db.executeTransaction(async function() {
            if (updateParams.length) {
              await db.executeCached(`
                UPDATE moz_bookmarks
                SET syncChangeCounter = MAX(syncChangeCounter - :syncChangeDelta, 0),
                    syncStatus = :syncStatus
                WHERE guid = :guid`,
                updateParams);
              // and if there are *both* bookmarks and tombstones for these
              // items, we nuke the tombstones.
              // This should be unlikely, but bad if it happens.
              let dupedGuids = updateParams.map(({ guid }) => guid);
              await removeUndeletedTombstones(db, dupedGuids);
            }
            await removeTombstones(db, tombstoneGuidsToRemove);
          });
        }

        BookmarkSyncLog.debug(`pushChanges: Processed change records`,
                              { weak: weakCount,
                                skipped: skippedCount,
                                updated: updateParams.length });
      }
    );
  },

  /**
   * Removes items from the database. Sync buffers incoming tombstones, and
   * calls this method to apply them at the end of each sync. Deletion
   * happens in three steps:
   *
   *  1. Remove all non-folder items. Deleting a folder on a remote client
   *     uploads tombstones for the folder and its children at the time of
   *     deletion. This preserves any new children we've added locally since
   *     the last sync.
   *  2. Reparent remaining children to the tombstoned folder's parent. This
   *     bumps the change counter for the children and their new parent.
   *  3. Remove the tombstoned folder. Because we don't do this in a
   *     transaction, the user might move new items into the folder before we
   *     can remove it. In that case, we keep the folder and upload the new
   *     subtree to the server.
   *
   * See the comment above `BookmarksStore::deletePending` for the details on
   * why delete works the way it does.
   */
  remove(recordIds) {
    if (!recordIds.length) {
      return null;
    }

    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: remove",
      async function(db) {
        let folderGuids = [];
        for (let recordId of recordIds) {
          if (recordId in ROOT_RECORD_ID_TO_GUID) {
            BookmarkSyncLog.warn(`remove: Refusing to remove root ${recordId}`);
            continue;
          }
          let guid = BookmarkSyncUtils.recordIdToGuid(recordId);
          let bookmarkItem = await PlacesUtils.bookmarks.fetch(guid);
          if (!bookmarkItem) {
            BookmarkSyncLog.trace(`remove: Item ${guid} already removed`);
            continue;
          }
          let kind = await getKindForItem(db, bookmarkItem);
          if (kind == BookmarkSyncUtils.KINDS.FOLDER) {
            folderGuids.push(bookmarkItem.guid);
            continue;
          }
          let wasRemoved = await deleteSyncedAtom(bookmarkItem);
          if (wasRemoved) {
             BookmarkSyncLog.trace(`remove: Removed item ${guid} with ` +
                                   `kind ${kind}`);
          }
        }

        for (let guid of folderGuids) {
          let bookmarkItem = await PlacesUtils.bookmarks.fetch(guid);
          if (!bookmarkItem) {
            BookmarkSyncLog.trace(`remove: Folder ${guid} already removed`);
            continue;
          }
          let wasRemoved = await deleteSyncedFolder(db, bookmarkItem);
          if (wasRemoved) {
            BookmarkSyncLog.trace(`remove: Removed folder ${bookmarkItem.guid}`);
          }
        }

        // TODO (Bug 1313890): Refactor the bookmarks engine to pull change records
        // before uploading, instead of returning records to merge into the engine's
        // initial changeset.
        return pullSyncChanges(db);
      }
    );
  },

  /**
   * Increments the change counter of a non-folder item and its parent. Sync
   * calls this method to override a remote deletion for an item that's changed
   * locally.
   *
   * @param recordId
   *        The record ID to revive.
   * @return {Promise} resolved once the change counters have been updated.
   * @resolves to `null` if the item doesn't exist or is a folder. Otherwise,
   *           resolves to an object containing new change records for the item
   *           and its parent. The bookmarks engine merges these records into
   *           the changeset for the current sync.
   */
  async touch(recordId) {
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(recordId);
    let guid = BookmarkSyncUtils.recordIdToGuid(recordId);

    let bookmarkItem = await PlacesUtils.bookmarks.fetch(guid);
    if (!bookmarkItem) {
      return null;
    }
    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: touch",
      async function(db) {
        let kind = await getKindForItem(db, bookmarkItem);
        if (kind == BookmarkSyncUtils.KINDS.FOLDER) {
          // We avoid reviving folders since reviving them properly would require
          // reviving their children as well. Unfortunately, this is the wrong
          // choice in the case of a bookmark restore where the bookmarks engine
          // fails to wipe the server. In that case, if the server has the folder
          // as deleted, we *would* want to reupload this folder. This is mitigated
          // by the fact that `remove` moves any undeleted children to the
          // grandparent when deleting the parent.
          return null;
        }
        return touchSyncBookmark(db, bookmarkItem);
      }
    );
  },

  /**
   * Returns true for record IDs that are considered roots.
   */
  isRootRecordID(id) {
    return ROOT_RECORD_ID_TO_GUID.hasOwnProperty(id);
  },

  /**
   * Removes all bookmarks and tombstones from the database. Sync calls this
   * method when it receives a command from a remote client to wipe all stored
   * data.
   *
   * @return {Promise} resolved once all items have been removed.
   */
  wipe() {
    return PlacesUtils.bookmarks.eraseEverything({
      source: SOURCE_SYNC,
    });
  },

  /**
   * Marks all bookmarks as "NEW" and removes all tombstones. Unlike `wipe`,
   * this keeps all existing bookmarks, and only clears their sync change
   * tracking info.
   *
   * @return {Promise} resolved once all items have been updated.
   */
  reset() {
    return PlacesUtils.withConnectionWrapper(
      "BookmarkSyncUtils: reset", function(db) {
        return db.executeTransaction(async function() {
          await BookmarkSyncUtils.resetSyncMetadata(db, SOURCE_SYNC);
        });
      }
    );
  },

  /**
   * De-dupes an item by changing its record ID to match the ID on the server.
   * Sync calls this method when it detects an incoming item is a duplicate of
   * an existing local item.
   *
   * Note that this method doesn't move the item if the local and remote sync
   * IDs are different. That happens after de-duping, when the bookmarks engine
   * calls `update` to update the item.
   *
   * @param localRecordId
   *        The local ID to change.
   * @param remoteRecordId
   *        The remote ID that should replace the local ID.
   * @param remoteParentRecordId
   *        The remote record's parent ID.
   * @return {Promise} resolved once the ID has been changed.
   * @resolves to an object containing new change records for the old item,
   *           the local parent, and the remote parent if different from the
   *           local parent. The bookmarks engine merges these records into the
   *           changeset for the current sync.
   */
  dedupe(localRecordId, remoteRecordId, remoteParentRecordId) {
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(localRecordId);
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(remoteRecordId);
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS.recordId(remoteParentRecordId);

    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: dedupe", db =>
      dedupeSyncBookmark(db, BookmarkSyncUtils.recordIdToGuid(localRecordId),
                         BookmarkSyncUtils.recordIdToGuid(remoteRecordId),
                         BookmarkSyncUtils.recordIdToGuid(remoteParentRecordId))
    );
  },

  /**
   * Updates a bookmark with synced properties. Only Sync should call this
   * method; other callers should use `Bookmarks.update`.
   *
   * The following properties are supported:
   *  - kind: Optional.
   *  - guid: Required.
   *  - parentGuid: Optional; reparents the bookmark if specified.
   *  - title: Optional.
   *  - url: Optional.
   *  - tags: Optional; replaces all existing tags.
   *  - keyword: Optional.
   *  - description: Optional.
   *  - loadInSidebar: Optional.
   *  - query: Optional.
   *
   * @param info
   *        object representing a bookmark-item, as defined above.
   *
   * @return {Promise} resolved when the update is complete.
   * @resolves to an object representing the updated bookmark.
   * @rejects if it's not possible to update the given bookmark.
   * @throws if the arguments are invalid.
   */
  update(info) {
    let updateInfo = validateSyncBookmarkObject("BookmarkSyncUtils: update",
      info, { recordId: { required: true } });

    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: update",
      db => updateSyncBookmark(db, updateInfo));
  },

  /**
   * Inserts a synced bookmark into the tree. Only Sync should call this
   * method; other callers should use `Bookmarks.insert`.
   *
   * The following properties are supported:
   *  - kind: Required.
   *  - guid: Required.
   *  - parentGuid: Required.
   *  - url: Required for bookmarks.
   *  - query: A smart bookmark query string, optional.
   *  - tags: An optional array of tag strings.
   *  - keyword: An optional keyword string.
   *  - description: An optional description string.
   *  - loadInSidebar: An optional boolean; defaults to false.
   *
   * Sync doesn't set the index, since it appends and reorders children
   * after applying all incoming items.
   *
   * @param info
   *        object representing a synced bookmark.
   *
   * @return {Promise} resolved when the creation is complete.
   * @resolves to an object representing the created bookmark.
   * @rejects if it's not possible to create the requested bookmark.
   * @throws if the arguments are invalid.
   */
  insert(info) {
    let insertInfo = validateNewBookmark("BookmarkSyncUtils: insert", info);

    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: insert",
      db => insertSyncBookmark(db, insertInfo));
  },

  /**
   * Fetches a Sync bookmark object for an item in the tree. The object contains
   * the following properties, depending on the item's kind:
   *
   *  - kind (all): A string representing the item's kind.
   *  - recordId (all): The item's record ID.
   *  - parentRecordId (all): The record ID of the item's parent.
   *  - parentTitle (all): The title of the item's parent, used for de-duping.
   *    Omitted for the Places root and parents with empty titles.
   *  - dateAdded (all): Timestamp in milliseconds, when the bookmark was added
   *    or created on a remote device if known.
   *  - title ("bookmark", "folder", "livemark", "query"): The item's title.
   *    Omitted if empty.
   *  - url ("bookmark", "query"): The item's URL.
   *  - tags ("bookmark", "query"): An array containing the item's tags.
   *  - keyword ("bookmark"): The bookmark's keyword, if one exists.
   *  - description ("bookmark", "folder", "livemark"): The item's description.
   *    Omitted if one isn't set.
   *  - loadInSidebar ("bookmark", "query"): Whether to load the bookmark in
   *    the sidebar. Always `false` for queries.
   *  - feed ("livemark"): A `URL` object pointing to the livemark's feed URL.
   *  - site ("livemark"): A `URL` object pointing to the livemark's site URL,
   *    or `null` if one isn't set.
   *  - childRecordIds ("folder"): An array containing the record IDs of the item's
   *    children, used to determine child order.
   *  - folder ("query"): The tag folder name, if this is a tag query.
   *  - query ("query"): The smart bookmark query name, if this is a smart
   *    bookmark.
   *  - index ("separator"): The separator's position within its parent.
   */
  async fetch(recordId) {
    let guid = BookmarkSyncUtils.recordIdToGuid(recordId);
    let bookmarkItem = await PlacesUtils.bookmarks.fetch(guid);
    if (!bookmarkItem) {
      return null;
    }
    return PlacesUtils.withConnectionWrapper("BookmarkSyncUtils: fetch",
      async function(db) {
        // Convert the Places bookmark object to a Sync bookmark and add
        // kind-specific properties. Titles are required for bookmarks,
        // folders, and livemarks; optional for queries, and omitted for
        // separators.
        let kind = await getKindForItem(db, bookmarkItem);
        let item;
        switch (kind) {
          case BookmarkSyncUtils.KINDS.BOOKMARK:
            item = await fetchBookmarkItem(db, bookmarkItem);
            break;

          case BookmarkSyncUtils.KINDS.QUERY:
            item = await fetchQueryItem(db, bookmarkItem);
            break;

          case BookmarkSyncUtils.KINDS.FOLDER:
            item = await fetchFolderItem(db, bookmarkItem);
            break;

          case BookmarkSyncUtils.KINDS.LIVEMARK:
            item = await fetchLivemarkItem(db, bookmarkItem);
            break;

          case BookmarkSyncUtils.KINDS.SEPARATOR:
            item = await placesBookmarkToSyncBookmark(db, bookmarkItem);
            item.index = bookmarkItem.index;
            break;

          default:
            throw new Error(`Unknown bookmark kind: ${kind}`);
        }

        // Sync uses the parent title for de-duping. All Sync bookmark objects
        // except the Places root should have this property.
        if (bookmarkItem.parentGuid) {
          let parent = await PlacesUtils.bookmarks.fetch(bookmarkItem.parentGuid);
          item.parentTitle = parent.title || "";
        }

        return item;
      }
    );
  },

  /**
   * Returns the sync change counter increment for a change source constant.
   */
  determineSyncChangeDelta(source) {
    // Don't bump the change counter when applying changes made by Sync, to
    // avoid sync loops.
    return source == PlacesUtils.bookmarks.SOURCES.SYNC ? 0 : 1;
  },

  /**
   * Returns the sync status for a new item inserted by a change source.
   */
  determineInitialSyncStatus(source) {
    if (source == PlacesUtils.bookmarks.SOURCES.SYNC) {
      // Incoming bookmarks are "NORMAL", since they already exist on the server.
      return PlacesUtils.bookmarks.SYNC_STATUS.NORMAL;
    }
    if (source == PlacesUtils.bookmarks.SOURCES.RESTORE_ON_STARTUP) {
      // If the user restores from a backup, or Places automatically recovers
      // from a corrupt database, all prior sync tracking is lost. Setting the
      // status to "UNKNOWN" allows Sync to reconcile restored bookmarks with
      // those on the server.
      return PlacesUtils.bookmarks.SYNC_STATUS.UNKNOWN;
    }
    // For all other sources, mark items as "NEW". We'll update their statuses
    // to "NORMAL" after the first sync.
    return PlacesUtils.bookmarks.SYNC_STATUS.NEW;
  },

  /**
   * An internal helper that bumps the change counter for all bookmarks with
   * a given URL. This is used to update bookmarks when adding or changing a
   * tag or keyword entry.
   *
   * @param db
   *        the Sqlite.jsm connection handle.
   * @param url
   *        the bookmark URL object.
   * @param syncChangeDelta
   *        the sync change counter increment.
   * @return {Promise} resolved when the counters have been updated.
   */
  addSyncChangesForBookmarksWithURL(db, url, syncChangeDelta) {
    if (!url || !syncChangeDelta) {
      return Promise.resolve();
    }
    return db.executeCached(`
      UPDATE moz_bookmarks
        SET syncChangeCounter = syncChangeCounter + :syncChangeDelta
      WHERE type = :type AND
            fk = (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND
                  url = :url)`,
      { syncChangeDelta, type: PlacesUtils.bookmarks.TYPE_BOOKMARK,
        url: url.href });
  },

  /**
   * Returns `0` if no sensible timestamp could be found.
   * Otherwise, returns the earliest sensible timestamp between `existingMillis`
   * and `serverMillis`.
   */
  ratchetTimestampBackwards(existingMillis, serverMillis, lowerBound = BookmarkSyncUtils.EARLIEST_BOOKMARK_TIMESTAMP) {
    const possible = [+existingMillis, +serverMillis].filter(n => !isNaN(n) && n > lowerBound);
    if (!possible.length) {
      return 0;
    }
    return Math.min(...possible);
  },

  /**
   * Rebuilds the left pane query for the mobile root under "All Bookmarks" if
   * necessary. Sync calls this method at the end of each bookmark sync. This
   * code should eventually move to `PlacesUIUtils#maybeRebuildLeftPane`; see
   * bug 647605.
   *
   * - If there are no mobile bookmarks, the query will not be created, or
   *   will be removed if it already exists.
   * - If there are mobile bookmarks, the query will be created if it doesn't
   *   exist, or will be updated with the correct title and URL otherwise.
   */
  async ensureMobileQuery() {
    let db = await PlacesUtils.promiseDBConnection();

    let mobileChildGuids = await fetchChildGuids(db, PlacesUtils.bookmarks.mobileGuid);
    let hasMobileBookmarks = mobileChildGuids.length > 0;

    Services.prefs.setBoolPref(MOBILE_BOOKMARKS_PREF, hasMobileBookmarks);
  },

  /**
   * Fetches an array of GUIDs for items that have an annotation set with the
   * given value.
   */
  async fetchGuidsWithAnno(anno, val) {
    let db = await PlacesUtils.promiseDBConnection();
    return fetchGuidsWithAnno(db, anno, val);
  },
});

XPCOMUtils.defineLazyGetter(this, "HistorySyncLog", () => {
  return Log.repository.getLogger("Sync.Engine.History.HistorySyncUtils");
});

XPCOMUtils.defineLazyGetter(this, "BookmarkSyncLog", () => {
  // Use a sub-log of the bookmarks engine, so setting the level for that
  // engine also adjust the level of this log.
  return Log.repository.getLogger("Sync.Engine.Bookmarks.BookmarkSyncUtils");
});

function validateSyncBookmarkObject(name, input, behavior) {
  return PlacesUtils.validateItemProperties(name,
    PlacesUtils.SYNC_BOOKMARK_VALIDATORS, input, behavior);
}

// Validates a sync change record as returned by `pullChanges` and passed to
// `pushChanges`.
function validateChangeRecord(name, changeRecord, behavior) {
  return PlacesUtils.validateItemProperties(name,
    PlacesUtils.SYNC_CHANGE_RECORD_VALIDATORS, changeRecord, behavior);
}

// Similar to the private `fetchBookmarksByParent` implementation in
// `Bookmarks.jsm`.
var fetchChildGuids = async function(db, parentGuid) {
  let rows = await db.executeCached(`
    SELECT guid
    FROM moz_bookmarks
    WHERE parent = (
      SELECT id FROM moz_bookmarks WHERE guid = :parentGuid
    )
    ORDER BY position`,
    { parentGuid }
  );
  return rows.map(row => row.getResultByName("guid"));
};

// A helper for whenever we want to know if a GUID doesn't exist in the places
// database. Primarily used to detect orphans on incoming records.
var GUIDMissing = async function(guid) {
  try {
    await PlacesUtils.promiseItemId(guid);
    return false;
  } catch (ex) {
    if (ex.message == "no item found for the given GUID") {
      return true;
    }
    throw ex;
  }
};

// Legacy tag queries may use a `place:` URL that refers to the tag folder ID.
// When we apply a synced tag query from a remote client, we need to update the
// URL to point to the local tag.
function updateTagQueryFolder(db, info) {
  if (info.kind != BookmarkSyncUtils.KINDS.QUERY || !info.folder || !info.url ||
      info.url.protocol != "place:") {
    return info;
  }

  let params = new URLSearchParams(info.url.pathname);
  let type = +params.get("type");
  if (type != Ci.nsINavHistoryQueryOptions.RESULTS_AS_TAG_CONTENTS) {
    return info;
  }

  BookmarkSyncLog.debug(`updateTagQueryFolder: Tag query folder: ${info.folder}`);

  // Rewrite the query to directly reference the tag.
  params.delete("queryType");
  params.delete("type");
  params.delete("folder");
  params.set("tag", info.folder);
  info.url = new URL(info.url.protocol + params);
  return info;
}

async function annotateOrphan(item, requestedParentRecordId) {
  let guid = BookmarkSyncUtils.recordIdToGuid(item.recordId);
  let itemId = await PlacesUtils.promiseItemId(guid);
  PlacesUtils.annotations.setItemAnnotation(itemId,
    BookmarkSyncUtils.SYNC_PARENT_ANNO, requestedParentRecordId, 0,
    PlacesUtils.annotations.EXPIRE_NEVER,
    SOURCE_SYNC);
}

var reparentOrphans = async function(db, item) {
  if (!item.kind || item.kind != BookmarkSyncUtils.KINDS.FOLDER) {
    return;
  }
  let orphanGuids = await fetchGuidsWithAnno(db,
                                             BookmarkSyncUtils.SYNC_PARENT_ANNO,
                                             item.recordId);
  let folderGuid = BookmarkSyncUtils.recordIdToGuid(item.recordId);
  BookmarkSyncLog.debug(`reparentOrphans: Reparenting ${
    JSON.stringify(orphanGuids)} to ${item.recordId}`);
  for (let i = 0; i < orphanGuids.length; ++i) {
    try {
      // Reparenting can fail if we have a corrupted or incomplete tree
      // where an item's parent is one of its descendants.
      BookmarkSyncLog.trace(`reparentOrphans: Attempting to move item ${
        orphanGuids[i]} to new parent ${item.recordId}`);
      await PlacesUtils.bookmarks.update({
        guid: orphanGuids[i],
        parentGuid: folderGuid,
        index: PlacesUtils.bookmarks.DEFAULT_INDEX,
        source: SOURCE_SYNC,
      });
    } catch (ex) {
      BookmarkSyncLog.error(`reparentOrphans: Failed to reparent item ${
        orphanGuids[i]} to ${item.recordId}`, ex);
    }
  }
};

// Inserts a synced bookmark into the database.
async function insertSyncBookmark(db, insertInfo) {
  let requestedParentRecordId = insertInfo.parentRecordId;
  let requestedParentGuid =
    BookmarkSyncUtils.recordIdToGuid(insertInfo.parentRecordId);
  let isOrphan = await GUIDMissing(requestedParentGuid);

  // Default to "unfiled" for new bookmarks if the parent doesn't exist.
  if (!isOrphan) {
    BookmarkSyncLog.debug(`insertSyncBookmark: Item ${
      insertInfo.recordId} is not an orphan`);
  } else {
    BookmarkSyncLog.debug(`insertSyncBookmark: Item ${
      insertInfo.recordId} is an orphan: parent ${
      insertInfo.parentRecordId} doesn't exist; reparenting to unfiled`);
    insertInfo.parentRecordId = "unfiled";
  }

  // If we're inserting a tag query, make sure the tag exists and fix the
  // folder ID to refer to the local tag folder.
  insertInfo = await updateTagQueryFolder(db, insertInfo);

  let newItem;
  if (insertInfo.kind == BookmarkSyncUtils.KINDS.LIVEMARK) {
    newItem = await insertSyncLivemark(db, insertInfo);
  } else {
    let bookmarkInfo = syncBookmarkToPlacesBookmark(insertInfo);
    let bookmarkItem = await PlacesUtils.bookmarks.insert(bookmarkInfo);
    newItem = await insertBookmarkMetadata(db, bookmarkItem, insertInfo);
  }

  if (!newItem) {
    return null;
  }

  // If the item is an orphan, annotate it with its real parent record ID.
  if (isOrphan) {
    await annotateOrphan(newItem, requestedParentRecordId);
  }

  // Reparent all orphans that expect this folder as the parent.
  await reparentOrphans(db, newItem);

  return newItem;
}

// Inserts a synced livemark.
async function insertSyncLivemark(db, insertInfo) {
  if (!insertInfo.feed) {
    BookmarkSyncLog.debug(`insertSyncLivemark: ${
      insertInfo.recordId} missing feed URL`);
    return null;
  }
  let livemarkInfo = syncBookmarkToPlacesBookmark(insertInfo);
  let parentIsLivemark = await getAnno(db, livemarkInfo.parentGuid,
                                       PlacesUtils.LMANNO_FEEDURI);
  if (parentIsLivemark) {
    // A livemark can't be a descendant of another livemark.
    BookmarkSyncLog.debug(`insertSyncLivemark: Invalid parent ${
      insertInfo.parentRecordId}; skipping livemark record ${
      insertInfo.recordId}`);
    return null;
  }

  let livemarkItem = await PlacesUtils.livemarks.addLivemark(livemarkInfo);

  return insertBookmarkMetadata(db, livemarkItem, insertInfo);
}

// Keywords are a 1 to 1 mapping between strings and pairs of (URL, postData).
// (the postData is not synced, so we ignore it). Sync associates keywords with
// bookmarks, which is not really accurate. -- We might already have a keyword
// with that name, or we might already have another bookmark with that URL with
// a different keyword, etc.
//
// If we don't handle those cases by removing the conflicting keywords first,
// the insertion  will fail, and the keywords will either be wrong, or missing.
// This function handles those cases.
function removeConflictingKeywords(bookmarkURL, newKeyword) {
  return PlacesUtils.withConnectionWrapper(
    "BookmarkSyncUtils: removeConflictingKeywords", async function(db) {
      let entryForURL = await PlacesUtils.keywords.fetch({
        url: bookmarkURL.href,
      });
      if (entryForURL && entryForURL.keyword !== newKeyword) {
        await PlacesUtils.keywords.remove({
          keyword: entryForURL.keyword,
          source: SOURCE_SYNC,
        });
        // This will cause us to reupload this record for this sync, but
        // without it, we will risk data corruption.
        await BookmarkSyncUtils.addSyncChangesForBookmarksWithURL(
          db, entryForURL.url, 1);
      }
      if (!newKeyword) {
        return;
      }
      let entryForNewKeyword = await PlacesUtils.keywords.fetch({
        keyword: newKeyword
      });
      if (entryForNewKeyword) {
        await PlacesUtils.keywords.remove({
          keyword: entryForNewKeyword.keyword,
          source: SOURCE_SYNC,
        });
        await BookmarkSyncUtils.addSyncChangesForBookmarksWithURL(
          db, entryForNewKeyword.url, 1);
      }
    }
  );
}

// Sets annotations, keywords, and tags on a new bookmark. Returns a Sync
// bookmark object.
async function insertBookmarkMetadata(db, bookmarkItem, insertInfo) {
  let itemId = await PlacesUtils.promiseItemId(bookmarkItem.guid);
  let newItem = await placesBookmarkToSyncBookmark(db, bookmarkItem);

  if (insertInfo.query) {
    PlacesUtils.annotations.setItemAnnotation(itemId,
      BookmarkSyncUtils.SMART_BOOKMARKS_ANNO, insertInfo.query, 0,
      PlacesUtils.annotations.EXPIRE_NEVER,
      SOURCE_SYNC);
    newItem.query = insertInfo.query;
  }

  try {
    newItem.tags = tagItem(bookmarkItem, insertInfo.tags);
  } catch (ex) {
    BookmarkSyncLog.warn(`insertBookmarkMetadata: Error tagging item ${
      insertInfo.recordId}`, ex);
  }

  if (insertInfo.keyword) {
    await removeConflictingKeywords(bookmarkItem.url, insertInfo.keyword);
    await PlacesUtils.keywords.insert({
      keyword: insertInfo.keyword,
      url: bookmarkItem.url.href,
      source: SOURCE_SYNC,
    });
    newItem.keyword = insertInfo.keyword;
  }

  if (insertInfo.description) {
    PlacesUtils.annotations.setItemAnnotation(itemId,
      BookmarkSyncUtils.DESCRIPTION_ANNO, insertInfo.description, 0,
      PlacesUtils.annotations.EXPIRE_NEVER,
      SOURCE_SYNC);
    newItem.description = insertInfo.description;
  }

  if (insertInfo.loadInSidebar) {
    PlacesUtils.annotations.setItemAnnotation(itemId,
      BookmarkSyncUtils.SIDEBAR_ANNO, insertInfo.loadInSidebar, 0,
      PlacesUtils.annotations.EXPIRE_NEVER,
      SOURCE_SYNC);
    newItem.loadInSidebar = insertInfo.loadInSidebar;
  }

  return newItem;
}

// Determines the Sync record kind for an existing bookmark.
async function getKindForItem(db, item) {
  switch (item.type) {
    case PlacesUtils.bookmarks.TYPE_FOLDER: {
      let isLivemark = await getAnno(db, item.guid,
                                     PlacesUtils.LMANNO_FEEDURI);
      return isLivemark ? BookmarkSyncUtils.KINDS.LIVEMARK :
                          BookmarkSyncUtils.KINDS.FOLDER;
    }
    case PlacesUtils.bookmarks.TYPE_BOOKMARK:
      return item.url.protocol == "place:" ?
             BookmarkSyncUtils.KINDS.QUERY :
             BookmarkSyncUtils.KINDS.BOOKMARK;

    case PlacesUtils.bookmarks.TYPE_SEPARATOR:
      return BookmarkSyncUtils.KINDS.SEPARATOR;
  }
  return null;
}

// Returns the `nsINavBookmarksService` bookmark type constant for a Sync
// record kind.
function getTypeForKind(kind) {
  switch (kind) {
    case BookmarkSyncUtils.KINDS.BOOKMARK:
    case BookmarkSyncUtils.KINDS.QUERY:
      return PlacesUtils.bookmarks.TYPE_BOOKMARK;

    case BookmarkSyncUtils.KINDS.FOLDER:
    case BookmarkSyncUtils.KINDS.LIVEMARK:
      return PlacesUtils.bookmarks.TYPE_FOLDER;

    case BookmarkSyncUtils.KINDS.SEPARATOR:
      return PlacesUtils.bookmarks.TYPE_SEPARATOR;
  }
  throw new Error(`Unknown bookmark kind: ${kind}`);
}

// Determines if a livemark should be reinserted. Returns true if `updateInfo`
// specifies different feed or site URLs; false otherwise.
var shouldReinsertLivemark = async function(updateInfo) {
  let hasFeed = updateInfo.hasOwnProperty("feed");
  let hasSite = updateInfo.hasOwnProperty("site");
  if (!hasFeed && !hasSite) {
    return false;
  }
  let guid = BookmarkSyncUtils.recordIdToGuid(updateInfo.recordId);
  let livemark = await PlacesUtils.livemarks.getLivemark({
    guid,
  });
  if (hasFeed) {
    let feedURI = PlacesUtils.toURI(updateInfo.feed);
    if (!livemark.feedURI.equals(feedURI)) {
      return true;
    }
  }
  if (hasSite) {
    if (!updateInfo.site) {
      return !!livemark.siteURI;
    }
    let siteURI = PlacesUtils.toURI(updateInfo.site);
    if (!livemark.siteURI || !siteURI.equals(livemark.siteURI)) {
      return true;
    }
  }
  return false;
};

async function updateSyncBookmark(db, updateInfo) {
  let guid = BookmarkSyncUtils.recordIdToGuid(updateInfo.recordId);
  let oldBookmarkItem = await PlacesUtils.bookmarks.fetch(guid);
  if (!oldBookmarkItem) {
    throw new Error(`Bookmark with record ID ${
      updateInfo.recordId} does not exist`);
  }

  if (updateInfo.hasOwnProperty("dateAdded")) {
    let newDateAdded = BookmarkSyncUtils.ratchetTimestampBackwards(
      oldBookmarkItem.dateAdded, updateInfo.dateAdded);
    if (!newDateAdded || newDateAdded === oldBookmarkItem.dateAdded) {
      delete updateInfo.dateAdded;
    } else {
      updateInfo.dateAdded = newDateAdded;
    }
  }

  let shouldReinsert = false;
  let oldKind = await getKindForItem(db, oldBookmarkItem);
  if (updateInfo.hasOwnProperty("kind") && updateInfo.kind != oldKind) {
    // If the item's aren't the same kind, we can't update the record;
    // we must remove and reinsert.
    shouldReinsert = true;
    if (BookmarkSyncLog.level <= Log.Level.Warn) {
      let oldRecordId = BookmarkSyncUtils.guidToRecordId(oldBookmarkItem.guid);
      BookmarkSyncLog.warn(`updateSyncBookmark: Local ${
        oldRecordId} kind = ${oldKind}; remote ${
        updateInfo.recordId} kind = ${
        updateInfo.kind}. Deleting and recreating`);
    }
  } else if (oldKind == BookmarkSyncUtils.KINDS.LIVEMARK) {
    // Similarly, if we're changing a livemark's site or feed URL, we need to
    // reinsert.
    shouldReinsert = await shouldReinsertLivemark(updateInfo);
    if (BookmarkSyncLog.level <= Log.Level.Debug) {
      let oldRecordId = BookmarkSyncUtils.guidToRecordId(oldBookmarkItem.guid);
      BookmarkSyncLog.debug(`updateSyncBookmark: Local ${
        oldRecordId} and remote ${
        updateInfo.recordId} livemarks have different URLs`);
    }
  }

  if (shouldReinsert) {
    if (!updateInfo.hasOwnProperty("dateAdded")) {
      updateInfo.dateAdded = oldBookmarkItem.dateAdded.getTime();
    }
    let newInfo = validateNewBookmark("BookmarkSyncUtils: reinsert",
                                      updateInfo);
    await PlacesUtils.bookmarks.remove({
      guid,
      source: SOURCE_SYNC,
    });
    // A reinsertion likely indicates a confused client, since there aren't
    // public APIs for changing livemark URLs or an item's kind (e.g., turning
    // a folder into a separator while preserving its annos and position).
    // This might be a good case to repair later; for now, we assume Sync has
    // passed a complete record for the new item, and don't try to merge
    // `oldBookmarkItem` with `updateInfo`.
    return insertSyncBookmark(db, newInfo);
  }

  let isOrphan = false, requestedParentRecordId;
  if (updateInfo.hasOwnProperty("parentRecordId")) {
    requestedParentRecordId = updateInfo.parentRecordId;
    let oldParentRecordId =
      BookmarkSyncUtils.guidToRecordId(oldBookmarkItem.parentGuid);
    if (requestedParentRecordId != oldParentRecordId) {
      let oldId = await PlacesUtils.promiseItemId(oldBookmarkItem.guid);
      if (PlacesUtils.isRootItem(oldId)) {
        throw new Error(`Cannot move Places root ${oldId}`);
      }
      let requestedParentGuid =
        BookmarkSyncUtils.recordIdToGuid(requestedParentRecordId);
      isOrphan = await GUIDMissing(requestedParentGuid);
      if (!isOrphan) {
        BookmarkSyncLog.debug(`updateSyncBookmark: Item ${
          updateInfo.recordId} is not an orphan`);
      } else {
        // Don't move the item if the new parent doesn't exist. Instead, mark
        // the item as an orphan. We'll annotate it with its real parent after
        // updating.
        BookmarkSyncLog.trace(`updateSyncBookmark: Item ${
          updateInfo.recordId} is an orphan: could not find parent ${
          requestedParentRecordId}`);
        delete updateInfo.parentRecordId;
      }
    } else {
      // If the parent is the same, just omit it so that `update` doesn't do
      // extra work.
      delete updateInfo.parentRecordId;
    }
  }

  updateInfo = await updateTagQueryFolder(db, updateInfo);

  let bookmarkInfo = syncBookmarkToPlacesBookmark(updateInfo);
  let newBookmarkItem = shouldUpdateBookmark(bookmarkInfo) ?
                        await PlacesUtils.bookmarks.update(bookmarkInfo) :
                        oldBookmarkItem;
  let newItem = await updateBookmarkMetadata(db, oldBookmarkItem,
                                             newBookmarkItem, updateInfo);

  // If the item is an orphan, annotate it with its real parent record ID.
  if (isOrphan) {
    await annotateOrphan(newItem, requestedParentRecordId);
  }

  // Reparent all orphans that expect this folder as the parent.
  await reparentOrphans(db, newItem);

  return newItem;
}

// Updates tags, keywords, and annotations for an existing bookmark. Returns a
// Sync bookmark object.
async function updateBookmarkMetadata(db, oldBookmarkItem,
                                      newBookmarkItem,
                                      updateInfo) {
  let itemId = await PlacesUtils.promiseItemId(newBookmarkItem.guid);
  let newItem = await placesBookmarkToSyncBookmark(db, newBookmarkItem);

  try {
    newItem.tags = tagItem(newBookmarkItem, updateInfo.tags);
  } catch (ex) {
    BookmarkSyncLog.warn(`updateBookmarkMetadata: Error tagging item ${
      updateInfo.recordId}`, ex);
  }

  if (updateInfo.hasOwnProperty("keyword")) {
    // Unconditionally remove the old keyword.
    await removeConflictingKeywords(oldBookmarkItem.url, updateInfo.keyword);
    if (updateInfo.keyword) {
      await PlacesUtils.keywords.insert({
        keyword: updateInfo.keyword,
        url: newItem.url.href,
        source: SOURCE_SYNC,
      });
    }
    newItem.keyword = updateInfo.keyword;
  }

  if (updateInfo.hasOwnProperty("description")) {
    if (updateInfo.description) {
      PlacesUtils.annotations.setItemAnnotation(itemId,
        BookmarkSyncUtils.DESCRIPTION_ANNO, updateInfo.description, 0,
        PlacesUtils.annotations.EXPIRE_NEVER,
        SOURCE_SYNC);
    } else {
      PlacesUtils.annotations.removeItemAnnotation(itemId,
        BookmarkSyncUtils.DESCRIPTION_ANNO, SOURCE_SYNC);
    }
    newItem.description = updateInfo.description;
  }

  if (updateInfo.hasOwnProperty("loadInSidebar")) {
    if (updateInfo.loadInSidebar) {
      PlacesUtils.annotations.setItemAnnotation(itemId,
        BookmarkSyncUtils.SIDEBAR_ANNO, updateInfo.loadInSidebar, 0,
        PlacesUtils.annotations.EXPIRE_NEVER,
        SOURCE_SYNC);
    } else {
      PlacesUtils.annotations.removeItemAnnotation(itemId,
        BookmarkSyncUtils.SIDEBAR_ANNO, SOURCE_SYNC);
    }
    newItem.loadInSidebar = updateInfo.loadInSidebar;
  }

  if (updateInfo.hasOwnProperty("query")) {
    PlacesUtils.annotations.setItemAnnotation(itemId,
      BookmarkSyncUtils.SMART_BOOKMARKS_ANNO, updateInfo.query, 0,
      PlacesUtils.annotations.EXPIRE_NEVER,
      SOURCE_SYNC);
    newItem.query = updateInfo.query;
  }

  return newItem;
}

function validateNewBookmark(name, info) {
  let insertInfo = validateSyncBookmarkObject(name, info,
    { kind: { required: true },
      recordId: { required: true },
      url: { requiredIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                                BookmarkSyncUtils.KINDS.QUERY ].includes(b.kind),
            validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                            BookmarkSyncUtils.KINDS.QUERY ].includes(b.kind) },
      parentRecordId: { required: true },
      title: { validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                               BookmarkSyncUtils.KINDS.QUERY,
                               BookmarkSyncUtils.KINDS.FOLDER,
                               BookmarkSyncUtils.KINDS.LIVEMARK ].includes(b.kind) ||
                             b.title === "" },
      query: { validIf: b => b.kind == BookmarkSyncUtils.KINDS.QUERY },
      folder: { validIf: b => b.kind == BookmarkSyncUtils.KINDS.QUERY },
      tags: { validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                              BookmarkSyncUtils.KINDS.QUERY ].includes(b.kind) },
      keyword: { validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                                 BookmarkSyncUtils.KINDS.QUERY ].includes(b.kind) },
      description: { validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                                     BookmarkSyncUtils.KINDS.QUERY,
                                     BookmarkSyncUtils.KINDS.FOLDER,
                                     BookmarkSyncUtils.KINDS.LIVEMARK ].includes(b.kind) },
      loadInSidebar: { validIf: b => [ BookmarkSyncUtils.KINDS.BOOKMARK,
                                       BookmarkSyncUtils.KINDS.QUERY ].includes(b.kind) },
      feed: { validIf: b => b.kind == BookmarkSyncUtils.KINDS.LIVEMARK },
      site: { validIf: b => b.kind == BookmarkSyncUtils.KINDS.LIVEMARK },
      dateAdded: { required: false }
    });

  return insertInfo;
}

async function fetchGuidsWithAnno(db, anno, val) {
  let rows = await db.executeCached(`
    SELECT b.guid FROM moz_items_annos a
    JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id
    JOIN moz_bookmarks b ON b.id = a.item_id
    WHERE n.name = :anno AND
          a.content = :val`,
    { anno, val });
  return rows.map(row => row.getResultByName("guid"));
}

// Returns the value of an item's annotation, or `null` if it's not set.
async function getAnno(db, guid, anno) {
  let rows = await db.executeCached(`
    SELECT a.content FROM moz_items_annos a
    JOIN moz_anno_attributes n ON n.id = a.anno_attribute_id
    JOIN moz_bookmarks b ON b.id = a.item_id
    WHERE b.guid = :guid AND
          n.name = :anno`,
    { guid, anno });
  return rows.length ? rows[0].getResultByName("content") : null;
}

function tagItem(item, tags) {
  if (!item.url) {
    return [];
  }

  // Remove leading and trailing whitespace, then filter out empty tags.
  let newTags = tags ? tags.map(tag => tag.trim()).filter(Boolean) : [];

  // Removing the last tagged item will also remove the tag. To preserve
  // tag IDs, we temporarily tag a dummy URI, ensuring the tags exist.
  let dummyURI = PlacesUtils.toURI("about:weave#BStore_tagURI");
  let bookmarkURI = PlacesUtils.toURI(item.url.href);
  if (newTags && newTags.length > 0)
    PlacesUtils.tagging.tagURI(dummyURI, newTags, SOURCE_SYNC);
  PlacesUtils.tagging.untagURI(bookmarkURI, null, SOURCE_SYNC);
  if (newTags && newTags.length > 0)
    PlacesUtils.tagging.tagURI(bookmarkURI, newTags, SOURCE_SYNC);
  PlacesUtils.tagging.untagURI(dummyURI, null, SOURCE_SYNC);

  return newTags;
}

// `PlacesUtils.bookmarks.update` checks if we've supplied enough properties,
// but doesn't know about additional livemark properties. We check this to avoid
// having it throw in case we only pass properties like `{ guid, feedURI }`.
function shouldUpdateBookmark(bookmarkInfo) {
  return bookmarkInfo.hasOwnProperty("parentGuid") ||
         bookmarkInfo.hasOwnProperty("title") ||
         bookmarkInfo.hasOwnProperty("url");
}

// Converts a Places bookmark or livemark to a Sync bookmark. This function
// maps Places GUIDs to record IDs and filters out extra Places properties like
// date added, last modified, and index.
async function placesBookmarkToSyncBookmark(db, bookmarkItem) {
  let item = {};

  for (let prop in bookmarkItem) {
    switch (prop) {
      // Record IDs are identical to Places GUIDs for all items except roots.
      case "guid":
        item.recordId = BookmarkSyncUtils.guidToRecordId(bookmarkItem.guid);
        break;

      case "parentGuid":
        item.parentRecordId =
          BookmarkSyncUtils.guidToRecordId(bookmarkItem.parentGuid);
        break;

      // Sync uses kinds instead of types, which distinguish between folders,
      // livemarks, bookmarks, and queries.
      case "type":
        item.kind = await getKindForItem(db, bookmarkItem);
        break;

      case "title":
      case "url":
        item[prop] = bookmarkItem[prop];
        break;

      case "dateAdded":
        item[prop] = new Date(bookmarkItem[prop]).getTime();
        break;

      // Livemark objects contain additional properties. The feed URL is
      // required; the site URL is optional.
      case "feedURI":
        item.feed = new URL(bookmarkItem.feedURI.spec);
        break;

      case "siteURI":
        if (bookmarkItem.siteURI) {
          item.site = new URL(bookmarkItem.siteURI.spec);
        }
        break;
    }
  }

  return item;
}

// Converts a Sync bookmark object to a Places bookmark or livemark object.
// This function maps record IDs to Places GUIDs, and filters out extra Sync
// properties like keywords, tags, and descriptions. Returns an object that can
// be passed to `PlacesUtils.livemarks.addLivemark` or
// `PlacesUtils.bookmarks.{insert, update}`.
function syncBookmarkToPlacesBookmark(info) {
  let bookmarkInfo = {
    source: SOURCE_SYNC,
  };

  for (let prop in info) {
    switch (prop) {
      case "kind":
        bookmarkInfo.type = getTypeForKind(info.kind);
        break;

      // Convert record IDs to Places GUIDs for roots.
      case "recordId":
        bookmarkInfo.guid = BookmarkSyncUtils.recordIdToGuid(info.recordId);
        break;

      case "dateAdded":
        bookmarkInfo.dateAdded = new Date(info.dateAdded);
        break;

      case "parentRecordId":
        bookmarkInfo.parentGuid =
          BookmarkSyncUtils.recordIdToGuid(info.parentRecordId);
        // Instead of providing an index, Sync reorders children at the end of
        // the sync using `BookmarkSyncUtils.order`. We explicitly specify the
        // default index here to prevent `PlacesUtils.bookmarks.update` and
        // `PlacesUtils.livemarks.addLivemark` from throwing.
        bookmarkInfo.index = PlacesUtils.bookmarks.DEFAULT_INDEX;
        break;

      case "title":
      case "url":
        bookmarkInfo[prop] = info[prop];
        break;

      // Livemark-specific properties.
      case "feed":
        bookmarkInfo.feedURI = PlacesUtils.toURI(info.feed);
        break;

      case "site":
        if (info.site) {
          bookmarkInfo.siteURI = PlacesUtils.toURI(info.site);
        }
        break;
    }
  }

  return bookmarkInfo;
}

// Creates and returns a Sync bookmark object containing the bookmark's
// tags, keyword, description, and whether it loads in the sidebar.
var fetchBookmarkItem = async function(db, bookmarkItem) {
  let item = await placesBookmarkToSyncBookmark(db, bookmarkItem);

  if (!item.title) {
    item.title = "";
  }

  item.tags = PlacesUtils.tagging.getTagsForURI(
    PlacesUtils.toURI(bookmarkItem.url), {});

  let keywordEntry = await PlacesUtils.keywords.fetch({
    url: bookmarkItem.url,
  });
  if (keywordEntry) {
    item.keyword = keywordEntry.keyword;
  }

  let description = await getAnno(db, bookmarkItem.guid,
                                  BookmarkSyncUtils.DESCRIPTION_ANNO);
  if (description) {
    item.description = description;
  }

  item.loadInSidebar = !!(await getAnno(db, bookmarkItem.guid,
                                        BookmarkSyncUtils.SIDEBAR_ANNO));

  return item;
};

// Creates and returns a Sync bookmark object containing the folder's
// description and children.
async function fetchFolderItem(db, bookmarkItem) {
  let item = await placesBookmarkToSyncBookmark(db, bookmarkItem);

  if (!item.title) {
    item.title = "";
  }

  let description = await getAnno(db, bookmarkItem.guid,
                                  BookmarkSyncUtils.DESCRIPTION_ANNO);
  if (description) {
    item.description = description;
  }

  let childGuids = await fetchChildGuids(db, bookmarkItem.guid);
  item.childRecordIds = childGuids.map(guid =>
    BookmarkSyncUtils.guidToRecordId(guid)
  );

  return item;
}

// Creates and returns a Sync bookmark object containing the livemark's
// description, children (none), feed URI, and site URI.
async function fetchLivemarkItem(db, bookmarkItem) {
  let item = await placesBookmarkToSyncBookmark(db, bookmarkItem);

  if (!item.title) {
    item.title = "";
  }

  let description = await getAnno(db, bookmarkItem.guid,
                                  BookmarkSyncUtils.DESCRIPTION_ANNO);
  if (description) {
    item.description = description;
  }

  let feedAnno = await getAnno(db, bookmarkItem.guid,
                               PlacesUtils.LMANNO_FEEDURI);
  item.feed = new URL(feedAnno);

  let siteAnno = await getAnno(db, bookmarkItem.guid,
                               PlacesUtils.LMANNO_SITEURI);
  if (siteAnno) {
    item.site = new URL(siteAnno);
  }

  return item;
}

// Creates and returns a Sync bookmark object containing the query's tag
// folder name and smart bookmark query ID.
async function fetchQueryItem(db, bookmarkItem) {
  let item = await placesBookmarkToSyncBookmark(db, bookmarkItem);

  let description = await getAnno(db, bookmarkItem.guid,
                                  BookmarkSyncUtils.DESCRIPTION_ANNO);
  if (description) {
    item.description = description;
  }

  let params = new URLSearchParams(bookmarkItem.url.pathname);
  let tags = params.getAll("tag");
  if (tags.length == 1) {
    item.folder = tags[0];
  }

  let query = await getAnno(db, bookmarkItem.guid,
                            BookmarkSyncUtils.SMART_BOOKMARKS_ANNO);
  if (query) {
    item.query = query;
  }

  return item;
}

function addRowToChangeRecords(row, changeRecords) {
  let guid = row.getResultByName("guid");
  if (!guid) {
    throw new Error(`Changed item missing GUID`);
  }
  let isTombstone = !!row.getResultByName("tombstone");
  let recordId = BookmarkSyncUtils.guidToRecordId(guid);
  if (recordId in changeRecords) {
    let existingRecord = changeRecords[recordId];
    if (existingRecord.tombstone == isTombstone) {
      // Should never happen: `moz_bookmarks.guid` has a unique index, and
      // `moz_bookmarks_deleted.guid` is the primary key.
      throw new Error(`Duplicate item or tombstone ${recordId} in changeset`);
    }
    if (!existingRecord.tombstone && isTombstone) {
      // Don't replace undeleted items with tombstones...
      BookmarkSyncLog.warn("addRowToChangeRecords: Ignoring tombstone for " +
                           "undeleted item", recordId);
      return;
    }
    // ...But replace undeleted tombstones with items.
    BookmarkSyncLog.warn("addRowToChangeRecords: Replacing tombstone for " +
                         "undeleted item", recordId);
  }
  let modifiedAsPRTime = row.getResultByName("modified");
  let modified = modifiedAsPRTime / MICROSECONDS_PER_SECOND;
  if (Number.isNaN(modified) || modified <= 0) {
    BookmarkSyncLog.error("addRowToChangeRecords: Invalid modified date for " +
                          recordId, modifiedAsPRTime);
    modified = 0;
  }
  changeRecords[recordId] = {
    modified,
    counter: row.getResultByName("syncChangeCounter"),
    status: row.getResultByName("syncStatus"),
    tombstone: isTombstone,
    synced: false,
  };
}

/**
 * Queries the database for synced bookmarks and tombstones, and returns a
 * changeset for the Sync bookmarks engine.
 *
 * @param db
 *        The Sqlite.jsm connection handle.
 * @return {Promise} resolved once all items have been fetched.
 * @resolves to an object containing records for changed bookmarks, keyed by
 *           the record ID.
 */
var pullSyncChanges = async function(db) {
  let changeRecords = {};

  let rows = await db.executeCached(`
    WITH RECURSIVE
    syncedItems(id, guid, modified, syncChangeCounter, syncStatus) AS (
      SELECT b.id, b.guid, b.lastModified, b.syncChangeCounter, b.syncStatus
       FROM moz_bookmarks b
       WHERE b.guid IN ('menu________', 'toolbar_____', 'unfiled_____',
                        'mobile______')
      UNION ALL
      SELECT b.id, b.guid, b.lastModified, b.syncChangeCounter, b.syncStatus
      FROM moz_bookmarks b
      JOIN syncedItems s ON b.parent = s.id
    )
    SELECT guid, modified, syncChangeCounter, syncStatus, 0 AS tombstone
    FROM syncedItems
    WHERE syncChangeCounter >= 1
    UNION ALL
    SELECT guid, dateRemoved AS modified, 1 AS syncChangeCounter,
           :deletedSyncStatus, 1 AS tombstone
    FROM moz_bookmarks_deleted`,
    { deletedSyncStatus: PlacesUtils.bookmarks.SYNC_STATUS.NORMAL });
  for (let row of rows) {
    addRowToChangeRecords(row, changeRecords);
  }

  return changeRecords;
};

var touchSyncBookmark = async function(db, bookmarkItem) {
  if (BookmarkSyncLog.level <= Log.Level.Trace) {
    BookmarkSyncLog.trace(
      `touch: Reviving item "${bookmarkItem.guid}" and marking parent ` +
      BookmarkSyncUtils.guidToRecordId(bookmarkItem.parentGuid) + ` as modified`);
  }

  // Bump the change counter of the item and its parent, so that we upload
  // both.
  await db.executeCached(`
    UPDATE moz_bookmarks SET
      syncChangeCounter = syncChangeCounter + 1
    WHERE guid IN (:guid, :parentGuid)`,
    { guid: bookmarkItem.guid, parentGuid: bookmarkItem.parentGuid });

  // TODO (Bug 1313890): Refactor the bookmarks engine to pull change records
  // before uploading, instead of returning records to merge into the engine's
  // initial changeset.
  return pullSyncChanges(db);
};

var dedupeSyncBookmark = async function(db, localGuid, remoteGuid,
                                               remoteParentGuid) {
  let rows = await db.executeCached(`
    SELECT b.id, b.type, p.id AS parentId, p.guid AS parentGuid, b.syncStatus
    FROM moz_bookmarks b
    JOIN moz_bookmarks p ON p.id = b.parent
    WHERE b.guid = :localGuid`,
    { localGuid });
  if (!rows.length) {
    throw new Error(`Local item ${localGuid} does not exist`);
  }

  let localId = rows[0].getResultByName("id");
  let localParentId = rows[0].getResultByName("parentId");
  let bookmarkType = rows[0].getResultByName("type");
  if (PlacesUtils.isRootItem(localId)) {
    throw new Error(`Cannot de-dupe local root ${localGuid}`);
  }

  let localParentGuid = rows[0].getResultByName("parentGuid");
  let sameParent = localParentGuid == remoteParentGuid;
  let modified = PlacesUtils.toPRTime(Date.now());

  await db.executeTransaction(async function() {
    // Change the item's old GUID to the new remote GUID. This will throw a
    // constraint error if the remote GUID already exists locally.
    BookmarkSyncLog.debug("dedupeSyncBookmark: Switching local GUID " +
                          localGuid + " to incoming GUID " + remoteGuid);
    await db.executeCached(`UPDATE moz_bookmarks
      SET guid = :remoteGuid
      WHERE id = :localId`,
      { remoteGuid, localId });
    PlacesUtils.invalidateCachedGuidFor(localId);

    // And mark the parent as being modified. Given we de-dupe based on the
    // parent *name* it's possible the item having its GUID changed has a
    // different parent from the incoming record.
    // So we need to return a change record for the parent, and bump its
    // counter to ensure we don't lose the change if the current sync is
    // interrupted.
    await db.executeCached(`UPDATE moz_bookmarks
      SET syncChangeCounter = syncChangeCounter + 1
      WHERE guid = :localParentGuid`,
      { localParentGuid });

    // And we also add the parent as reflected in the incoming record as the
    // de-dupe process might have used an existing item in a different folder.
    // This statement is a no-op if we don't have the new parent yet, but that's
    // fine: applying the record will add our special SYNC_PARENT_ANNO
    // annotation and move it to unfiled. If the parent arrives in the future
    // (either this Sync or a later one), the item will be reparented. Note that
    // this scenario will still leave us with inconsistent client and server
    // states; the incoming record on the server references a parent that isn't
    // the actual parent locally - see bug 1297955.
    if (!sameParent) {
      await db.executeCached(`UPDATE moz_bookmarks
        SET syncChangeCounter = syncChangeCounter + 1
        WHERE guid = :remoteParentGuid`,
        { remoteParentGuid });
    }

    // The local, duplicate ID is always deleted on the server - but for
    // bookmarks it is a logical delete.
    let localSyncStatus = rows[0].getResultByName("syncStatus");
    if (localSyncStatus == PlacesUtils.bookmarks.SYNC_STATUS.NORMAL) {
      await db.executeCached(`
        INSERT INTO moz_bookmarks_deleted (guid, dateRemoved)
        VALUES (:localGuid, :modified)`,
        { localGuid, modified });
    }
  });

  let observers = PlacesUtils.bookmarks.getObservers();
  notify(observers, "onItemChanged", [ localId, "guid", false,
                                       remoteGuid,
                                       modified,
                                       bookmarkType,
                                       localParentId,
                                       remoteGuid, remoteParentGuid,
                                       localGuid, SOURCE_SYNC
                                     ]);

  // TODO (Bug 1313890): Refactor the bookmarks engine to pull change records
  // before uploading, instead of returning records to merge into the engine's
  // initial changeset.
  let changeRecords = await pullSyncChanges(db);

  if (BookmarkSyncLog.level <= Log.Level.Debug && !sameParent) {
    let remoteParentRecordId = BookmarkSyncUtils.guidToRecordId(remoteParentGuid);
    if (!changeRecords.hasOwnProperty(remoteParentRecordId)) {
      BookmarkSyncLog.debug("dedupeSyncBookmark: Incoming duplicate item " +
                            remoteGuid + " specifies non-existing parent " +
                            remoteParentGuid);
    }
  }

  return changeRecords;
};

// Moves a synced folder's remaining children to its parent, and deletes the
// folder if it's empty.
async function deleteSyncedFolder(db, bookmarkItem) {
  // At this point, any member in the folder that remains is either a folder
  // pending deletion (which we'll get to in this function), or an item that
  // should not be deleted. To avoid deleting these items, we first move them
  // to the parent of the folder we're about to delete.
  let childGuids = await fetchChildGuids(db, bookmarkItem.guid);
  if (!childGuids.length) {
    // No children -- just delete the folder.
    return deleteSyncedAtom(bookmarkItem);
  }

  if (BookmarkSyncLog.level <= Log.Level.Trace) {
    BookmarkSyncLog.trace(
      `deleteSyncedFolder: Moving ${JSON.stringify(childGuids)} children of ` +
      `"${bookmarkItem.guid}" to grandparent
      "${BookmarkSyncUtils.guidToRecordId(bookmarkItem.parentGuid)}" before ` +
      `deletion`);
  }

  // Move children out of the parent and into the grandparent
  for (let guid of childGuids) {
    await PlacesUtils.bookmarks.update({
      guid,
      parentGuid: bookmarkItem.parentGuid,
      index: PlacesUtils.bookmarks.DEFAULT_INDEX,
      // `SYNC_REPARENT_REMOVED_FOLDER_CHILDREN` bumps the change counter for
      // the child and its new parent, without incrementing the bookmark
      // tracker's score.
      //
      // We intentionally don't check if the child is one we'll remove later,
      // so it's possible we'll bump the change counter of the closest living
      // ancestor when it's not needed. This avoids inconsistency if removal
      // is interrupted, since we don't run this operation in a transaction.
      source: PlacesUtils.bookmarks.SOURCES.SYNC_REPARENT_REMOVED_FOLDER_CHILDREN,
    });
  }

  // Delete the (now empty) parent
  try {
    await PlacesUtils.bookmarks.remove(bookmarkItem.guid, {
      preventRemovalOfNonEmptyFolders: true,
      // We don't want to bump the change counter for this deletion, because
      // a tombstone for the folder is already on the server.
      source: SOURCE_SYNC,
    });
  } catch (e) {
    // We failed, probably because someone added something to this folder
    // between when we got the children and now (or the database is corrupt,
    // or something else happened...) This is unlikely, but possible. To
    // avoid corruption in this case, we need to reupload the record to the
    // server.
    //
    // (Ideally this whole operation would be done in a transaction, and this
    // wouldn't be possible).
    BookmarkSyncLog.trace(`deleteSyncedFolder: Error removing parent ` +
                          `${bookmarkItem.guid} after reparenting children`, e);
    return false;
  }

  return true;
}

// Removes a synced bookmark or empty folder from the database.
var deleteSyncedAtom = async function(bookmarkItem) {
  try {
    await PlacesUtils.bookmarks.remove(bookmarkItem.guid, {
      preventRemovalOfNonEmptyFolders: true,
      source: SOURCE_SYNC,
    });
  } catch (ex) {
    // Likely already removed.
    BookmarkSyncLog.trace(`deleteSyncedAtom: Error removing ` +
                          bookmarkItem.guid, ex);
    return false;
  }

  return true;
};

/**
 * Updates the sync status on all "NEW" and "UNKNOWN" bookmarks to "NORMAL".
 *
 * We do this when pulling changes instead of in `pushChanges` to make sure
 * we write tombstones if a new item is deleted after an interrupted sync. (For
 * example, if a "NEW" record is uploaded or reconciled, then the app is closed
 * before Sync calls `pushChanges`).
 */
function markChangesAsSyncing(db, changeRecords) {
  let unsyncedGuids = [];
  for (let recordId in changeRecords) {
    if (changeRecords[recordId].tombstone) {
      continue;
    }
    if (changeRecords[recordId].status ==
        PlacesUtils.bookmarks.SYNC_STATUS.NORMAL) {
      continue;
    }
    let guid = BookmarkSyncUtils.recordIdToGuid(recordId);
    unsyncedGuids.push(JSON.stringify(guid));
  }
  if (!unsyncedGuids.length) {
    return Promise.resolve();
  }
  return db.execute(`
    UPDATE moz_bookmarks
    SET syncStatus = :syncStatus
    WHERE guid IN (${unsyncedGuids.join(",")})`,
    { syncStatus: PlacesUtils.bookmarks.SYNC_STATUS.NORMAL });
}

/**
 * Removes tombstones for successfully synced items.
 *
 * @return {Promise}
 */
var removeTombstones = function(db, guids) {
  if (!guids.length) {
    return Promise.resolve();
  }
  return db.execute(`
    DELETE FROM moz_bookmarks_deleted
    WHERE guid IN (${guids.map(guid => JSON.stringify(guid)).join(",")})`);
};

/**
 * Removes tombstones for successfully synced items where the specified GUID
 * exists in *both* the bookmarks and tombstones tables.
 *
 * @return {Promise}
 */
var removeUndeletedTombstones = function(db, guids) {
  if (!guids.length) {
    return Promise.resolve();
  }
  // sqlite can't join in a DELETE, so we use a subquery.
  return db.execute(`
    DELETE FROM moz_bookmarks_deleted
    WHERE guid IN (${guids.map(guid => JSON.stringify(guid)).join(",")})
    AND guid IN (SELECT guid from moz_bookmarks)`);
};
/**
 * Sends a bookmarks notification through the given observers.
 *
 * @param observers
 *        array of nsINavBookmarkObserver objects.
 * @param notification
 *        the notification name.
 * @param args
 *        array of arguments to pass to the notification.
 */
function notify(observers, notification, args = []) {
  for (let observer of observers) {
    try {
      observer[notification](...args);
    } catch (ex) {}
  }
}

// Sets the history sync ID and clears the last sync time.
async function setHistorySyncId(db, newSyncId) {
  await PlacesUtils.metadata.setWithConnection(db,
    HistorySyncUtils.SYNC_ID_META_KEY, newSyncId);

  await PlacesUtils.metadata.deleteWithConnection(db,
    HistorySyncUtils.LAST_SYNC_META_KEY);
}

// Sets the bookmarks sync ID and clears the last sync time.
async function setBookmarksSyncId(db, newSyncId) {
  await PlacesUtils.metadata.setWithConnection(db,
    BookmarkSyncUtils.SYNC_ID_META_KEY, newSyncId);

  await PlacesUtils.metadata.deleteWithConnection(db,
    BookmarkSyncUtils.LAST_SYNC_META_KEY,
    BookmarkSyncUtils.WIPE_REMOTE_META_KEY);
}

// Bumps the change counter and sets the given sync status for all bookmarks,
// removes all orphan annos, and drops stale tombstones.
async function resetAllSyncStatuses(db, syncStatus) {
  await db.execute(`
    UPDATE moz_bookmarks
    SET syncChangeCounter = 1,
        syncStatus = :syncStatus`,
    { syncStatus });

  // The orphan anno isn't meaningful after a restore, disconnect, or node
  // reassignment.
  await db.execute(`
    DELETE FROM moz_items_annos
    WHERE anno_attribute_id = (SELECT id FROM moz_anno_attributes
                               WHERE name = :orphanAnno)`,
    { orphanAnno: BookmarkSyncUtils.SYNC_PARENT_ANNO });

  // Drop stale tombstones.
  await db.execute("DELETE FROM moz_bookmarks_deleted");
}
