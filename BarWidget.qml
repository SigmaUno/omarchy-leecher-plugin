import QtQuick
import QtQuick.Shapes
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons

BarWidget {
    id: root
    moduleName: "leecher.media"

    /* IPC directory shared with the backend.  Resolved once to a literal path
     * via Quickshell.env() (no shell fragment) so the file watchers below can
     * watch it directly and no per-poll `sh -c cat` process is needed.  Mirrors
     * app.c init_ipc_dir(): $XDG_RUNTIME_DIR/leecher, else /tmp/leecher-<uid>.
     *
     * `$UID` is a shell variable that is normally NOT exported, so it must not
     * be used to derive the fallback path (it would collapse to the fixed name
     * `/tmp/leecher-`, letting another user pre-create it as a symlink).  The
     * numeric uid is instead read from `id -u` once (see numericUid/fetch). */
    property int numericUid: -1
    property string runtimeDir: (function () {
        var rd = Quickshell.env("XDG_RUNTIME_DIR");
        return (rd && rd !== "") ? (rd + "/leecher") : ("/tmp/leecher-" + root.numericUid);
    })()
    readonly property string statusFile: root.runtimeDir + "/status.json"
    readonly property string controlFile: root.runtimeDir + "/control"

    /* The backend only uses the IPC dir after validating that it exists, is a
     * real directory (not a symlink) and is owned by the current user, else it
     * falls back to a freshly mkdtemp'd dir.  Mirror that guard on the write
     * side: cache a stat of the directory and only submit control commands once
     * it is confirmed to be a directory owned by our numeric uid.  Until then
     * commands are dropped, which fails safe (no hostile path is ever written). */
    property bool runtimeDirSafe: false
    property bool restatNeeded: false
    Process {
        id: runtimeProbe
        command: ["sh", "-c", "id -u; ls -ldn \"" + root.runtimeDir + "\""]
        running: true
        stdout: SplitParser {
            onRead: data => {
                var raw = String(data);
                if (root.numericUid < 0) {
                    /* First line: `id -u`.  The fallback path depends on this, so
                     * once the uid is known mark the directory for a re-stat. */
                    var u = parseInt(raw.trim());
                    if (!isNaN(u) && u >= 0) {
                        root.numericUid = u;
                        root.runtimeDirSafe = false;
                        root.restatNeeded = true;
                    }
                } else {
                    /* `ls -ldn` line, e.g.
                     * "drwx------ 2 1000 1000 40 ... /run/user/1000/leecher"
                     * (perms, link count, owner uid, group gid, ...).  Must be a
                     * real directory (leading 'd', never a symlink 'l'),
                     * owner-writable, and owned by our numeric uid. */
                    var m = /^\s*([dl])([rwxsStT-]{9})\s+\d+\s+(\d+)/.exec(raw);
                    if (m) {
                        /* Owner bits are the first "rwx" (charset indices 0-2);
                         * the owner write bit is index 1.  A directory we do not
                         * own, a symlink, or a read-only path must never be
                         * written through. */
                        if (m[1] === "d" && m[2].charAt(1) === "w" && parseInt(m[3], 10) === root.numericUid) {
                            root.runtimeDirSafe = true;
                            root.restatNeeded = false;
                            root.refresh();
                        } else {
                            root.runtimeDirSafe = false;
                        }
                    }
                }
            }
        }
        onRunningChanged: {
            if (!running && root.restatNeeded) {
                /* The uid only just became known, so the command started with a
                 * stale fallback path.  Restart once against the corrected dir. */
                root.restatNeeded = false;
                runtimeProbe.running = true;
            }
        }
    }

    property string title: ""
    property string artist: ""
    property string album: ""
    property string libraryPath: ""
    property int durationMs: 0
    property bool hasTrack: false
    property bool autoplay: true
    property bool shuffle: false
    property bool repeatOne: false
    property int volume: 100
    property bool muted: false
    property string outputDevice: ""
    property var outputDevices: []
    property string coverSource: ""
    property int coverVersion: 0

    property int basePosition: 0
    property real baseTime: 0
    property bool playing: false
    property int lastSrvPos: 0
    property bool serverAdvancing: true

    property real positionMs: 0
    property bool popupOpen: false
    property bool contextOpen: false
    property bool libraryOpen: false
    property bool audioOpen: false
    property bool titleExpanded: false
    property bool hidden: false
    property bool hoverPeek: false
    property bool closed: false
    property int selectedTrackIndex: -1
    property int pendingPlayIndex: -1
    property string statusText: ""
    property int lastCommandId: 0
    property int pendingCommandId: -1
    property bool commandAcked: true
    property bool reloadOnAck: false
    property var tracks: []
    property var queue: []
    property string librarySearch: ""

    /* Multi-playlist library: every playlist stem in the library directory, the
     * one currently shown (viewed) and the one playback is running over
     * (playing). They differ when you browse playlist B while A keeps playing. */
    property var playlists: []
    property string viewedPlaylist: ""
    property string playingPlaylist: ""
    property bool addingPlaylist: false
    /* The library list shows the playlist that playback is actually running
     * over: only then do the "now playing" row and the queue strip apply. */
    readonly property bool viewingPlayingList: root.viewedPlaylist === root.playingPlaylist

    /* A directory scan stages its finds in "INCOMING >> <target> <<" for review.
     * incomingTarget is that <target> when such a playlist is being viewed. */
    readonly property string incomingPrefix: "INCOMING >> "
    readonly property string incomingSuffix: " <<"
    function incomingTargetOf(name) {
        var s = String(name || "");
        if (s.length > root.incomingPrefix.length + root.incomingSuffix.length &&
            s.indexOf(root.incomingPrefix) === 0 &&
            s.lastIndexOf(root.incomingSuffix) === s.length - root.incomingSuffix.length)
            return s.slice(root.incomingPrefix.length, s.length - root.incomingSuffix.length);
        return "";
    }
    readonly property string incomingTarget: root.incomingTargetOf(root.viewedPlaylist)
    readonly property bool viewingIncoming: root.incomingTarget !== ""
    readonly property color incomingColor: Qt.rgba(0.85, 0.53, 0.15, 1)
    property bool scanning: false
    property int scanCount: 0

    /* Linear interpolation between two colors, t in [0,1]. */
    function blend(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t,
                       a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t, 1);
    }

    /* 1-based position of a library index in the play queue, or 0 if absent. */
    function queuePosition(index) {
        for (var i = 0; i < root.queue.length; i++)
            if (root.queue[i] === index)
                return i + 1;
        return 0;
    }

    /* The queued track titles, in order, for the "Up next" strip. */
    readonly property string queueSummary: {
        var out = [];
        for (var i = 0; i < root.queue.length; i++) {
            var qi = root.queue[i];
            var name = "#" + (qi + 1);
            for (var j = 0; j < root.tracks.length; j++)
                if (root.tracks[j].index === qi) { name = root.tracks[j].title; break; }
            out.push(name);
        }
        return out.join("  ·  ");
    }

    /* Client-side library filter: case-insensitive substring over
     * title/artist/album. Each entry keeps its original `index` so playback,
     * edit and remove still address the right library row. */
    readonly property var filteredTracks: {
        var q = root.librarySearch.trim().toLowerCase();
        if (q === "")
            return root.tracks;
        var out = [];
        for (var i = 0; i < root.tracks.length; i++) {
            var t = root.tracks[i];
            if ((t.title + " " + t.artist + " " + t.album).toLowerCase().indexOf(q) !== -1)
                out.push(t);
        }
        return out;
    }

    property int actionsIndex: -1
    property var actionsAnchor: root
    property int editIndex: -1
    property bool editVisible: false
    property bool addVisible: false
    property string addType: "local"
    property string addPath: ""
    property string addUser: ""
    property string addHost: ""
    property string addUrl: ""
    /* "Scan directory for all music files": the path is treated as a folder and
     * every audio file in it is staged for review instead of added directly. */
    property bool addScan: false
    property string editTitle: ""
    property string editArtist: ""
    property string editAlbum: ""

    readonly property string playIcon: root.playing ? "\uf04c" : "\uf04b"
    readonly property bool effectiveHidden: root.hidden && !root.hoverPeek
    /* A closed widget collapses to a restorable sliver instead of vanishing: if
     * `visible` were disabled there would be no handle left to click and no way
     * to bring it back short of re-configuration.  Closing also stops every
     * timer and file watcher (see the `!root.closed` bindings below). */
    readonly property bool collapsed: root.closed || root.effectiveHidden

    /* Longest the title is allowed to grow before it is elided.  Shared by the
     * implicit-width math and the label's size so the widget's requested width
     * matches what the row actually draws (no trailing dead space or clipping). */
    readonly property real labelMaxWidth: Style.space(150)

    /* Transport-control borders: a muted trace of the bar foreground so the
     * buttons read as a set without a hard outline.  Foreground-derived (not
     * accent) and low-alpha so it re-tints with whatever theme is active. */
    readonly property var transportButtonBorder: Border.flat(
        Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.20),
        Math.max(1, Style.space(1)))

    /* Every transport/mode button in the popup is this exact square so the two
     * control rows read as one even grid -- no bespoke size for play/pause. */
    readonly property real controlButtonSize: Style.space(36)
    /* One cell of a segmented control (used by the transport group and the
     * mode group): centred glyph, its own hover / press highlight, tooltip. */
    component ControlSeg: Item {
        id: seg
        property string glyph: ""
        property bool active: false
        property color idle: Qt.darker(root.bar.foreground, 1.6)
        property string tip: ""
        signal act()
        height: parent ? parent.height : 0

        Rectangle {
            anchors.fill: parent
            color: segMouse.pressed
                ? Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.28)
                : segHover.hovered
                    ? Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.16)
                    : "transparent"
        }
        Text {
            anchors.centerIn: parent
            text: seg.glyph
            color: seg.active ? Color.accent : seg.idle
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.icon
        }
        HoverHandler { id: segHover }
        MouseArea {
            id: segMouse
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: seg.act()
        }
        PanelToolTip {
            visible: segHover.hovered && seg.tip !== ""
            text: seg.tip
        }
    }



    /* Size the widget from the glyph's real ink width (glyphSlot) instead of a
     * fixed barSize: the old formula requested barSize for the icon slot but the
     * icon is typically narrower, leaving an invisible strip of unused space on
     * the right so the title sat off-centre with asymmetric padding. */
    implicitWidth: root.collapsed
        ? Math.max(root.barSize, Style.space(12) + glyphSlot.width)
        : Style.space(24) + glyphSlot.width + Style.space(6) + root.labelMaxWidth + Style.space(18)
    implicitHeight: root.barSize

    function now() {
        return Date.now();
    }
    function currentPosition() {
        if (!root.playing)
            return root.basePosition;
        /* Only extrapolate while the backend is actually confirming progress.
         * If it has stalled (position not advancing) we race ahead and then snap
         * back every poll, which showed up as a 0:00/0:01 flicker. */
        if (!root.serverAdvancing)
            return root.basePosition;
        var pos = root.basePosition + (root.now() - root.baseTime);
        return Math.min(pos, root.durationMs || pos);
    }
    function clampPosition() {
        root.positionMs = root.currentPosition();
        /* Drive the scrubber from position only when the user is not dragging,
         * so the 500 ms timer never fights the handle under the finger.  The
         * slider holds its own drag state (PanelSlider.dragging) while moving. */
        if (!slider.dragging)
            slider.value = root.positionMs;
    }
    function enc(s) {
        var out = "", i, c, code, hex;
        for (i = 0; i < s.length; i++) {
            c = s.charAt(i);
            code = s.charCodeAt(i);
            if (c === "%" || c === "'" || code < 0x21 || code === 0x7f) {
                hex = code.toString(16).toUpperCase();
                if (hex.length === 1) hex = "0" + hex;
                out += "%" + hex;
            } else {
                out += c;
            }
        }
        return out;
    }
    function sendControlRaw(cmdLine) {
        if (!root.runtimeDirSafe)
            return;
        root.lastCommandId = (root.lastCommandId + 1) & 0x7fffffff;
        root.pendingCommandId = root.lastCommandId;
        root.commandAcked = false;
        /* Quote the literal control path so a runtime dir with spaces still
         * resolves to a single redirection target. `cmdLine` is either one
         * already-encoded token (enc()) or a list of encoded tokens joined by
         * literal spaces: the backend splits encoded tokens on spaces, so the
         * separator must never itself be encoded. */
        Quickshell.execDetached(["sh", "-c", "printf '%s\\n' '" + root.lastCommandId + " " + cmdLine + "' > \"" + root.controlFile + "\""]);
    }
    function sendControl(cmd) {
        /* Encode the payload so values (titles/artists/albums) can contain
         * newlines or shell-special characters without corrupting the line. */
        root.sendControlRaw(enc(cmd));
    }
    /* Single-command multi-field edit: each value is percent-encoded with enc()
     * (no literal spaces/quotes/newlines remain), so the three values are joined
     * by literal spaces into one control line that the backend splits and
     * applies in a single atomic library write. */
    function sendControlFields(idx, f1, f2, f3) {
        root.sendControlRaw("set_fields " + idx + " " + enc(f1) + " " + enc(f2) + " " + enc(f3));
    }
    function seekTo(ms) {
        root.sendControl("seek " + Math.round(ms));
    }
    function playPause() {
        root.sendControl("play_pause");
    }
    function next() {
        root.sendControl("next");
    }
    function previous() {
        root.sendControl("previous");
    }
    function playIndex(i) {
        root.sendControl("play " + i);
        root.close();
    }
    function playFromRow(i) {
        root.pendingPlayIndex = i;
        root.sendControl("play " + i);
    }
    /* Switch the library list to another playlist. Playback keeps running over
     * whatever it was; it only moves if the user then picks a track here.
     * Re-tapping "*" is allowed: the backend rebuilds it from the other
     * playlists on every view, so it always shows what's been added since. */
    function viewPlaylist(name) {
        if (name === root.viewedPlaylist && name !== "*")
            return;
        root.actionsIndex = -1;
        root.editVisible = false;
        root.clearLibrarySearch();
        /* Re-read the track list once the backend acks the switch: for "*" the
         * file is rebuilt under the same path, so a path-change reload alone
         * would miss it. */
        root.reloadOnAck = true;
        root.sendControl("playlist " + name);
    }
    function beginAddPlaylist() {
        root.addingPlaylist = true;
    }
    function commitAddPlaylist(name) {
        var n = String(name).trim();
        root.addingPlaylist = false;
        if (n === "")
            return;
        root.sendControl("playlist_new " + n);
    }
    function cancelAddPlaylist() {
        root.addingPlaylist = false;
    }
    function toggleAutoplay() {
        root.autoplay = !root.autoplay;
        root.sendControl(root.autoplay ? "autoplay on" : "autoplay off");
    }
    function toggleShuffle() {
        root.shuffle = !root.shuffle;
        root.sendControl(root.shuffle ? "shuffle on" : "shuffle off");
    }
    function toggleRepeatOne() {
        root.repeatOne = !root.repeatOne;
        root.sendControl(root.repeatOne ? "repeat one" : "repeat off");
    }
    function setVolume(v) {
        var clamped = Math.max(0, Math.min(100, Math.round(v)));
        root.volume = clamped;
        root.sendControl("volume " + clamped);
    }
    function toggleMute() {
        root.muted = !root.muted;
        root.sendControl(root.muted ? "mute on" : "mute off");
    }
    function queueTrack(i) {
        root.sendControl("queue " + i);
    }
    function unqueueTrack(i) {
        root.sendControl("unqueue " + i);
    }
    function clearQueue() {
        root.sendControl("queue_clear");
    }
    function selectOutput(name) {
        root.sendControl("output " + (name === "" ? "default" : name));
    }
    /* The audio button hints at non-default state so it is worth opening. */
    readonly property bool audioAdjusted: root.muted || root.volume !== 100 || root.outputDevice !== ""
    /* Bar tooltip text: track (or "Nothing playing"), plus the latest backend
     * status on its own line when it says something the title doesn't already.
     * Uses the bar's themed tooltip -- the old inline QtQuick ToolTip rendered
     * with the unstyled platform default. */
    function hoverTooltip() {
        if (!root.hasTrack)
            return root.statusText !== "" ? root.statusText : "Nothing playing";
        var base = root.title + (root.artist !== "" ? " — " + root.artist : "");
        var s = root.statusText;
        if (s !== "" && s.indexOf(root.title) === -1)
            return base + "\n" + s;
        return base;
    }
    function startEdit(i) {
        for (var k = 0; k < root.tracks.length; k++) {
            if (root.tracks[k].index === i) {
                root.editIndex = i;
                root.editTitle = root.tracks[k].title;
                root.editArtist = root.tracks[k].artist;
                root.editAlbum = root.tracks[k].album;
                root.editVisible = true;
                root.actionsIndex = -1;
                return;
            }
        }
    }
    function saveEdit() {
        if (root.editIndex < 0)
            return;
        /* Apply all three fields in one atomic backend write, then re-read the
         * list once the backend acks it: the file is rewritten in place
         * (mkstemp + rename), which the FileView watch does not always catch. */
        root.reloadOnAck = true;
        root.sendControlFields(root.editIndex, root.editTitle, root.editArtist, root.editAlbum);
        root.editVisible = false;
        root.editIndex = -1;
    }
    function cancelEdit() {
        root.editVisible = false;
        root.editIndex = -1;
        root.actionsIndex = -1;
    }
    function removeTrack(i) {
        /* Re-read the list once the backend acks the removal -- it rewrites the
         * playlist file in place (mkstemp + rename), which the FileView watch
         * does not always catch, so an immediate reload would just re-show the
         * track that is still on disk. */
        root.reloadOnAck = true;
        root.sendControl("remove " + i);
        root.actionsIndex = -1;
        root.editVisible = false;
    }
    /* The playlist a scan should stage into: the one being viewed, or, if
     * that is itself a staging list, its underlying target. Falls back to
     * "home" when viewing "*" or nothing. */
    function scanTargetPlaylist() {
        if (root.viewingIncoming)
            return root.incomingTarget;
        if (root.viewedPlaylist !== "" && root.viewedPlaylist !== "*")
            return root.viewedPlaylist;
        return "home";
    }
    function addLocalTrack(path) {
        var trimmed = path.trim();
        if (trimmed === "") {
            root.statusText = "Enter the path to an audio file first.";
            return;
        }
        /* sendControl() encodes the complete line before it reaches the
         * backend, so spaces and special characters in a local path remain a
         * single safe command argument. */
        if (root.addScan) {
            root.sendControlRaw("scan_local " + enc(root.scanTargetPlaylist()) + " " + enc(trimmed));
        } else {
            root.reloadOnAck = true;
            root.sendControl("add_local " + trimmed);
        }
        root.addPath = "";
        root.addVisible = false;
    }
    /* SSH ("ssh") and local-network ("network") sources share the same three
     * fields but are stored under distinct kinds.  Each value is enc()'d so it
     * contains no literal space; the tokens are joined by literal spaces that
     * the backend splits on. */
    function addSshTrack(user, host, path) {
        var u = user.trim();
        var h = host.trim();
        var p = path.trim();
        if (u === "" || h === "" || p === "") {
            root.statusText = root.addType === "ssh"
                ? "Enter the SSH user, host/IP, and remote path first."
                : "Enter the network user, host/IP, and path first.";
            return;
        }
        if (root.addScan) {
            root.sendControlRaw("scan_" + root.addType + " " + enc(root.scanTargetPlaylist()) +
                                " " + enc(u) + " " + enc(h) + " " + enc(p));
        } else {
            root.reloadOnAck = true;
            root.sendControlRaw("add_" + root.addType + " " + enc(u) + " " + enc(h) + " " + enc(p));
        }
        root.closeAddForm();
    }
    /* Move (accept) or discard (decline) staged tracks. `indices` is an array of
     * library indices in the "INCOMING >> ... <<" list currently being viewed. */
    function acceptIncoming(indices) {
        if (indices.length > 0) {
            root.reloadOnAck = true;
            root.sendControl("accept_incoming " + indices.join(" "));
        }
    }
    function declineIncoming(indices) {
        if (indices.length > 0) {
            root.reloadOnAck = true;
            root.sendControl("decline_incoming " + indices.join(" "));
        }
    }
    /* Indices of every row currently shown (after any filter), for shift-click. */
    function shownIncomingIndices() {
        return root.filteredTracks.map(function (t) { return t.index; });
    }
    function addHttpsTrack(url) {
        var trimmed = url.trim();
        if (trimmed === "") {
            root.statusText = "Enter an https:// URL first.";
            return;
        }
        if (trimmed.toLowerCase().indexOf("https://") !== 0) {
            root.statusText = "Only https:// URLs are supported.";
            return;
        }
        root.reloadOnAck = true;
        root.sendControlRaw("add_https " + enc(trimmed));
        root.closeAddForm();
    }
    function submitAdd() {
        if (root.addType === "local")
            root.addLocalTrack(addLocalField.text);
        else if (root.addType === "https")
            root.addHttpsTrack(addUrlField.text);
        else
            root.addSshTrack(addUserField.text, addHostField.text, addSshPathField.text);
    }
    function closeAddForm() {
        root.addPath = "";
        root.addUser = "";
        root.addHost = "";
        root.addUrl = "";
        root.addScan = false;
        root.addVisible = false;
    }
    /* file:// URLs delivered by a drag-and-drop carry percent-encoded path
     * segments; convert one back to a plain local path so the backend can stat
     * it.  Non-file URLs (http/smb/webdav) fall through to a bare path the
     * backend will reject as "not a regular file", which is fail-safe. */
    function urlToPath(url) {
        var s = String(url);
        if (s.indexOf("file://") === 0)
            s = s.substring(7);
        else if (s.indexOf("file:") === 0)
            s = s.substring(5);
        var out = "", i = 0;
        while (i < s.length) {
            var c = s.charAt(i);
            if (c === "%" && i + 2 < s.length && /^[0-9a-fA-F]{2}$/.test(s.substring(i + 1, i + 3))) {
                out += String.fromCharCode(parseInt(s.substring(i + 1, i + 3), 16));
                i += 3;
            } else {
                out += c;
                i++;
            }
        }
        return out;
    }
    function fmt(ms) {
        if (!ms || ms <= 0)
            return "0:00";
        var total = Math.floor(ms / 1000);
        var h = Math.floor(total / 3600);
        var m = Math.floor((total % 3600) / 60);
        var s = total % 60;
        /* Minutes are zero-padded only past the hour mark ("1:02:03"); below
         * an hour the leading digit is bare ("5:31"). Seconds always pad. */
        var mm = (h > 0 && m < 10) ? "0" + m : String(m);
        var ss = s < 10 ? "0" + s : String(s);
        return (h > 0 ? h + ":" : "") + mm + ":" + ss;
    }
    function close() {
        root.popupOpen = false;
        root.libraryOpen = false;
        root.audioOpen = false;
        root.actionsIndex = -1;
        root.editVisible = false;
        root.editIndex = -1;
        root.closeAddForm();
        root.pendingPlayIndex = -1;
        root.addingPlaylist = false;
        root.clearLibrarySearch();
    }
    function clearLibrarySearch() {
        root.librarySearch = "";
        if (typeof librarySearchField !== "undefined")
            librarySearchField.text = "";
    }
    /* Closing drops the widget to a collapsed sliver and halts all background
     * work (position timer + the two file watchers are bound to !closed); the
     * context menu on the sliver offers "Open widget" to restore it. */
    function setClosed(v) {
        if (root.closed === v)
            return;
        root.closed = v;
        if (v) {
            root.close();
            root.contextOpen = false;
            root.hoverPeek = false;
        } else {
            root.refresh();
            libraryFileView.reload();
        }
    }
    function refresh() {
        statusFileView.reload();
    }
    function updateStatus(raw) {
        var data;
        try {
            data = JSON.parse(raw);
        } catch (e) {
            return;
        }
        var newTitle = String(data.title || "");
        var newArtist = String(data.artist || "");
        var newAlbum = String(data.album || "");
        var newDur = Number(data.duration_ms || 0);
        var isPlayingNow = data.is_playing === true;
        var idx = -1;
        if (data.track_index !== undefined && data.track_index !== null)
            idx = Number(data.track_index);

        var trackChanged = newTitle !== root.title || newDur !== root.durationMs;
        var playingChanged = isPlayingNow !== root.playing;
        var srvPos = Number(data.position_ms || 0);

        /* Track whether the backend is confirming playback progress. If it stops
         * advancing, stop extrapolating so the clock doesn't flicker back and
         * forth between 0:00 and 0:01. */
        if (!isPlayingNow) {
            root.serverAdvancing = true;
        } else if (srvPos - root.lastSrvPos >= 200) {
            root.serverAdvancing = true;
        } else {
            root.serverAdvancing = false;
        }

        var estimate = root.currentPosition();

        if (trackChanged) {
            root.basePosition = srvPos;
            root.baseTime = root.now();
            root.serverAdvancing = true;
            root.coverVersion++;
            root.titleExpanded = false;
        } else if (playingChanged) {
            root.basePosition = root.currentPosition();
            root.baseTime = root.now();
            root.serverAdvancing = true;
        } else if (Math.abs(srvPos - estimate) > 1200) {
            root.basePosition = srvPos;
            root.baseTime = root.now();
        }

        root.title = newTitle;
        root.artist = newArtist !== "" ? newArtist : "No Artist";
        root.album = newAlbum !== "" ? newAlbum : "No Album";
        root.durationMs = newDur;
        root.playing = isPlayingNow;
        /* Continue the command-id sequence from wherever the backend already is,
         * so a widget reload against a still-running backend doesn't reissue ids
         * it would treat as duplicates of earlier commands. */
        if (root.lastCommandId === 0 && typeof data.cmd_id === "number" && data.cmd_id > 0)
            root.lastCommandId = Number(data.cmd_id);
        /* Command acknowledgment: when the backend echoes the id we sent with a
         * control write, a previously-issued command has been processed. Only
         * then do we let the backend's autoplay value override an optimistic
         * local toggle; otherwise the echo would wipe a not-yet-applied tweak. */
        if (data.cmd_id !== undefined && data.cmd_id !== null &&
            root.pendingCommandId >= 0 && Number(data.cmd_id) === root.pendingCommandId) {
            root.commandAcked = true;
            root.pendingCommandId = -1;
            if (root.reloadOnAck) {
                root.reloadOnAck = false;
                Qt.callLater(root.loadLibrary);
            }
        }
        if (root.pendingCommandId < 0) {
            root.autoplay = data.autoplay !== false;
            root.shuffle = data.shuffle === true;
            root.repeatOne = data.repeat_one === true;
            root.muted = data.muted === true;
            if (typeof data.volume === "number" && !volSlider.dragging)
                root.volume = data.volume;
        }
        root.statusText = data.status ? String(data.status) : "";
        root.hasTrack = root.title !== "";
        root.queue = Array.isArray(data.queue) ? data.queue : [];
        root.outputDevices = Array.isArray(data.outputs) ? data.outputs : [];
        root.outputDevice = typeof data.output === "string" ? data.output : "";
        root.coverSource = data.cover ? String(data.cover) : "";
        root.selectedTrackIndex = idx;
        if (isPlayingNow && root.pendingPlayIndex >= 0 && idx === root.pendingPlayIndex) {
            root.pendingPlayIndex = -1;
        }
        root.playlists = Array.isArray(data.playlists) ? data.playlists : [];
        root.viewedPlaylist = typeof data.viewed_playlist === "string" ? data.viewed_playlist : "";
        root.playingPlaylist = typeof data.playing_playlist === "string" ? data.playing_playlist : "";
        root.scanning = data.scanning === true;
        root.scanCount = typeof data.scan_count === "number" ? data.scan_count : 0;
        if (data.library) {
            var newLib = String(data.library);
            if (newLib !== root.libraryPath) {
                /* Playlist changed under us: drop stale per-list UI state. */
                root.pendingPlayIndex = -1;
                root.actionsIndex = -1;
                root.editVisible = false;
            }
            root.libraryPath = newLib;
        }
        root.lastSrvPos = srvPos;
        root.clampPosition();
    }
    function loadLibrary() {
        if (root.libraryPath === "")
            return;
        libraryFileView.reload();
    }
    function setTracks(raw) {
        var tracksList = [];
        try {
            var data = JSON.parse(raw);
            var arr = data && data.tracks ? data.tracks : [];
            for (var i = 0; i < arr.length; i++) {
                var t = arr[i] || {};
                tracksList.push({
                    title: String(t.title || "Untitled"),
                    artist: String(t.artist || ""),
                    album: String(t.album || ""),
                    index: i
                });
            }
        } catch (e) {}
        root.tracks = tracksList;
    }

    Component.onCompleted: refresh()

    Timer {
        id: posTimer
        interval: 500
        running: !root.closed
        repeat: true
        onTriggered: root.clampPosition()
    }

    /* Coalesce the flood of slider `moved` signals during a volume drag into
     * about eight control writes a second so the backend is not re-queuing
     * audio on every pixel. */
    Timer {
        id: volApply
        interval: 120
        repeat: false
        onTriggered: root.setVolume(root.volume)
    }

    /* Watch status.json with an in-process file watcher instead of re-spawning
     * `sh -c cat` every second.  `reload()`/`text()` read the file in-process,
     * so no shell or forked reader is involved.  The backend rewrites the file
     * atomically (mkstemp + rename); FileView re-watches the recreated path. */
    FileView {
        id: statusFileView
        path: root.statusFile
        watchChanges: !root.closed
        printErrors: false
        onFileChanged: reload()
        onLoaded: root.updateStatus(text())
    }

    Row {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.collapsed ? Style.space(12) : Style.space(24)
        anchors.rightMargin: root.collapsed ? 0 : Style.space(18)
        anchors.verticalCenter: parent.verticalCenter
        spacing: root.collapsed ? 0 : Style.space(6)

        Item {
            id: glyphSlot
            /* Size the slot to the glyph's real ink width plus a hair of slack
             * so the icon never paints over the status text to its right (a
             * fixed 18px slot under-sizes wide glyphs like the music note). */
            width: glyph.implicitWidth + Style.space(2)
            height: parent.height

            Text {
                id: glyph
                anchors.centerIn: parent
                text: root.closed ? "\uf05e" : (root.hasTrack ? root.playIcon : "\uf001")
                color: root.closed ? Qt.darker(root.bar.foreground, 1.7) : (root.hasTrack ? (root.playing ? Color.accent : Qt.darker(Color.accent, 1.25)) : Qt.darker(root.bar.foreground, 1.5))
                font.family: root.bar.fontFamily
                font.pixelSize: Style.font.body
            }

            MouseArea {
                anchors.fill: parent
                z: 10
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                enabled: !root.closed
                onClicked: {
                    if (root.hasTrack)
                        root.playPause();
                }
            }
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.collapsed
            text: root.hasTrack ? (root.title + (root.artist !== "" ? "  \u00b7  " + root.artist : "")) : (root.statusText !== "" ? root.statusText : "Nothing playing")
            textFormat: Text.PlainText
            color: root.hasTrack ? root.bar.foreground : Qt.darker(root.bar.foreground, 1.4)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            font.bold: root.hasTrack
            font.italic: !root.hasTrack
            elide: Text.ElideRight
            /* The icon lives in glyphSlot, a fixed-width row item.  Size the
             * title from the actual remaining row width rather than the
             * glyph's ink width, which varies by font and previously allowed
             * the title to paint into the icon. */
            width: Math.max(0, Math.min(root.labelMaxWidth, row.width - glyphSlot.width - row.spacing))
        }
    }

    MouseArea {
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: function (mouse) {
            if (mouse.button === Qt.RightButton)
                root.contextOpen = !root.contextOpen;
            else if (root.closed)
                root.contextOpen = true;
            else
                root.popupOpen = true;
        }
        onEntered: {
            if (root.closed)
                return;
            if (root.hidden) {
                root.hoverPeek = true;
                peekTimer.restart();
            } else if (root.bar) {
                root.bar.showTooltip(root, root.hoverTooltip());
            }
        }
        onExited: {
            peekTimer.stop();
            root.hoverPeek = false;
            if (root.bar)
                root.bar.hideTooltip(root);
        }
    }

    Timer {
        id: peekTimer
        interval: 3000
        onTriggered: root.hoverPeek = false
    }

    PopupCard {
        id: popup
        anchorItem: root
        bar: root.bar
        owner: root
        open: root.popupOpen
        // The panel holds text fields (filter, playlist name, track edit, add
        // source). Without an explicit keyboard-focus grab the compositor only
        // routes keys here incidentally, so a click on a field can sit dead for
        // a beat before it starts typing.
        grabFocus: root.popupOpen
        contentWidth: popup.fittedContentWidth(Style.space(360))
        contentHeight: popup.fittedContentHeight(column.implicitHeight)
        padding: Style.space(16)

        Column {
            id: column
            anchors.fill: parent
            spacing: Style.space(10)

            Row {
                width: parent.width
                spacing: Style.space(12)

                Column {
                    width: parent.width - Style.space(96) - Style.space(12)
                    spacing: Style.space(2)
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        id: titleText
                        text: root.title || "No song loaded"
                        textFormat: Text.PlainText
                        color: root.bar.foreground
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.subtitle
                        font.bold: true
                        /* Word-wrap only (never mid-word). Three lines, then the
                         * third is elided and a "see more" link expands it. */
                        width: parent.width
                        wrapMode: Text.WordWrap
                        maximumLineCount: root.titleExpanded ? 99 : 3
                        elide: root.titleExpanded ? Text.ElideNone : Text.ElideRight
                    }

                    Text {
                        id: titleMore
                        visible: titleText.truncated || root.titleExpanded
                        text: root.titleExpanded ? "[see less]" : "[see more]"
                        color: Color.accent
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.caption

                        MouseArea {
                            id: titleMoreMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.titleExpanded = !root.titleExpanded
                        }

                        /* Hover peek: the full title in a small font, at the
                         * cursor. Only useful while still collapsed. */
                        PanelToolTip {
                            visible: titleMoreMouse.containsMouse && !root.titleExpanded
                            delay: 150
                            x: titleMoreMouse.mouseX + Style.space(8)
                            y: titleMoreMouse.mouseY + Style.space(10)
                            fontSize: Style.font.caption
                            text: root.title
                        }
                    }
                    Text {
                        text: root.artist
                        textFormat: Text.PlainText
                        color: Qt.darker(root.bar.foreground, 1.3)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.bodySmall
                        elide: Text.ElideRight
                        width: parent.width
                        visible: text !== ""
                    }
                    Text {
                        text: root.album
                        textFormat: Text.PlainText
                        color: Qt.darker(root.bar.foreground, 1.6)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.caption
                        elide: Text.ElideRight
                        width: parent.width
                        visible: text !== ""
                    }
                }

                Item {
                    id: coverItem
                    width: Style.space(96)
                    height: Style.space(96)
                    anchors.verticalCenter: parent.verticalCenter

                    BorderSurface {
                        id: coverBox
                        anchors.fill: parent
                        radius: Math.max(Style.cornerRadius, Style.space(8))
                        color: Style.normalFillFor(root.bar.foreground, Color.accent)
                        borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)
                    }

                    Image {
                        id: coverImg
                        anchors.fill: parent
                        anchors.margins: Style.space(2)
                        /* Append a cache-busting nonce so QML re-fetches the file
                         * when a track changes (the backend also gives each cover
                         * a fresh filename, but this makes coverVersion meaningful
                         * even if the path were reused). */
                        source: root.coverSource !== "" ? ("file://" + root.coverSource + "?v=" + root.coverVersion) : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        clip: true
                        visible: root.coverSource !== ""
                    }
                }
            }

            Row {
                width: parent.width
                spacing: Style.space(8)

                Text {
                    text: root.fmt(slider.dragging ? slider.liveValue : root.positionMs)
                    color: root.bar.foreground
                    font.family: root.bar.fontFamily
                    font.pixelSize: Style.font.caption
                    width: Style.space(44)
                    verticalAlignment: Text.AlignVCenter
                }

                PanelSlider {
                    id: slider
                    width: parent.width - Style.space(44) - Style.space(44) - Style.space(8)
                    height: root.barSize
                    bar: root.bar
                    anchors.verticalCenter: parent.verticalCenter
                    minimum: 0
                    maximum: Math.max(1, root.durationMs)
                    step: 500
                    value: 0
                    enabled: root.durationMs > 0
                    trackHeight: Math.max(Style.space(6), Math.round(Style.spacing.controlHeight * 0.18))
                    knobSize: Math.max(Style.space(18), Math.round(Style.spacing.controlHeight * 0.48))
                    trackColor: Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.28)
                    fillColor: Color.accent
                    knobColor: root.bar.foreground
                    onReleased: function (v) {
                        slider.value = v;
                        root.seekTo(v);
                    }
                }

                Text {
                    text: root.fmt(root.durationMs)
                    color: Qt.darker(root.bar.foreground, 1.3)
                    font.family: root.bar.fontFamily
                    font.pixelSize: Style.font.caption
                    width: Style.space(44)
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                }
            }

            /* One control line: the transport group (prev / play / next), the
             * library toggle, the modes group (autoplay / shuffle / repeat) and
             * the volume+output button. */
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Style.space(4)

                /* Transport as one segmented control (prev / play-pause /
                 * next): one border, each segment its own hover highlight. */
                BorderSurface {
                    id: transportGroup
                    width: root.controlButtonSize * 3 + Style.space(4)
                    height: root.controlButtonSize
                    radius: Style.cornerRadius
                    clip: true
                    color: Style.normalFillFor(root.bar.foreground, Color.accent)
                    borderSpec: root.transportButtonBorder

                    Row {
                        anchors.fill: parent
                        anchors.margins: transportGroup.border.width
                        readonly property real segW: (width - 2 * tdiv.implicitWidth) / 3

                        ControlSeg {
                            width: parent.segW
                            glyph: "\uf048"
                            idle: root.bar.foreground
                            tip: "Previous track"
                            onAct: root.previous()
                        }
                        Rectangle {
                            id: tdiv
                            implicitWidth: Math.max(1, Style.space(1))
                            width: implicitWidth
                            height: parent.height
                            color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.16)
                        }
                        ControlSeg {
                            width: parent.segW
                            glyph: root.playIcon
                            active: root.playing
                            idle: root.bar.foreground
                            tip: "Play / pause"
                            onAct: root.playPause()
                        }
                        Rectangle {
                            implicitWidth: tdiv.implicitWidth
                            width: implicitWidth
                            height: parent.height
                            color: tdiv.color
                        }
                        ControlSeg {
                            width: parent.segW
                            glyph: "\uf051"
                            idle: root.bar.foreground
                            tip: "Next track"
                            onAct: root.next()
                        }
                    }
                }
                Button {
                    iconText: "\uf01d"
                    foreground: root.bar.foreground
                    width: root.controlButtonSize
                    height: root.controlButtonSize
                    iconSize: Style.font.icon
                    background: Style.normalFillFor(root.bar.foreground, Color.accent)
                    borderSpec: root.transportButtonBorder
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: "Change song (library)"
                    onClicked: {
                        root.libraryOpen = !root.libraryOpen;
                        if (!root.libraryOpen) {
                            root.actionsIndex = -1;
                            root.editVisible = false;
                            root.clearLibrarySearch();
                        }
                        if (root.libraryOpen)
                            root.loadLibrary();
                    }
                }

                /* Playback modes as one segmented control: a single
                 * border around all three toggles, each segment lighting up
                 * on its own hover. Width = 3x a button + the two dividers. */
                BorderSurface {
                    id: modeGroup
                    width: root.controlButtonSize * 3 + Style.space(4)
                    height: root.controlButtonSize
                    radius: Style.cornerRadius
                    clip: true
                    color: Style.normalFillFor(root.bar.foreground, Color.accent)
                    borderSpec: root.transportButtonBorder

                    Row {
                        anchors.fill: parent
                        anchors.margins: modeGroup.border.width

                        readonly property real segW: (width - 2 * divider.implicitWidth) / 3

                        ControlSeg {
                            width: parent.segW
                            glyph: root.autoplay ? "\uf01e" : "\uf00d"
                            active: root.autoplay
                            tip: root.autoplay ? "Autoplay on (turns off)" : "Autoplay off (turns on)"
                            onAct: root.toggleAutoplay()
                        }
                        Rectangle {
                            id: divider
                            implicitWidth: Math.max(1, Style.space(1))
                            width: implicitWidth
                            height: parent.height
                            color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.16)
                        }
                        ControlSeg {
                            width: parent.segW
                            glyph: "\uf074"
                            active: root.shuffle
                            tip: root.shuffle ? "Shuffle on (turns off)" : "Shuffle off (turns on)"
                            onAct: root.toggleShuffle()
                        }
                        Rectangle {
                            implicitWidth: divider.implicitWidth
                            width: implicitWidth
                            height: parent.height
                            color: divider.color
                        }
                        ControlSeg {
                            width: parent.segW
                            glyph: "\uf021"
                            active: root.repeatOne
                            tip: root.repeatOne ? "Repeat one (turns off)" : "Repeat off (repeats current track)"
                            onAct: root.toggleRepeatOne()
                        }
                    }
                }
                Button {
                    /* One entry point for volume + output; the panel below is
                     * collapsed until this is clicked. Accent-tinted while it
                     * is open or while anything is off its default. */
                    iconText: (root.muted || root.volume === 0) ? "\uf026"
                        : (root.volume < 50 ? "\uf027" : "\uf028")
                    foreground: (root.audioOpen || root.audioAdjusted) ? Color.accent : Qt.darker(root.bar.foreground, 1.6)
                    width: root.controlButtonSize
                    height: root.controlButtonSize
                    iconSize: Style.font.icon
                    background: Style.normalFillFor(root.bar.foreground, Color.accent)
                    borderSpec: root.transportButtonBorder
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: "Volume & output"
                    onClicked: root.audioOpen = !root.audioOpen
                }
            }

            Item {
                id: audioSection
                width: parent.width
                visible: root.audioOpen
                implicitHeight: visible ? audioCol.implicitHeight : 0

                Column {
                    id: audioCol
                    width: parent.width
                    spacing: Style.space(8)

                    Row {
                        width: parent.width
                        spacing: Style.space(8)

                        Button {
                            iconText: (root.muted || root.volume === 0) ? "\uf026"
                                : (root.volume < 50 ? "\uf027" : "\uf028")
                            foreground: root.muted ? Qt.darker(root.bar.foreground, 1.8) : root.bar.foreground
                            width: root.controlButtonSize
                            height: root.controlButtonSize
                            iconSize: Style.font.icon
                            background: Style.normalFillFor(root.bar.foreground, Color.accent)
                            borderSpec: root.transportButtonBorder
                            horizontalPadding: Style.spacing.controlPaddingX
                            verticalPadding: 0
                            tooltipText: root.muted ? "Muted (click to unmute)" : "Mute"
                            onClicked: root.toggleMute()
                        }

                        PanelSlider {
                            id: volSlider
                            width: parent.width - Style.space(36) - Style.space(40) - Style.space(16)
                            height: root.barSize
                            bar: root.bar
                            anchors.verticalCenter: parent.verticalCenter
                            minimum: 0
                            maximum: 100
                            step: 1
                            value: root.volume
                            enabled: !root.muted
                            trackHeight: Math.max(Style.space(6), Math.round(Style.spacing.controlHeight * 0.18))
                            knobSize: Math.max(Style.space(18), Math.round(Style.spacing.controlHeight * 0.48))
                            trackColor: Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.28)
                            fillColor: Color.accent
                            knobColor: root.bar.foreground
                            onMoved: function (v) {
                                root.volume = Math.round(v);
                                volApply.restart();
                            }
                            onReleased: function (v) {
                                volApply.stop();
                                root.setVolume(v);
                            }
                        }

                        Text {
                            text: (root.muted ? 0 : root.volume) + "%"
                            color: Qt.darker(root.bar.foreground, 1.3)
                            font.family: root.bar.fontFamily
                            font.pixelSize: Style.font.caption
                            width: Style.space(40)
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Column {
                        width: parent.width
                        spacing: Style.space(4)
                        visible: root.outputDevices.length > 0

                        Text {
                            text: "Output"
                            color: Qt.darker(root.bar.foreground, 1.3)
                            font.family: root.bar.fontFamily
                            font.pixelSize: Style.font.caption
                        }

                        ListView {
                            id: outputList
                            width: parent.width
                            /* Show ~3 sinks then scroll, so a host with many
                             * audio devices doesn't push the popup off-screen. */
                            height: Math.min(Style.space(28) * 3 + Style.space(4), outputList.contentHeight)
                            clip: true
                            spacing: Style.space(2)
                            readonly property bool overflowing: contentHeight > height
                            interactive: overflowing
                            boundsBehavior: Flickable.StopAtBounds
                            model: [""].concat(root.outputDevices)

                            /* Persistent rail on the right whenever the list is
                             * longer than it looks, so it reads as scrollable. */
                            ScrollBar.vertical: ScrollBar {
                                id: outputScroll
                                policy: outputList.overflowing ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                                width: Style.space(7)
                                padding: Style.space(1)
                                contentItem: Rectangle {
                                    implicitWidth: Style.space(5)
                                    radius: width / 2
                                    color: outputScroll.pressed ? Color.accent
                                        : Qt.rgba(Color.accent.r, Color.accent.g, Color.accent.b, 0.7)
                                }
                                background: Rectangle {
                                    radius: width / 2
                                    color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g,
                                                   root.bar.foreground.b, 0.22)
                                }
                            }

                            delegate: Button {
                                required property var modelData
                                required property int index
                                readonly property string devName: String(modelData)
                                readonly property bool current: root.outputDevice === devName
                                width: outputList.width - (outputList.overflowing ? Style.space(8) : 0)
                                height: Style.space(28)
                                leftAlign: true
                                fontSize: Style.font.bodySmall
                                text: (current ? "●  " : "○  ")
                                    + (devName === "" ? "System default"
                                       : (devName.length > 38 ? devName.slice(0, 37) + "…" : devName))
                                foreground: current ? Color.accent : Qt.darker(root.bar.foreground, 1.2)
                                selected: current
                                verticalPadding: 0
                                horizontalPadding: Style.spacing.controlPaddingX
                                tooltipText: devName === "" ? "System default" : devName
                                onClicked: root.selectOutput(devName)
                            }
                        }
                    }
                }
            }

            PanelSeparator {
                width: parent.width
                foreground: root.bar.foreground
                visible: root.libraryOpen
            }

            Item {
                id: librarySection
                width: parent.width
                visible: root.libraryOpen
                // A plain Item has no implicit size, so without this binding the
                // popup only measures the player controls and clips the library.
                implicitHeight: visible ? libraryCol.implicitHeight : 0

                Column {
                    id: libraryCol
                    width: parent.width
                    spacing: Style.space(4)

                    /* "Library" label followed by a horizontally-scrolling strip
                     * of playlist tabs. The viewed tab has an accent fill; a tab
                     * that is playing while a different one is viewed gets a
                     * half-accent fill so it reads as "still going". The framed
                     * "+" opens an inline name field right in the strip. */
                    Item {
                        width: parent.width
                        height: Math.max(playlistLabel.implicitHeight, Style.space(24))

                        Text {
                            id: playlistLabel
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.librarySearch.trim() === ""
                                ? "Library"
                                : "Library — " + root.filteredTracks.length + " of " + root.tracks.length
                            color: Qt.darker(root.bar.foreground, 1.3)
                            font.family: root.bar.fontFamily
                            font.pixelSize: Style.font.caption
                        }

                        Flickable {
                            id: playlistStrip
                            anchors.left: playlistLabel.right
                            anchors.leftMargin: Style.space(8)
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: Style.space(24)
                            contentWidth: playlistRow.width
                            contentHeight: height
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            flickableDirection: Flickable.HorizontalFlick
                            interactive: contentWidth > width

                            Row {
                                id: playlistRow
                                height: parent.height
                                spacing: Style.space(4)

                                Repeater {
                                    model: root.playlists

                                    delegate: Rectangle {
                                        id: ptab
                                        required property var modelData
                                        readonly property string pname: String(modelData)
                                        readonly property bool isViewed: pname === root.viewedPlaylist
                                        readonly property bool isPlaying: pname === root.playingPlaylist
                                        readonly property bool isIncoming: root.incomingTargetOf(pname) !== ""
                                        /* A staging list is always drawn in orange -- accent when
                                         * viewed, dimmer when not -- so it stands out as pending. */
                                        readonly property color tabTint: isIncoming ? root.incomingColor : Color.accent

                                        height: playlistRow.height
                                        width: ptabText.implicitWidth + Style.space(14)
                                        radius: 0
                                        color: isViewed
                                            ? tabTint
                                            : (isIncoming
                                                ? root.blend(tabTint, Color.popups.background, 0.35)
                                                : (isPlaying
                                                    ? root.blend(tabTint, Color.popups.background, 0.5)
                                                    : "transparent"))

                                        Text {
                                            id: ptabText
                                            anchors.centerIn: parent
                                            text: ptab.pname
                                            textFormat: Text.PlainText
                                            /* Passive tabs sit on the dark popup, so a dimmed
                                             * foreground reads as secondary. A filled tab is on
                                             * the accent (or half-accent) fill, so switch to the
                                             * popup's own dark colour for contrast against it. */
                                            color: (ptab.isViewed || ptab.isPlaying || ptab.isIncoming)
                                                ? Color.popups.background
                                                : Qt.darker(root.bar.foreground, 1.6)
                                            font.family: root.bar.fontFamily
                                            font.pixelSize: Style.font.caption
                                            font.bold: ptab.isViewed || ptab.isPlaying || ptab.isIncoming
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: root.viewPlaylist(ptab.pname)
                                        }
                                    }
                                }

                                Rectangle {
                                    id: addPlaylistChip
                                    height: playlistRow.height
                                    width: root.addingPlaylist
                                        ? Style.space(110)
                                        : (addPlusText.implicitWidth + Style.space(14))
                                    radius: 0
                                    color: "transparent"
                                    border.width: Math.max(1, Style.space(1))
                                    border.color: Color.accent

                                    Text {
                                        id: addPlusText
                                        anchors.centerIn: parent
                                        visible: !root.addingPlaylist
                                        text: "+"
                                        color: Color.accent
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.bodySmall
                                        font.bold: true
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: !root.addingPlaylist
                                        onClicked: root.beginAddPlaylist()
                                    }
                                    TextInput {
                                        id: newPlaylistInput
                                        anchors.fill: parent
                                        anchors.leftMargin: Style.space(6)
                                        anchors.rightMargin: Style.space(6)
                                        visible: root.addingPlaylist
                                        verticalAlignment: TextInput.AlignVCenter
                                        color: root.bar.foreground
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.caption
                                        clip: true
                                        maximumLength: 64
                                        /* Block caret in the accent colour: typing
                                         * a name reads like a text editor. */
                                        cursorDelegate: Rectangle {
                                            width: Style.space(6)
                                            color: Color.accent
                                            visible: newPlaylistInput.cursorVisible
                                        }
                                        onVisibleChanged: {
                                            if (visible) {
                                                text = "";
                                                /* Defer: the item isn't in the scene
                                                 * graph yet on the same frame it shows. */
                                                Qt.callLater(newPlaylistInput.forceActiveFocus);
                                            }
                                        }
                                        onAccepted: root.commitAddPlaylist(text)
                                        onActiveFocusChanged: {
                                            if (!activeFocus && root.addingPlaylist)
                                                root.commitAddPlaylist(text);
                                        }
                                        Keys.onEscapePressed: root.cancelAddPlaylist()
                                    }
                                }
                            }
                        }
                    }

                    TextField {
                        id: librarySearchField
                        width: parent.width
                        visible: !root.editVisible
                        placeholderText: "Filter by title, artist or album"
                        foreground: root.bar.foreground
                        onTextChanged: root.librarySearch = text
                    }

                    /* Track-info editor. Shown instead of the list (not on top of
                     * it) so the two never overlap. */
                    BorderSurface {
                        id: editPanel
                        width: parent.width
                        visible: root.editVisible
                        radius: Style.spacing.labelGap
                        color: Style.selectedFillFor(root.bar.foreground, Color.accent)
                        borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)
                        implicitHeight: editCol.implicitHeight + Style.space(16)
                        onVisibleChanged: if (visible) Qt.callLater(efTitle.forceActiveFocus)

                        Column {
                            id: editCol
                            anchors.fill: parent
                            anchors.margins: Style.space(8)
                            spacing: Style.space(6)

                            Text {
                                text: "Edit track info"
                                color: root.bar.foreground
                                font.family: root.bar.fontFamily
                                font.pixelSize: Style.font.subtitle
                                font.bold: true
                            }
                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                Text {
                                    text: "Title"
                                    color: Qt.darker(root.bar.foreground, 1.3)
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                }
                                TextField {
                                    id: efTitle
                                    width: parent.width
                                    text: root.editTitle
                                    foreground: root.bar.foreground
                                    onAccepted: efTitle.focus = false
                                }
                            }
                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                Text {
                                    text: "Artist"
                                    color: Qt.darker(root.bar.foreground, 1.3)
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                }
                                TextField {
                                    id: efArtist
                                    width: parent.width
                                    text: root.editArtist
                                    foreground: root.bar.foreground
                                    onAccepted: efArtist.focus = false
                                }
                            }
                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                Text {
                                    text: "Album"
                                    color: Qt.darker(root.bar.foreground, 1.3)
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                }
                                TextField {
                                    id: efAlbum
                                    width: parent.width
                                    text: root.editAlbum
                                    foreground: root.bar.foreground
                                    onAccepted: efAlbum.focus = false
                                }
                            }

                            Row {
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: Style.space(8)
                                Button {
                                    text: "Save"
                                    height: Style.space(32)
                                    iconSize: Style.font.icon
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    onClicked: {
                                        root.editTitle = efTitle.text;
                                        root.editArtist = efArtist.text;
                                        root.editAlbum = efAlbum.text;
                                        root.saveEdit();
                                    }
                                }
                                Button {
                                    text: "Cancel"
                                    height: Style.space(32)
                                    iconSize: Style.font.icon
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    onClicked: root.cancelEdit()
                                }
                            }
                        }
                    }

                    Item {
                        width: parent.width
                        visible: root.queue.length > 0 && root.viewingPlayingList && !root.editVisible
                        implicitHeight: visible ? clearQueueBtn.height : 0

                        Text {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Up next (" + root.queue.length + ")"
                            color: Color.accent
                            font.family: root.bar.fontFamily
                            font.pixelSize: Style.font.caption
                            font.bold: true
                        }
                        Button {
                            id: clearQueueBtn
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Clear"
                            height: Style.space(26)
                            iconSize: Style.font.caption
                            foreground: root.bar.foreground
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            borderSpec: root.transportButtonBorder
                            onClicked: root.clearQueue()
                        }
                    }

                    Text {
                        width: parent.width
                        visible: root.queue.length > 0 && root.viewingPlayingList && !root.editVisible
                        text: root.queueSummary
                        textFormat: Text.PlainText
                        color: Qt.darker(root.bar.foreground, 1.3)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.caption
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        visible: root.viewingIncoming && !root.editVisible
                        text: "Shift-click to accept/decline all in search"
                        color: Qt.darker(root.bar.foreground, 1.7)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.caption
                        font.italic: true
                    }

                    ListView {
                        id: trackList
                        width: parent.width
                        visible: !root.editVisible
                        height: Math.min(260, trackList.contentHeight)
                        clip: true
                        model: root.filteredTracks
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: BorderSurface {
                            required property int index
                            required property var modelData
                            readonly property bool isActive: modelData.index === root.selectedTrackIndex && root.hasTrack && root.viewingPlayingList
                            readonly property bool isCurrent: modelData.index === root.selectedTrackIndex && root.viewingPlayingList
                            readonly property bool isPending: root.pendingPlayIndex === modelData.index

                            width: trackList.width
                            height: Style.space(34)
                            radius: Style.spacing.labelGap
                            color: isActive ? Style.selectedFillFor(root.bar.foreground, Color.accent) : ((isPending || rowHover.hovered) ? Style.hoverFillFor(root.bar.foreground, Color.accent) : "transparent")
                            borderSpec: isActive ? Border.controlSpec("selected", root.bar.foreground, Color.accent) : Border.none()

                            HoverHandler {
                                id: rowHover
                            }

                            Shape {
                                id: pendingOutline
                                anchors.fill: parent
                                visible: isPending
                                preferredRendererType: Shape.CurveRenderer
                                antialiasing: true

                                ShapePath {
                                    id: pendingPath
                                    strokeColor: Color.accent
                                    strokeWidth: 1
                                    strokeStyle: ShapePath.DashLine
                                    dashPattern: [4, 3]
                                    fillColor: "transparent"

                                    startX: Style.spacing.labelGap
                                    startY: 0
                                    PathLine {
                                        x: pendingOutline.width - Style.spacing.labelGap
                                        y: 0
                                    }
                                    PathLine {
                                        x: pendingOutline.width
                                        y: Style.spacing.labelGap
                                    }
                                    PathLine {
                                        x: pendingOutline.width
                                        y: pendingOutline.height - Style.spacing.labelGap
                                    }
                                    PathLine {
                                        x: pendingOutline.width - Style.spacing.labelGap
                                        y: pendingOutline.height
                                    }
                                    PathLine {
                                        x: Style.spacing.labelGap
                                        y: pendingOutline.height
                                    }
                                    PathLine {
                                        x: 0
                                        y: pendingOutline.height - Style.spacing.labelGap
                                    }
                                    PathLine {
                                        x: 0
                                        y: Style.spacing.labelGap
                                    }
                                    PathLine {
                                        x: Style.spacing.labelGap
                                        y: 0
                                    }
                                }
                            }

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: Style.space(8)
                                anchors.rightMargin: Style.space(8)
                                spacing: Style.space(8)
                                z: 2

                                Text {
                                    readonly property int queuePos: root.viewingPlayingList ? root.queuePosition(modelData.index) : 0
                                    width: Style.space(24)
                                    text: queuePos > 0 ? "▸" + queuePos : String(modelData.index + 1)
                                    color: queuePos > 0 ? Color.accent : Qt.darker(root.bar.foreground, 1.6)
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                    font.bold: queuePos > 0
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Style.space(1)
                                    width: parent.width - Style.space(24) - Style.space(24) - Style.space(16)
                                        - (root.viewingIncoming ? Style.space(48) : 0)
                                    Text {
                                        text: modelData.title
                                        textFormat: Text.PlainText
                                        color: root.bar.foreground
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.bodySmall
                                        font.bold: isCurrent
                                        elide: Text.ElideRight
                                        width: parent.width
                                    }
                                    /* Secondary line: "Artist | Album" as one
                                     * flowing string, elided as a whole. */
                                    Text {
                                        width: parent.width
                                        visible: modelData.artist !== "" || modelData.album !== ""
                                        text: modelData.album !== ""
                                            ? modelData.artist + "  |  " + modelData.album
                                            : modelData.artist
                                        textFormat: Text.PlainText
                                        color: Qt.darker(root.bar.foreground, 1.5)
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.caption
                                        elide: Text.ElideRight
                                    }
                                }

                                /* Staging-list rows get accept (green tick) and
                                 * decline (red cross). Shift-click applies to
                                 * every row currently shown (after any filter). */
                                Item {
                                    width: Style.space(24)
                                    height: parent.height
                                    visible: root.viewingIncoming
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\uf00c"
                                        color: Qt.rgba(0.35, 0.75, 0.4, 1)
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.bodySmall
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        z: 3
                                        onClicked: (mouse) => {
                                            if (mouse.modifiers & Qt.ShiftModifier)
                                                root.acceptIncoming(root.shownIncomingIndices());
                                            else
                                                root.acceptIncoming([modelData.index]);
                                        }
                                    }
                                }
                                Item {
                                    width: Style.space(24)
                                    height: parent.height
                                    visible: root.viewingIncoming
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\uf00d"
                                        color: Qt.rgba(0.85, 0.35, 0.35, 1)
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.bodySmall
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        z: 3
                                        onClicked: (mouse) => {
                                            if (mouse.modifiers & Qt.ShiftModifier)
                                                root.declineIncoming(root.shownIncomingIndices());
                                            else
                                                root.declineIncoming([modelData.index]);
                                        }
                                    }
                                }
                                Item {
                                    width: Style.space(24)
                                    height: parent.height
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\uf141"
                                        color: rowHover.hovered ? root.bar.foreground : Qt.darker(root.bar.foreground, 1.5)
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.caption
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        z: 2
                                        onClicked: {
                                            root.editVisible = false;
                                            root.actionsAnchor = actionAnchor;
                                            root.actionsIndex = (root.actionsIndex === modelData.index) ? -1 : modelData.index;
                                        }
                                    }
                                }
                            }

                            Item {
                                id: actionAnchor
                                x: parent.width - Style.space(8) - Style.space(12)
                                y: parent.height / 2
                                width: 1
                                height: 1
                            }

                            MouseArea {
                                anchors.fill: parent
                                z: 1
                                enabled: !root.viewingIncoming
                                onClicked: root.playFromRow(modelData.index)
                            }
                        }
                    }

                    Text {
                        width: parent.width
                        visible: root.tracks.length > 0 && root.filteredTracks.length === 0 && !root.editVisible
                        text: "No tracks match “" + root.librarySearch.trim() + "”"
                        color: Qt.darker(root.bar.foreground, 1.5)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.caption
                        font.italic: true
                        elide: Text.ElideRight
                    }

                    Row {
                        width: parent.width
                        spacing: Style.space(8)
                        visible: !root.editVisible

                        Button {
                            iconText: root.addVisible ? "\uf00d" : "\uf067"
                            height: Style.space(32)
                            iconSize: Style.font.icon
                            foreground: root.bar.foreground
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            tooltipText: root.addVisible ? "Cancel adding a source" : "Add a local, SSH, https, or local-network source"
                            onClicked: root.addVisible = !root.addVisible
                        }
                        Text {
                            text: "Add source"
                            anchors.verticalCenter: parent.verticalCenter
                            color: Qt.darker(root.bar.foreground, 1.3)
                            font.family: root.bar.fontFamily
                            font.pixelSize: Style.font.caption
                        }
                    }

                    BorderSurface {
                        width: parent.width
                        visible: root.addVisible && !root.editVisible
                        radius: Style.spacing.labelGap
                        color: Style.selectedFillFor(root.bar.foreground, Color.accent)
                        borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)
                        implicitHeight: addForm.implicitHeight + Style.space(16)

                        Column {
                            id: addForm
                            anchors.fill: parent
                            anchors.margins: Style.space(8)
                            spacing: Style.space(6)

                            Row {
                                width: parent.width
                                spacing: Style.space(4)

                                Repeater {
                                    model: [ ["local", "Local"], ["ssh", "SSH"], ["https", "https"], ["network", "Network"] ]

                                    delegate: Button {
                                        required property var modelData
                                        text: modelData[1]
                                        height: Style.space(28)
                                        foreground: root.addType === modelData[0] ? Color.accent : root.bar.foreground
                                        selected: root.addType === modelData[0]
                                        verticalPadding: 0
                                        horizontalPadding: Style.spacing.controlPaddingX
                                        onClicked: root.addType = modelData[0]
                                    }
                                }
                            }

                            /* "Scan directory for all music files" -- when on, the
                             * path is a folder and every file in it is staged in an
                             * "INCOMING >> <playlist> <<" list for review. */
                            Item {
                                id: scanCheckRow
                                width: parent.width
                                visible: root.addType !== "https"
                                implicitHeight: Style.space(18)

                                Rectangle {
                                    id: scanBox
                                    width: Style.space(16)
                                    height: Style.space(16)
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    radius: 0
                                    color: root.addScan ? Color.accent : "transparent"
                                    border.width: Math.max(1, Style.space(1))
                                    border.color: root.addScan ? Color.accent : Qt.darker(root.bar.foreground, 1.4)
                                    Text {
                                        anchors.centerIn: parent
                                        visible: root.addScan
                                        text: "\uf00c"
                                        color: Color.popups.background
                                        font.family: root.bar.fontFamily
                                        font.pixelSize: Style.font.caption
                                    }
                                }
                                Text {
                                    anchors.left: scanBox.right
                                    anchors.leftMargin: Style.space(6)
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "Scan directory for all music files"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.addScan = !root.addScan
                                }
                            }

                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                visible: root.addType === "local"

                                Text {
                                    text: root.addScan ? "Folder on this machine" : "Path on this machine"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: true
                                }
                                TextField {
                                    id: addLocalField
                                    width: parent.width
                                    text: root.addPath
                                    placeholderText: root.addScan ? "/home/me/Music" : "/home/me/Music/song.flac"
                                    foreground: root.bar.foreground
                                    onAccepted: {
                                        root.addPath = text;
                                        root.addLocalTrack(text);
                                    }
                                }
                            }

                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                visible: root.addType === "ssh" || root.addType === "network"

                                Text {
                                    text: root.addType === "ssh" ? "SSH user" : "Network user"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: true
                                }
                                TextField {
                                    id: addUserField
                                    width: parent.width
                                    text: root.addUser
                                    placeholderText: "username"
                                    foreground: root.bar.foreground
                                    onAccepted: root.submitAdd()
                                }
                                Text {
                                    text: "Host or IP"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: true
                                }
                                TextField {
                                    id: addHostField
                                    width: parent.width
                                    text: root.addHost
                                    placeholderText: "nas.local or 192.168.1.10"
                                    foreground: root.bar.foreground
                                    onAccepted: root.submitAdd()
                                }
                                Text {
                                    text: "Remote path"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: true
                                }
                                TextField {
                                    id: addSshPathField
                                    width: parent.width
                                    text: root.addPath
                                    placeholderText: "/music/song.flac"
                                    foreground: root.bar.foreground
                                    onAccepted: {
                                        root.addPath = text;
                                        root.submitAdd();
                                    }
                                }
                            }

                            Column {
                                width: parent.width
                                spacing: Style.space(4)
                                visible: root.addType === "https"

                                Text {
                                    text: "https:// URL"
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: true
                                }
                                TextField {
                                    id: addUrlField
                                    width: parent.width
                                    text: root.addUrl
                                    placeholderText: "https://example.com/song.flac"
                                    foreground: root.bar.foreground
                                    onAccepted: root.submitAdd()
                                }
                            }

                            Text {
                                text: root.addScan
                                    ? ("Every audio file in the folder is staged in \"INCOMING >> " +
                                       root.scanTargetPlaylist() + " <<\" for you to accept or decline.")
                                    : (root.addType === "local"
                                        ? "Metadata is read from the file; missing tags use the file name."
                                        : (root.addType === "https"
                                            ? "The URL is streamed with curl; tags are read with ffprobe when possible."
                                            : "The file is streamed over SSH; tags are read from the remote host when possible."))
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: Qt.darker(root.bar.foreground, 1.5)
                                font.family: root.bar.fontFamily
                                font.pixelSize: Style.font.caption
                            }
                            Row {
                                anchors.horizontalCenter: parent.horizontalCenter
                                spacing: Style.space(8)
                                Button {
                                    text: "Add"
                                    height: Style.space(32)
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    onClicked: root.submitAdd()
                                }
                                Button {
                                    text: "Cancel"
                                    height: Style.space(32)
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    onClicked: root.addVisible = false
                                }
                            }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    z: 40
                    visible: root.actionsIndex >= 0 && !root.editVisible
                    onClicked: {
                        root.actionsIndex = -1;
                        root.editVisible = false;
                    }
                }

                BorderSurface {
                    id: actionsMenu
                    z: 50
                    visible: root.actionsIndex >= 0 && !root.editVisible
                    width: Style.space(150)
                    height: actionsCol.implicitHeight + Style.space(8)
                    radius: Style.spacing.labelGap
                    color: Color.popups.background
                    borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)

                    x: root.actionsAnchor
                        ? Math.max(0, librarySection.mapFromItem(root.actionsAnchor, 0, 0).x - width)
                        : 0
                    y: root.actionsAnchor
                        ? Math.max(0, librarySection.mapFromItem(root.actionsAnchor, 0, 0).y - height / 2)
                        : 0

                    Column {
                        id: actionsCol
                        anchors.fill: parent
                        anchors.margins: Style.space(4)
                        spacing: Style.space(2)

                        Button {
                            text: "Play"
                            iconText: "\uf04b"
                            visible: !root.viewingIncoming
                            width: parent.width
                            height: Style.space(32)
                            iconSize: Style.font.icon
                            foreground: root.bar.foreground
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            onClicked: root.playIndex(root.actionsIndex)
                        }
                        Button {
                            readonly property bool queued: root.queuePosition(root.actionsIndex) > 0
                            text: queued ? "Unqueue" : "Add to queue"
                            iconText: "\uf0cb"
                            /* The queue indexes the playing playlist; hide it
                             * while a different playlist is being browsed. */
                            visible: root.viewingPlayingList
                            width: parent.width
                            height: Style.space(32)
                            iconSize: Style.font.icon
                            foreground: queued ? Color.accent : root.bar.foreground
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            onClicked: {
                                var idx = root.actionsIndex;
                                root.actionsIndex = -1;
                                if (queued)
                                    root.unqueueTrack(idx);
                                else
                                    root.queueTrack(idx);
                            }
                        }
                        Button {
                            text: "Edit info"
                            iconText: "\uf040"
                            width: parent.width
                            height: Style.space(32)
                            iconSize: Style.font.icon
                            foreground: root.bar.foreground
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            onClicked: {
                                var idx = root.actionsIndex;
                                root.actionsIndex = -1;
                                root.startEdit(idx);
                            }
                        }
                        Button {
                            text: "Remove"
                            iconText: "\uf2ed"
                            width: parent.width
                            height: Style.space(32)
                            iconSize: Style.font.icon
                            foreground: Color.urgent
                            verticalPadding: 0
                            horizontalPadding: Style.spacing.controlPaddingX
                            onClicked: root.removeTrack(root.actionsIndex)
                        }
                    }
                }
            }
        }

        DropArea {
            id: libraryDropArea
            anchors.fill: parent
            z: 100
            onEntered: (drop) => { drop.accepted = true }
            onDropped: (drop) => {
                var i;
                var paths = [];
                for (i = 0; i < drop.urls.length; i++)
                    paths.push(root.urlToPath(drop.urls[i]));
                if (paths.length === 0 && drop.text) {
                    var lines = String(drop.text).split("\n");
                    for (i = 0; i < lines.length; i++) {
                        var t = lines[i].trim();
                        if (t !== "")
                            paths.push(root.urlToPath(t));
                    }
                }
                if (paths.length === 0) {
                    root.statusText = "Drop a local audio file to add it.";
                    return;
                }
                for (i = 0; i < paths.length; i++)
                    root.addLocalTrack(paths[i]);
                root.loadLibrary();
            }
        }
    }

    PopupCard {
        id: contextPopup
        anchorItem: root
        bar: root.bar
        owner: root
        open: root.contextOpen
        contentWidth: popup.fittedContentWidth(Style.space(180))
        contentHeight: popup.fittedContentHeight(contextCol.implicitHeight)

        Column {
            id: contextCol
            anchors.fill: parent
            spacing: Style.space(4)

            Button {
                text: root.hidden ? "Show widget" : "Hide widget"
                width: parent.width
                height: Style.space(34)
                iconSize: Style.font.icon
                foreground: root.bar.foreground
                verticalPadding: 0
                horizontalPadding: Style.spacing.controlPaddingX
                onClicked: {
                    root.hidden = !root.hidden;
                    root.contextOpen = false;
                }
            }
            Button {
                text: root.closed ? "Open widget" : "Close widget"
                width: parent.width
                height: Style.space(34)
                iconSize: Style.font.icon
                foreground: root.closed ? root.bar.foreground : Color.urgent
                verticalPadding: 0
                horizontalPadding: Style.spacing.controlPaddingX
                onClicked: {
                    root.contextOpen = false;
                    root.setClosed(!root.closed);
                }
            }
        }
    }

    /* Watch the library file in-process (no `sh -c cat` fork).  watchChanges
     * plus reload() means the list re-reads fresh content whenever the backend
     * rewrites the file, so a remove/edit edit is reflected immediately instead
     * of lingering from a stale snapshot read before the write landed. */
    FileView {
        id: libraryFileView
        path: root.libraryPath
        watchChanges: !root.closed
        printErrors: false
        onFileChanged: reload()
        onLoaded: root.setTracks(text())
    }
}
