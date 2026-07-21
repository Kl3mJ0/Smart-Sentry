// Smart Sentry dashboard UI — ported from the approved Claude design
// (Smart Sentry Dashboard.html). Fixed 1280x720 design surface, uniformly
// scaled to the window, so it renders identically windowed or kiosk.
import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 1280
    height: 720
    visible: true
    visibility: startFullscreen ? Window.FullScreen : Window.AutomaticVisibility
    color: "#050709"
    title: "Smart Sentry"
    property bool updatePanelOpen: false

    // ---- connection-state styling (from design CONN_CONFIG) --------------
    readonly property var connCfg: ({
        scanning:       { label: "SCANNING…",        color: "#7fd4ff", bg: "#1A7FD4FF", border: "#4D7FD4FF" },
        connecting:     { label: "CONNECTING",            color: "#ffd166", bg: "#1AFFD166", border: "#4DFFD166" },
        authenticating: { label: "AUTHENTICATING",        color: "#c792ff", bg: "#1AC792FF", border: "#4DC792FF" },
        secure:         { label: "SECURE · CONNECTED", color: "#5eead4", bg: "#1A5EEAD4", border: "#4D5EEAD4" }
    })
    readonly property var cs: connCfg[sentry.connState]

    // ---- animated display values (count-up like the design) --------------
    readonly property real tempMin: 15; readonly property real tempMax: 35
    property real dispTemp: sentry.temp
    property real dispHum: sentry.humidity
    Behavior on dispTemp { NumberAnimation { duration: 650; easing.type: Easing.OutCubic } }
    Behavior on dispHum  { NumberAnimation { duration: 650; easing.type: Easing.OutCubic } }

    // ---- glow pulse on fresh data -----------------------------------------
    property real halo: 0
    SequentialAnimation {
        id: pulseAnim
        NumberAnimation { target: root; property: "halo"; to: 1; duration: 120 }
        NumberAnimation { target: root; property: "halo"; to: 0; duration: 580 }
    }
    Connections { target: sentry; function onDataArrived() { pulseAnim.restart() } }

    // ---- chart history (sampled, ~last 60s) -------------------------------
    property var history: []
    function pushSample() {
        var h = history.slice(-29)
        h.push({ temp: sentry.temp, hum: sentry.humidity })
        history = h
        chart.requestPaint()
    }
    Timer {
        interval: 2000; repeat: true
        running: sentry.connState === "secure" && !sentry.reconnecting
        onTriggered: root.pushSample()
    }
    Component.onCompleted: {
        var h = [], t = sentry.temp, u = sentry.humidity
        for (var i = 0; i < 30; i++) {
            t = Math.max(tempMin, Math.min(tempMax, t + (Math.random() - 0.5) * 0.6))
            u = Math.max(0, Math.min(100, u + (Math.random() - 0.5) * 1.2))
            h.push({ temp: t, hum: u })
        }
        history = h
    }

    // ======================= reusable pieces ===============================
    component GlowCard: Rectangle {
        radius: 18
        border.width: 1
        gradient: Gradient {
            GradientStop { position: 0; color: "#E6141C26" }
            GradientStop { position: 1; color: "#E60A0E14" }
        }
    }

    component Spinner: Item {
        property color ringColor: "#7fd4ff"
        width: 14; height: 14
        Canvas {
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var r = width / 2 - 1.5
                ctx.lineWidth = 2
                ctx.strokeStyle = "rgba(255,255,255,0.15)"
                ctx.beginPath(); ctx.arc(width/2, height/2, r, 0, 2*Math.PI); ctx.stroke()
                ctx.strokeStyle = parent.ringColor
                ctx.beginPath(); ctx.arc(width/2, height/2, r, -Math.PI*0.85, -Math.PI*0.15); ctx.stroke()
            }
            Component.onCompleted: requestPaint()
        }
        RotationAnimator on rotation {
            from: 0; to: 360; duration: 700
            loops: Animation.Infinite; running: visible
        }
    }

    component RingGauge: Canvas {
        id: gauge
        property real pct: 0
        property color gradA: "#7fd4ff"
        property color gradB: "#4aa8ff"
        width: 140; height: 140
        onPctChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.lineWidth = 10
            ctx.lineCap = "round"
            ctx.strokeStyle = "rgba(127,212,255,0.12)"
            ctx.beginPath(); ctx.arc(70, 70, 54, 0, 2*Math.PI); ctx.stroke()
            var g = ctx.createLinearGradient(0, 0, width, height)
            g.addColorStop(0, String(gradA)); g.addColorStop(1, String(gradB))
            ctx.strokeStyle = g
            var p = Math.max(0.001, Math.min(1, pct))
            ctx.beginPath(); ctx.arc(70, 70, 54, -Math.PI/2, -Math.PI/2 + p*2*Math.PI); ctx.stroke()
        }
        Behavior on pct { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
        Component.onCompleted: requestPaint()
    }

    // ============================ stage ====================================
    Item {
        id: stage
        width: 1280; height: 720
        anchors.centerIn: parent
        scale: Math.min(root.width / 1280, root.height / 720)

        Rectangle { anchors.fill: parent; color: "#0a0e14" }
        Canvas {  // background radial glows
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var g1 = ctx.createRadialGradient(256, 0, 0, 256, 0, 700)
                g1.addColorStop(0, "rgba(74,168,255,0.10)"); g1.addColorStop(1, "rgba(74,168,255,0)")
                ctx.fillStyle = g1; ctx.fillRect(0, 0, width, height)
                var g2 = ctx.createRadialGradient(1280, 720, 0, 1280, 720, 600)
                g2.addColorStop(0, "rgba(74,168,255,0.06)"); g2.addColorStop(1, "rgba(74,168,255,0)")
                ctx.fillStyle = g2; ctx.fillRect(0, 0, width, height)
            }
            Component.onCompleted: requestPaint()
        }

        Column {
            id: content
            anchors.fill: parent
            anchors.leftMargin: 32; anchors.rightMargin: 32
            anchors.topMargin: 28; anchors.bottomMargin: 28
            spacing: 20
            opacity: sentry.reconnecting ? 0.5 : 1
            Behavior on opacity { NumberAnimation { duration: 500 } }

            // ---------------- HEADER ----------------
            Item {
                width: parent.width; height: 56
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    Rectangle {
                        width: 40; height: 40; radius: 10
                        border.width: 1; border.color: "#597FD4FF"
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#407FD4FF" }
                            GradientStop { position: 1; color: "#0D4AA8FF" }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 16; height: 16; radius: 4
                            gradient: Gradient {
                                GradientStop { position: 0; color: "#7fd4ff" }
                                GradientStop { position: 1; color: "#4aa8ff" }
                            }
                        }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            text: "SMART SENTRY"
                            color: "#eaf6ff"; font.family: uiFont
                            font.pixelSize: 22; font.bold: true; font.letterSpacing: 1.3
                        }
                        Text {
                            text: "ENVIRONMENTAL SENSOR CONTROL"
                            color: "#5a6472"; font.family: uiFont
                            font.pixelSize: 11; font.letterSpacing: 1.5
                        }
                    }
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 16

                    Rectangle {  // fleet device selector
                        width: 250; height: 48; radius: 12
                        color: "#0D7FD4FF"; border.width: 1; border.color: "#337FD4FF"
                        Row {
                            anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                            Text { text: "‹"; color: "#7fd4ff"; font.pixelSize: 28; anchors.verticalCenter: parent.verticalCenter }
                            MouseArea { width: 28; height: parent.height; onClicked: sentry.previousDevice() }
                            Column {
                                width: 164; anchors.verticalCenter: parent.verticalCenter
                                Text { text: sentry.deviceName; width: parent.width; elide: Text.ElideRight; color: "#eaf6ff"; font.family: uiFont; font.pixelSize: 12; font.bold: true }
                                Text { text: "SS1 " + sentry.devicePosition + "/" + sentry.deviceCount + " · FW " + sentry.fwVersion; color: "#5a6472"; font.family: monoFont; font.pixelSize: 10 }
                            }
                            MouseArea { width: 28; height: parent.height; onClicked: sentry.nextDevice() }
                            Text { text: "›"; color: "#7fd4ff"; font.pixelSize: 28; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }

                    Rectangle {  // update status / queue button
                        id: updateButton
                        width: 170; height: 48; radius: 12
                        color: "#147FD4FF"; border.width: 1
                        border.color: sentry.updateStatus === "error" ? "#ff6b6b" : "#4D7FD4FF"
                        Column {
                            anchors.centerIn: parent; spacing: 2
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: sentry.activeUpdateDevice ? "UPDATING SS1" :
                                      sentry.updateStatus === "checking" ? "CHECKING…" :
                                      sentry.updateStatus === "up_to_date" ? "FIRMWARE UP TO DATE" : "FIRMWARE / QUEUE"
                                color: "#7fd4ff"; font.family: uiFont; font.pixelSize: 11; font.bold: true
                            }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: sentry.activeUpdateDevice ? sentry.updateProgress + "% · " + sentry.activeUpdateDevice.slice(-8) : sentry.pendingJobCount + " pending"; color: "#8b95a3"; font.family: monoFont; font.pixelSize: 9 }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.updatePanelOpen = true }
                    }

                    Row {  // signal bars
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Repeater {
                            model: [6, 10, 14, 18]
                            Rectangle {
                                width: 5; height: modelData; radius: 2
                                anchors.bottom: parent.bottom
                                color: index < sentry.signalLevel ? "#7fd4ff" : "#267FD4FF"
                                Behavior on color { ColorAnimation { duration: 300 } }
                            }
                        }
                    }

                    Rectangle {  // status pill
                        id: pill
                        width: pillRow.width + 36; height: 48; radius: 24
                        color: root.cs.bg
                        border.width: 1; border.color: root.cs.border
                        Behavior on color { ColorAnimation { duration: 400 } }
                        Behavior on border.color { ColorAnimation { duration: 400 } }
                        scale: pillMa.pressed ? 0.95 : 1
                        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }
                        Row {
                            id: pillRow
                            anchors.centerIn: parent
                            spacing: 10
                            Rectangle {  // pulsing scan dot
                                visible: sentry.connState === "scanning"
                                anchors.verticalCenter: parent.verticalCenter
                                width: 10; height: 10; radius: 5
                                color: root.cs.color
                                SequentialAnimation on scale {
                                    running: visible; loops: Animation.Infinite
                                    NumberAnimation { to: 1.7; duration: 600; easing.type: Easing.InOutQuad }
                                    NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutQuad }
                                }
                                SequentialAnimation on opacity {
                                    running: visible; loops: Animation.Infinite
                                    NumberAnimation { to: 0.35; duration: 600 }
                                    NumberAnimation { to: 1.0; duration: 600 }
                                }
                            }
                            Spinner {
                                visible: sentry.connState === "connecting" || sentry.connState === "authenticating"
                                anchors.verticalCenter: parent.verticalCenter
                                ringColor: root.cs.color
                            }
                            Canvas {  // shield
                                visible: sentry.connState === "secure"
                                anchors.verticalCenter: parent.verticalCenter
                                width: 12; height: 13
                                onVisibleChanged: if (visible) requestPaint()
                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.clearRect(0, 0, width, height)
                                    ctx.fillStyle = "#5eead4"
                                    ctx.beginPath()
                                    ctx.moveTo(6, 0); ctx.lineTo(12, 2.6); ctx.lineTo(12, 7.15)
                                    ctx.lineTo(6, 13); ctx.lineTo(0, 7.15); ctx.lineTo(0, 2.6)
                                    ctx.closePath(); ctx.fill()
                                }
                                Component.onCompleted: requestPaint()
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.cs.label
                                color: root.cs.color; font.family: uiFont
                                font.pixelSize: 13; font.weight: Font.DemiBold; font.letterSpacing: 1.04
                            }
                        }
                        MouseArea {
                            id: pillMa
                            anchors.fill: parent
                            enabled: isFakeDriver  // demo: cycle states; real BLE states are driven by the link
                            onClicked: sentry.cycleConnState()
                        }
                    }
                }
            }

            // ---------------- HERO ROW ----------------
            Row {
                width: parent.width; height: 251
                spacing: 20

                Repeater {
                    model: [
                        { label: "TEMPERATURE", unit: "°C", ga: "#7fd4ff", gb: "#4aa8ff", range: "Range 15–35 °C" },
                        { label: "HUMIDITY",    unit: "%RH",     ga: "#bffcff", gb: "#4aa8ff", range: "Range 0–100 %RH" }
                    ]
                    Item {
                        width: (parent.width - 20) / 2; height: parent.height
                        readonly property bool isTemp: index === 0
                        readonly property real val: isTemp ? root.dispTemp : root.dispHum

                        Canvas {  // glow halo behind card, pulsed on data arrival
                            anchors.fill: card; anchors.margins: -50
                            opacity: root.halo * 0.85
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.shadowColor = "rgba(74,168,255,0.55)"
                                ctx.shadowBlur = 42
                                ctx.fillStyle = "rgba(74,168,255,0.28)"
                                ctx.beginPath()
                                ctx.roundedRect(50, 50, width - 100, height - 100, 18, 18)
                                ctx.fill()
                            }
                            Component.onCompleted: requestPaint()
                        }
                        GlowCard {
                            id: card
                            anchors.fill: parent
                            border.color: "#387FD4FF"

                            Column {
                                anchors.left: parent.left; anchors.leftMargin: 32
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8
                                Text {
                                    text: modelData.label
                                    color: "#7fd4ff"; opacity: 0.8; font.family: uiFont
                                    font.pixelSize: 13; font.letterSpacing: 2.08
                                }
                                Row {
                                    spacing: 6
                                    Text {
                                        text: val.toFixed(2)
                                        color: "#eaf6ff"; font.family: monoFont
                                        font.pixelSize: 64; font.weight: Font.DemiBold
                                    }
                                    Text {
                                        anchors.baseline: parent.children[0].baseline
                                        text: modelData.unit
                                        color: "#7fd4ff"; font.family: monoFont; font.pixelSize: 28
                                    }
                                }
                                Text {
                                    text: modelData.range + " · updated " + sentry.lastUpdate
                                    color: "#5a6472"; font.family: uiFont; font.pixelSize: 13
                                }
                            }
                            RingGauge {
                                anchors.right: parent.right; anchors.rightMargin: 32
                                anchors.verticalCenter: parent.verticalCenter
                                gradA: modelData.ga; gradB: modelData.gb
                                pct: isTemp ? (val - root.tempMin) / (root.tempMax - root.tempMin)
                                            : val / 100
                            }
                        }
                    }
                }
            }

            // ---------------- CHART ----------------
            Rectangle {
                width: parent.width; height: 193; radius: 18
                border.width: 1; border.color: "#297FD4FF"
                gradient: Gradient {
                    GradientStop { position: 0; color: "#D910161E" }
                    GradientStop { position: 1; color: "#D9080B10" }
                }
                Item {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 20
                    anchors.topMargin: 16; anchors.bottomMargin: 16

                    Text {
                        id: chartTitle
                        text: "LIVE TREND · LAST ~60S"
                        color: "#5a6472"; font.family: uiFont
                        font.pixelSize: 12; font.letterSpacing: 1.68
                    }
                    Row {
                        anchors.right: parent.right
                        spacing: 16
                        Repeater {
                            model: [{ name: "Temp", c: "#4aa8ff" }, { name: "Humidity", c: "#bffcff" }]
                            Row {
                                spacing: 6
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 10; height: 10; radius: 5; color: modelData.c
                                }
                                Text {
                                    text: modelData.name
                                    color: "#8b95a3"; font.family: uiFont; font.pixelSize: 12
                                }
                            }
                        }
                    }
                    Canvas {
                        id: chart
                        anchors.top: chartTitle.bottom; anchors.topMargin: 6
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        onPaint: {
                            var ctx = getContext("2d")
                            var W = width, H = height
                            ctx.clearRect(0, 0, W, H)
                            var hist = root.history
                            if (hist.length < 2) return
                            var stepX = W / (hist.length - 1)
                            function pts(key, min, max) {
                                var out = []
                                for (var i = 0; i < hist.length; i++)
                                    out.push({ x: i * stepX,
                                               y: H - ((hist[i][key] - min) / (max - min)) * (H - 6) - 3 })
                                return out
                            }
                            function drawLine(p, color, fillTop) {
                                ctx.beginPath()
                                for (var i = 0; i < p.length; i++)
                                    i === 0 ? ctx.moveTo(p[i].x, p[i].y) : ctx.lineTo(p[i].x, p[i].y)
                                ctx.strokeStyle = color
                                ctx.lineWidth = 2.2
                                ctx.lineJoin = "round"; ctx.lineCap = "round"
                                ctx.stroke()
                                var g = ctx.createLinearGradient(0, 0, 0, H)
                                g.addColorStop(0, fillTop); g.addColorStop(1, "rgba(0,0,0,0)")
                                ctx.lineTo(p[p.length - 1].x, H); ctx.lineTo(p[0].x, H)
                                ctx.closePath(); ctx.fillStyle = g; ctx.fill()
                            }
                            drawLine(pts("temp", root.tempMin, root.tempMax), "#4aa8ff", "rgba(74,168,255,0.28)")
                            drawLine(pts("hum", 0, 100), "#bffcff", "rgba(191,252,255,0.16)")
                        }
                        Component.onCompleted: requestPaint()
                    }
                }
            }

            // ---------------- BOTTOM ROW ----------------
            Row {
                width: parent.width; height: 104
                spacing: 20

                GlowCard {  // LED control
                    width: 280; height: parent.height; radius: 16
                    border.color: "#337FD4FF"
                    Text {
                        anchors.left: parent.left; anchors.leftMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: "DEVICE LED"
                        color: "#7fd4ff"; font.family: uiFont
                        font.pixelSize: 13; font.letterSpacing: 1.3
                    }
                    Rectangle {  // toggle track
                        id: ledTrack
                        anchors.right: parent.right; anchors.rightMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        width: 96; height: 52; radius: 26
                        color: "#08FFFFFF"
                        border.width: 1
                        border.color: sentry.ledOn ? "#997FD4FF" : "#407FD4FF"
                        Behavior on border.color { ColorAnimation { duration: 250 } }
                        scale: ledMa.pressed ? 0.94 : 1
                        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
                        Rectangle {  // ON track glow (crossfaded)
                            anchors.fill: parent; radius: parent.radius
                            opacity: sentry.ledOn ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 250 } }
                            gradient: Gradient {
                                GradientStop { position: 0; color: "#597FD4FF" }
                                GradientStop { position: 1; color: "#264AA8FF" }
                            }
                        }
                        Rectangle {  // knob
                            x: sentry.ledOn ? 48 : 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: 44; height: 44; radius: 22
                            Behavior on x { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }
                            gradient: Gradient {
                                GradientStop { position: 0; color: "#eaf6ff" }
                                GradientStop { position: 1; color: "#7fd4ff" }
                            }
                            Rectangle {  // OFF state gray (crossfaded over gradient)
                                anchors.fill: parent; radius: parent.radius
                                color: "#80788291"
                                opacity: sentry.ledOn ? 0 : 1
                                Behavior on opacity { NumberAnimation { duration: 250 } }
                            }
                        }
                        MouseArea { id: ledMa; anchors.fill: parent; onClicked: sentry.toggleLed() }
                    }
                }

                Rectangle {  // footer stats
                    width: parent.width - 280 - 20; height: parent.height; radius: 16
                    border.width: 1; border.color: "#247FD4FF"
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#D910161E" }
                        GradientStop { position: 1; color: "#D9080B10" }
                    }
                    Row {
                        anchors.left: parent.left; anchors.leftMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 32
                        Repeater {
                            model: [
                                { k: "DEVICE", v: sentry.deviceId.length > 8 ? sentry.deviceId.slice(-8) : sentry.deviceId },
                                { k: "LAST UPDATE", v: sentry.lastUpdate },
                                { k: "UPTIME", v: sentry.uptime }
                            ]
                            Column {
                                spacing: 2
                                Text {
                                    text: modelData.k
                                    color: "#5a6472"; font.family: uiFont
                                    font.pixelSize: 11; font.letterSpacing: 1.32
                                }
                                Text {
                                    text: modelData.v
                                    color: "#eaf6ff"; font.family: monoFont; font.pixelSize: 16
                                }
                            }
                        }
                    }
                    Rectangle {  // reconnect button
                        anchors.right: parent.right; anchors.rightMargin: 24
                        anchors.verticalCenter: parent.verticalCenter
                        width: reconnectLabel.width + 48; height: 48; radius: 10
                        color: "#147FD4FF"
                        border.width: 1; border.color: "#4D7FD4FF"
                        scale: reconnectMa.pressed ? 0.94 : 1
                        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
                        Text {
                            id: reconnectLabel
                            anchors.centerIn: parent
                            text: "Reconnect"
                            color: "#7fd4ff"; font.family: uiFont
                            font.pixelSize: 13; font.weight: Font.DemiBold; font.letterSpacing: 0.78
                        }
                        MouseArea { id: reconnectMa; anchors.fill: parent; onClicked: sentry.reconnect() }
                    }
                }
            }
        }

        // ---------------- RECONNECTING OVERLAY ----------------
        Rectangle {
            anchors.fill: parent
            color: "#8C05070A"
            opacity: sentry.reconnecting ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 300 } }

            Canvas {  // radar sweep (conic gradient as alpha-faded wedges)
                id: radar
                anchors.centerIn: parent
                width: 520; height: 520
                opacity: 0.5
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    var cx = width / 2, cy = height / 2, r = width / 2
                    var segs = 72
                    for (var i = 0; i < segs; i++) {
                        var f = i / segs
                        if (f >= 0.7) break
                        var a0 = f * 2 * Math.PI - Math.PI / 2
                        var a1 = (i + 1.3) / segs * 2 * Math.PI - Math.PI / 2
                        ctx.fillStyle = "rgba(127,212,255," + (0.35 * (1 - f / 0.7)).toFixed(3) + ")"
                        ctx.beginPath()
                        ctx.moveTo(cx, cy)
                        ctx.arc(cx, cy, r, a0, a1)
                        ctx.closePath(); ctx.fill()
                    }
                }
                Component.onCompleted: requestPaint()
                RotationAnimator on rotation {
                    from: 0; to: 360; duration: 1600
                    loops: Animation.Infinite; running: sentry.reconnecting
                }
            }
            Column {
                anchors.centerIn: parent
                spacing: 14
                Spinner {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 64; height: 64
                }
                Text {
                    text: "RECONNECTING…"
                    color: "#eaf6ff"; font.family: uiFont
                    font.pixelSize: 20; font.bold: true; font.letterSpacing: 2.8
                }
            }
        }

        // ---------------- FIRMWARE / MULTI-DEVICE QUEUE ----------------
        Rectangle {
            anchors.fill: parent; color: "#CC05070A"
            visible: root.updatePanelOpen; z: 20
            MouseArea { anchors.fill: parent; onClicked: root.updatePanelOpen = false }
            Rectangle {
                anchors.centerIn: parent; width: 820; height: 540; radius: 20
                color: "#F20A0E14"; border.width: 1; border.color: "#4D7FD4FF"
                MouseArea { anchors.fill: parent }
                Column {
                    anchors.fill: parent; anchors.margins: 28; spacing: 16
                    Row {
                        width: parent.width
                        Column {
                            width: parent.width - 160
                            Text { text: "FIRMWARE UPDATE QUEUE"; color: "#eaf6ff"; font.family: uiFont; font.pixelSize: 22; font.bold: true; font.letterSpacing: 1.2 }
                            Text { text: sentry.updateMessage; color: "#8b95a3"; font.family: uiFont; font.pixelSize: 13 }
                        }
                        Rectangle {
                            width: 150; height: 44; radius: 10; color: "#197FD4FF"; border.width: 1; border.color: "#597FD4FF"
                            Text { anchors.centerIn: parent; text: sentry.updateStatus === "checking" ? "CHECKING…" : "CHECK NOW"; color: "#7fd4ff"; font.bold: true; font.pixelSize: 12 }
                            MouseArea { anchors.fill: parent; enabled: sentry.updateStatus !== "checking"; onClicked: sentry.checkUpdates() }
                        }
                    }
                    Rectangle { width: parent.width; height: 1; color: "#267FD4FF" }
                    Text { text: "LATEST LOCAL RELEASE  " + (sentry.latestVersion || "NONE") + "    ·    " + sentry.pendingJobCount + " PENDING    ·    UPDATES RUN ONE SS1 AT A TIME"; color: "#7fd4ff"; font.family: monoFont; font.pixelSize: 12 }
                    Text {
                        visible: sentry.activeUpdateDevice.length > 0
                        text: "NOW UPDATING  " + sentry.activeUpdateDevice + "    ·    " + String(sentry.activeUpdateState).replace("_", " ").toUpperCase() + "    ·    " + sentry.updateProgress + "%"
                        color: "#5eead4"; font.family: monoFont; font.pixelSize: 12; font.bold: true
                    }
                    Item {
                        width: parent.width; height: sentry.activeUpdateDevice.length > 0 ? 312 : 340
                        Text { anchors.centerIn: parent; visible: sentry.jobs.length === 0; text: "No firmware jobs yet"; color: "#5a6472"; font.pixelSize: 16 }
                        Column {
                            width: parent.width; spacing: 8
                            Repeater {
                                model: sentry.jobs.slice(0, 6)
                                Rectangle {
                                    width: parent.width; height: 48; radius: 8; color: "#0DFFFFFF"; border.width: 1; border.color: "#267FD4FF"
                                    Row {
                                        anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 14
                                        Text { width: 180; anchors.verticalCenter: parent.verticalCenter; text: (modelData.device_name || modelData.device_id); elide: Text.ElideRight; color: "#eaf6ff"; font.family: monoFont; font.pixelSize: 12 }
                                        Text { width: 80; anchors.verticalCenter: parent.verticalCenter; text: "→ " + (modelData.target_version || "?"); color: "#7fd4ff"; font.family: monoFont; font.pixelSize: 12 }
                                        Text { width: 155; anchors.verticalCenter: parent.verticalCenter; text: modelData.queue_position ? "QUEUED #" + modelData.queue_position : String(modelData.state).replace("_", " ").toUpperCase(); color: (modelData.state === "failed" || modelData.state === "health_check_failed" || modelData.state === "confirm_failed") ? "#ff6b6b" : "#5eead4"; font.pixelSize: 11; font.bold: true }
                                        Rectangle {
                                            width: 220; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter; color: "#1AFFFFFF"
                                            Rectangle { width: parent.width * (modelData.progress || 0) / 100; height: parent.height; radius: 4; color: "#4aa8ff" }
                                        }
                                        Text { width: 40; anchors.verticalCenter: parent.verticalCenter; text: (modelData.progress || 0) + "%"; color: "#8b95a3"; font.family: monoFont; font.pixelSize: 11 }
                                    }
                                }
                            }
                        }
                    }
                    Text { text: "Trial images are confirmed only after the Pi sees 15 seconds of authenticated sensor data. Failed trials reboot into MCUboot rollback."; wrapMode: Text.WordWrap; width: parent.width; color: "#5a6472"; font.pixelSize: 11 }
                }
                Text { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; text: "×"; color: "#8b95a3"; font.pixelSize: 28; MouseArea { anchors.fill: parent; anchors.margins: -12; onClicked: root.updatePanelOpen = false } }
            }
        }
    }
}
