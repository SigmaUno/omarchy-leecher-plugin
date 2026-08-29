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
     * app.c init_ipc_dir(): $XDG_RUNTIME_DIR/leecher, else /tmp/leecher-<uid>. */
    readonly property string runtimeDir: (function () {
        var rd = Quickshell.env("XDG_RUNTIME_DIR");
        var uid = Quickshell.env("UID");
        return (rd && rd !== "") ? (rd + "/leecher") : ("/tmp/leecher-" + uid);
    })()
    readonly property string statusFile: root.runtimeDir + "/status.json"
    readonly property string controlFile: root.runtimeDir + "/control"

    property string title: ""
    property string artist: ""
    property string album: ""
    property string libraryPath: ""
    property int durationMs: 0
    property bool hasTrack: false
    property bool autoplay: true
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
    property bool hidden: false
    property bool hoverPeek: false
    property bool closed: false
    property int selectedTrackIndex: -1
    property int pendingPlayIndex: -1
    property string statusText: ""
    property int lastCommandId: 0
    property int pendingCommandId: -1
    property bool commandAcked: true
    property var tracks: []

    property int actionsIndex: -1
    property int editIndex: -1
    property bool editVisible: false
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

    implicitWidth: root.collapsed ? root.barSize : Style.space(24) + root.barSize + Style.space(6) + Style.space(150) + Style.space(18)
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
    function sendControl(cmd) {
        root.lastCommandId = (root.lastCommandId + 1) & 0x7fffffff;
        root.pendingCommandId = root.lastCommandId;
        root.commandAcked = false;
        /* Encode the payload so values (titles/artists/albums) can contain
         * newlines or shell-special characters without corrupting the line, and
         * quote the literal control path so a runtime dir with spaces still
         * resolves to a single redirection target. */
        Quickshell.execDetached(["sh", "-c", "printf '%s\\n' '" + root.lastCommandId + " " + enc(cmd) + "' > \"" + root.controlFile + "\""]);
    }
    /* Single-command multi-field edit: each value is percent-encoded with enc()
     * (no literal spaces/quotes/newlines remain), so the three values are joined
     * by literal spaces into one control line that the backend splits and
     * applies in a single atomic library write. */
    function sendControlFields(idx, f1, f2, f3) {
        root.lastCommandId = (root.lastCommandId + 1) & 0x7fffffff;
        root.pendingCommandId = root.lastCommandId;
        root.commandAcked = false;
        var values = enc(f1) + " " + enc(f2) + " " + enc(f3);
        Quickshell.execDetached(["sh", "-c", "printf '%s\\n' '" + root.lastCommandId + " set_fields " + idx + " " + values + "' > \"" + root.controlFile + "\""]);
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
    function toggleAutoplay() {
        root.autoplay = !root.autoplay;
        root.sendControl(root.autoplay ? "autoplay on" : "autoplay off");
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
        /* Apply all three fields in one atomic backend write. */
        root.sendControlFields(root.editIndex, root.editTitle, root.editArtist, root.editAlbum);
        root.editVisible = false;
        root.editIndex = -1;
        root.loadLibrary();
    }
    function cancelEdit() {
        root.editVisible = false;
        root.editIndex = -1;
        root.actionsIndex = -1;
    }
    function removeTrack(i) {
        root.sendControl("remove " + i);
        root.actionsIndex = -1;
        root.editVisible = false;
        root.loadLibrary();
    }
    function fmt(ms) {
        if (!ms || ms <= 0)
            return "0:00";
        var s = Math.floor(ms / 1000);
        var m = Math.floor(s / 60);
        var h = Math.floor(m / 60);
        s = s % 60;
        m = m % 60;
        return h + ":" + (m < 10 ? "0" : "") + ":" + (s < 10 ? "0" : "") + s;
    }
    function close() {
        root.popupOpen = false;
        root.libraryOpen = false;
        root.actionsIndex = -1;
        root.editVisible = false;
        root.editIndex = -1;
        root.pendingPlayIndex = -1;
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
        /* Command acknowledgment: when the backend echoes the id we sent with a
         * control write, a previously-issued command has been processed. Only
         * then do we let the backend's autoplay value override an optimistic
         * local toggle; otherwise the echo would wipe a not-yet-applied tweak. */
        if (data.cmd_id !== undefined && data.cmd_id !== null &&
            root.pendingCommandId >= 0 && Number(data.cmd_id) === root.pendingCommandId) {
            root.commandAcked = true;
            root.pendingCommandId = -1;
        }
        if (root.pendingCommandId < 0)
            root.autoplay = data.autoplay !== false;
        root.statusText = data.status ? String(data.status) : "";
        root.hasTrack = root.title !== "";
        root.coverSource = data.cover ? String(data.cover) : "";
        root.selectedTrackIndex = idx;
        if (isPlayingNow && root.pendingPlayIndex >= 0 && idx === root.pendingPlayIndex) {
            root.pendingPlayIndex = -1;
        }
        if (data.library)
            root.libraryPath = String(data.library);
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

        Text {
            id: glyph
            anchors.verticalCenter: parent.verticalCenter
            text: root.closed ? "\uf05e" : (root.hasTrack ? root.playIcon : "\uf001")
            color: root.closed ? Qt.darker(root.bar.foreground, 1.7) : (root.hasTrack ? (root.playing ? root.bar.foreground : Qt.darker(root.bar.foreground, 1.4)) : Qt.darker(root.bar.foreground, 1.5))
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
        }

        MouseArea {
            anchors.fill: glyph
            z: 10
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            enabled: !root.closed
            onClicked: {
                if (root.hasTrack)
                    root.playPause();
            }
        }

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.collapsed
            text: root.hasTrack ? (root.title + (root.artist !== "" ? "  \u00b7  " + root.artist : "")) : (root.statusText !== "" ? root.statusText : "Nothing's playing")
            color: root.hasTrack ? root.bar.foreground : Qt.darker(root.bar.foreground, 1.4)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.body
            font.italic: !root.hasTrack
            elide: Text.ElideRight
            width: Math.max(0, Math.min(Style.space(150), row.width - Style.space(24) - glyph.width - row.spacing - Style.space(18)))

            ToolTip.visible: root.statusText !== "" && label.hovered
            ToolTip.text: root.statusText
            ToolTip.delay: 400
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
                root.bar.showTooltip(root, root.hasTrack ? (root.title + (root.artist !== "" ? " \u2014 " + root.artist : "")) : "Nothing's playing");
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
        contentWidth: popup.fittedContentWidth(Style.space(340))
        contentHeight: popup.fittedContentHeight(column.implicitHeight)

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
                        text: root.title || "No song loaded"
                        color: root.bar.foreground
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.subtitle
                        font.bold: true
                        elide: Text.ElideRight
                        width: parent.width
                    }
                    Text {
                        text: root.artist
                        color: Qt.darker(root.bar.foreground, 1.3)
                        font.family: root.bar.fontFamily
                        font.pixelSize: Style.font.bodySmall
                        elide: Text.ElideRight
                        width: parent.width
                        visible: text !== ""
                    }
                    Text {
                        text: root.album
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

                    Rectangle {
                        id: coverBox
                        anchors.fill: parent
                        radius: Style.cornerRadius
                        color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(root.bar.foreground.r, root.bar.foreground.g, root.bar.foreground.b, 0.18)
                    }

                    Image {
                        id: coverImg
                        anchors.fill: parent
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

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Style.space(6)

                Button {
                    iconText: "\uf048"
                    foreground: root.bar.foreground
                    height: Style.space(36)
                    iconSize: Style.font.icon
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: "Skip to start / back"
                    onClicked: root.previous()
                }
                Button {
                    iconText: root.playIcon
                    foreground: root.bar.foreground
                    height: Style.space(36)
                    iconSize: Style.font.icon
                    horizontalPadding: Style.spacing.panelGap
                    verticalPadding: 0
                    tooltipText: "Play / pause"
                    onClicked: root.playPause()
                }
                Button {
                    iconText: "\uf051"
                    foreground: root.bar.foreground
                    height: Style.space(36)
                    iconSize: Style.font.icon
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: "Skip forward"
                    onClicked: root.next()
                }
                Button {
                    iconText: "\uf01d"
                    foreground: root.bar.foreground
                    height: Style.space(36)
                    iconSize: Style.font.icon
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: "Change song (library)"
                    onClicked: {
                        root.libraryOpen = !root.libraryOpen;
                        if (root.libraryOpen)
                            root.loadLibrary();
                    }
                }
                Button {
                    iconText: root.autoplay ? "\uf01e" : "\uf00d"
                    foreground: root.autoplay ? Color.accent : Qt.darker(root.bar.foreground, 1.6)
                    width: Style.space(36)
                    height: Style.space(36)
                    iconSize: Style.font.icon
                    horizontalPadding: Style.spacing.controlPaddingX
                    verticalPadding: 0
                    tooltipText: root.autoplay ? "Autoplay on (turns off)" : "Autoplay off (turns on)"
                    onClicked: root.toggleAutoplay()
                }
            }

            PanelSeparator {
                width: parent.width
                foreground: root.bar.foreground
                visible: root.libraryOpen
            }

            Column {
                id: librarySection
                width: parent.width
                visible: root.libraryOpen
                spacing: Style.space(4)

                Text {
                    text: "Library"
                    color: Qt.darker(root.bar.foreground, 1.3)
                    font.family: root.bar.fontFamily
                    font.pixelSize: Style.font.caption
                }

                ListView {
                    id: trackList
                    width: parent.width
                    height: Math.min(260, trackList.contentHeight)
                    clip: true
                    model: root.tracks
                    boundsBehavior: Flickable.StopAtBounds

                    delegate: BorderSurface {
                        required property int index
                        required property var modelData
                        readonly property bool isActive: modelData.index === root.selectedTrackIndex && root.hasTrack
                        readonly property bool isCurrent: modelData.index === root.selectedTrackIndex
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
                                width: Style.space(24)
                                text: String(modelData.index + 1)
                                color: Qt.darker(root.bar.foreground, 1.6)
                                font.family: root.bar.fontFamily
                                font.pixelSize: Style.font.caption
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Column {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Style.space(1)
                                width: parent.width - Style.space(24) - Style.space(24) - Style.space(16)
                                Text {
                                    text: modelData.title
                                    color: root.bar.foreground
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.bodySmall
                                    font.bold: isCurrent
                                    elide: Text.ElideRight
                                    width: parent.width
                                }
                                Text {
                                    text: modelData.artist
                                    color: Qt.darker(root.bar.foreground, 1.5)
                                    font.family: root.bar.fontFamily
                                    font.pixelSize: Style.font.caption
                                    elide: Text.ElideRight
                                    width: parent.width
                                    visible: text !== ""
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
                                        root.actionsIndex = (root.actionsIndex === modelData.index) ? -1 : modelData.index;
                                    }
                                }
                            }
                        }

                        BorderSurface {
                            width: parent.width
                            visible: root.actionsIndex === modelData.index && !root.editVisible
                            z: 3
                            radius: Style.spacing.labelGap
                            color: "transparent"
                            borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)
                            height: Style.space(40)

                            Row {
                                anchors.centerIn: parent
                                spacing: Style.space(6)
                                Button {
                                    iconText: "\uf04b"
                                    height: Style.space(34)
                                    iconSize: Style.font.icon
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    tooltipText: "Play"
                                    onClicked: root.playIndex(modelData.index)
                                }
                                Button {
                                    iconText: "\uf040"
                                    height: Style.space(34)
                                    iconSize: Style.font.icon
                                    foreground: root.bar.foreground
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    tooltipText: "Edit info"
                                    onClicked: root.startEdit(modelData.index)
                                }
                                Button {
                                    iconText: "\uf2ed"
                                    height: Style.space(34)
                                    iconSize: Style.font.icon
                                    foreground: Color.urgent
                                    verticalPadding: 0
                                    horizontalPadding: Style.spacing.controlPaddingX
                                    tooltipText: "Remove"
                                    onClicked: root.removeTrack(modelData.index)
                                }
                            }
                        }

                        BorderSurface {
                            width: parent.width
                            visible: root.editVisible && root.editIndex === modelData.index
                            z: 4
                            radius: Style.spacing.labelGap
                            color: Style.selectedFillFor(root.bar.foreground, Color.accent)
                            borderSpec: Border.controlSpec("normal", root.bar.foreground, Color.accent)

                            Column {
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

                        MouseArea {
                            anchors.fill: parent
                            z: 1
                            onClicked: root.playFromRow(modelData.index)
                        }
                    }
                }
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
