package me.magnum.melonds.ui.emulator.multiplayer

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.MaterialTheme
import androidx.compose.material.OutlinedTextField
import androidx.compose.material.Text
import androidx.compose.material.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.R
import me.magnum.melonds.ui.common.component.dialog.BaseDialog
import me.magnum.melonds.ui.common.melonTextButtonColors
import me.magnum.melonds.ui.common.component.dialog.DialogButton

// Mirrors LanMode in MelonDSAndroidJNI.cpp
private const val MODE_NONE = 0
private const val MODE_DISCOVERING = 1
private const val MODE_HOSTING = 2
private const val MODE_JOINED = 3

// Mirrors melonDS::LAN::PlayerStatus
private const val PLAYER_CLIENT = 1
private const val PLAYER_HOST = 2
private const val PLAYER_CONNECTING = 3
private const val PLAYER_HOST_LOST = 4 // Player_Disconnected

data class LanSession(val address: String, val name: String, val players: Int, val maxPlayers: Int, val playing: Boolean)
data class LanPlayer(val id: Int, val maxPlayers: Int, val name: String, val status: Int, val ping: Int, val isLocal: Boolean, val address: String)

private fun parseSessions(rows: Array<String>) = rows.mapNotNull { row ->
    row.split('\t').takeIf { it.size == 5 }?.let { LanSession(it[0], it[1], it[2].toInt(), it[3].toInt(), it[4] == "1") }
}

private fun parsePlayers(rows: Array<String>) = rows.mapNotNull { row ->
    row.split('\t').takeIf { it.size == 7 }?.let {
        LanPlayer(it[0].toInt(), it[1].toInt(), it[2], it[3].toInt(), it[4].toInt(), it[5] == "1", it[6])
    }
}

/**
 * The group's leader (lobby id), as the last group command set it: the session's player 0 (a
 * Hosted session's server), who sends the group's commands. "Make <player> the host" hands it over.
 */
object MultiplayerGroup {
    var server = 0
}

data class Seat(val player: Int, val players: Int, val host: String)

/** The group's players (empty when not in a group): the lobby kept connected through a session. */
fun groupRows() = parsePlayers(MelonEmulator.groupPlayers())

fun LanPlayer.live() = status == PLAYER_HOST || status == PLAYER_CLIENT

/**
 * This device's seat in a session of the group's connected players: the server (lobby id) is
 * player 0, the others follow in lobby order. host = the server's address ("" on the server).
 */
fun groupSeat(rows: List<LanPlayer>, server: Int): Seat? {
    val live = rows.filter { it.live() }
    val me = live.firstOrNull { it.isLocal } ?: return null
    val srv = live.firstOrNull { it.id == server } ?: return null
    val order = listOf(server) + live.map { it.id }.filter { it != server }.sorted()
    val player = order.indexOf(me.id)
    return Seat(player, order.size, if (player == 0) "" else srv.address)
}

// Screens before a LAN session exists; once hosting/joined the lobby replaces them
// SAME / SWITCH: a mode was picked while a session is active (resume or end it / end it and switch)
private enum class Screen { MENU, USB, LAN, HOTSPOT, HOST_HOTSPOT, WIFI, SAME, SWITCH, LEAVE_GROUP }

// What the lobby is for. Netplay / Hosted: the lobby only finds the players; the host's Start
// sends every guest into the session. LAN: the game's own wireless runs over the lobby.
private enum class Goal { NETPLAY, HOSTED, LAN, USB }

/**
 * Multiplayer dialog, opened from the pause menu: Netplay | Hosted Netplay | LAN | USB, then
 * Hotspot | Wi-Fi, then host or join the lobby (a LAN session). Hotspot: one device creates a game
 * network ([DirectLink]) and the others join it, then join the session by the host's address
 * (session discovery does not cross these networks). Wi-Fi: the current network, with discovery.
 *
 * The game stays paused while it is open, so it pumps the native LAN backend itself.
 * Netplay / Hosted: the lobby dialog cannot be closed (only Cancel lobby / Leave / Start), so the
 * paused game never talks over it; the host's Start sends a start message to every guest and each
 * device restarts the game into the session. LAN: players resume and use their game's own
 * wireless features over the session, which outlives the dialog until "Leave session" or the
 * emulator stops.
 */
@Composable
fun LanMultiplayerDialog(defaultPlayerName: String, onEndSession: suspend () -> Unit, onGroupCommand: (request: Int) -> Unit, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var screen by remember { mutableStateOf(Screen.MENU) }
    var goal by remember { mutableStateOf(Goal.LAN) }
    val netplayGoal = goal == Goal.NETPLAY || goal == Goal.HOSTED
    var playerName by remember { mutableStateOf(defaultPlayerName) }
    var maxPlayers by remember { mutableIntStateOf(2) }
    var hostAddress by remember { mutableStateOf("") }
    var mode by remember { mutableIntStateOf(MelonEmulator.lanGetMode()) }
    // What already runs when the dialog opens: a Netplay / Hosted session (netplayKind 1 / 2), or a
    // LAN session, which the menu is shown over (lanMenu) until a mode is picked. Switching to
    // another mode ends it first, so a lobby never starts over a running session's link.
    var netplayKind by remember { mutableIntStateOf(MelonEmulator.netplayKind()) }
    var lanMenu by remember { mutableStateOf(mode != MODE_NONE) }
    var pending by remember { mutableStateOf(Goal.LAN) }   // the mode picked on SAME / SWITCH
    var ending by remember { mutableStateOf(false) }       // the game restarts: no LAN polling
    // the group (the lobby kept connected through a session); the host's commands lead it
    var group by remember { mutableStateOf(groupRows()) }
    var server by remember { mutableIntStateOf(0) }         // lobby: who runs a Hosted session
    var sessions by remember { mutableStateOf(emptyList<LanSession>()) }
    var players by remember { mutableStateOf(emptyList<LanPlayer>()) }
    var busyText by remember { mutableStateOf<String?>(null) }
    var errorText by remember { mutableStateOf<String?>(null) }
    var nearbyNetworks by remember { mutableStateOf(emptyList<String>()) }
    var manualSsid by remember { mutableStateOf(DirectLink.NAME_PREFIX) }
    var manualPassword by remember { mutableStateOf(DirectLink.PASSPHRASE) }
    var canScan by remember { mutableStateOf(false) }
    // Wi-Fi scan results are also empty while the device's Location switch is off
    var locationOn by remember { mutableStateOf(true) }
    // Netplay players: the largest complete lobby seen (everyone connected, addresses known).
    // Once a device restarts into Netplay it leaves the lobby (the host leaving ends it), so the
    // others' lists shrink: a smaller list never replaces it.
    var netplayPlayers by remember { mutableStateOf<List<LanPlayer>?>(null) }
    // a hotspot action waiting on the runtime permission prompt
    var afterPermission by remember { mutableStateOf<(() -> Unit)?>(null) }
    // the nearby-games notice asked for location while Android had stopped showing its prompt
    var scanPromptBlocked by remember { mutableStateOf(false) }

    fun granted(permission: String) = ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED
    // Wi-Fi Direct / local-only hotspot: NEARBY_WIFI_DEVICES on Android 13+, location before
    fun hasNetworkPermission() = granted(
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) Manifest.permission.NEARBY_WIFI_DEVICES else Manifest.permission.ACCESS_FINE_LOCATION
    )

    // Wi-Fi drivers drop broadcast packets (session discovery beacons) unless a multicast lock is held.
    DisposableEffect(Unit) {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val lock = wifi.createMulticastLock("sereneds-lan").apply {
            setReferenceCounted(false)
            acquire()
        }
        onDispose { lock.release() }
    }

    val hostLeft = stringResource(R.string.multiplayer_host_left)

    // a guest starts as soon as the host's start arrives: the lobby becomes the group, and the
    // game restarts into the session
    fun guestStart(request: Int) {
        scope.launch(Dispatchers.IO) {
            MelonEmulator.lanToGroup()
            withContext(Dispatchers.Main) { onGroupCommand(request) }
        }
    }

    LaunchedEffect(Unit) {
        while (true) {
            if (ending) {
                delay(100)
                continue
            }
            var startRequest = -1
            val (newMode, newSessions, newPlayers) = withContext(Dispatchers.IO) {
                MelonEmulator.lanTick()
                val m = MelonEmulator.lanGetMode()
                if (m == MODE_JOINED) startRequest = MelonEmulator.lanGetStartRequest()
                Triple(
                    m,
                    if (m == MODE_DISCOVERING) parseSessions(MelonEmulator.lanGetSessions()) else emptyList(),
                    if (m == MODE_HOSTING || m == MODE_JOINED) parsePlayers(MelonEmulator.lanGetPlayers()) else emptyList(),
                )
            }
            group = withContext(Dispatchers.IO) { groupRows() }
            mode = newMode
            sessions = newSessions
            players = newPlayers
            if (newMode != MODE_HOSTING && newMode != MODE_JOINED) {
                netplayPlayers = null
            } else if (newPlayers.size >= 2 && newPlayers.size >= (netplayPlayers?.size ?: 0) &&
                newPlayers.count { it.isLocal } == 1 &&
                newPlayers.all { it.isLocal || (it.status != PLAYER_CONNECTING && it.address != "127.0.0.1" && it.address != "0.0.0.0") }
            ) {
                netplayPlayers = newPlayers
            }
            if (startRequest >= 0) {
                guestStart(startRequest)
                return@LaunchedEffect
            }
            // the host cancelled the Netplay lobby: back to the start
            if (netplayGoal && newMode == MODE_JOINED && newPlayers.any { it.status == PLAYER_HOST_LOST && !it.isLocal && it.id == 0 }) {
                withContext(Dispatchers.IO) { MelonEmulator.lanLeave() }
                errorText = hostLeft
            }
            delay(100)
        }
    }

    // Nearby SereneDS networks, while the Hotspot screen is open
    LaunchedEffect(screen, mode) {
        if (screen != Screen.HOTSPOT || mode != MODE_NONE) return@LaunchedEffect
        var polls = 0
        while (true) {
            canScan = granted(Manifest.permission.ACCESS_FINE_LOCATION)
            locationOn = (context.getSystemService(Context.LOCATION_SERVICE) as? LocationManager)
                ?.let(LocationManagerCompat::isLocationEnabled) ?: true
            // Android throttles scan requests (4 per 2 minutes); results also arrive from system scans
            if (polls++ % 15 == 0) DirectLink.requestScan(context)
            nearbyNetworks = withContext(Dispatchers.IO) { DirectLink.nearbyNetworks(context) }
            delay(2000)
        }
    }

    fun runAction(progress: String, failure: String, action: suspend () -> Boolean) {
        scope.launch {
            errorText = null
            busyText = progress
            val ok = withContext(Dispatchers.IO) { action() }
            busyText = null
            if (!ok) errorText = failure
        }
    }

    val permissionDenied = stringResource(R.string.multiplayer_error_permission_denied)
    fun openAppSettings() {
        runCatching {
            context.startActivity(
                Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        }
    }

    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        canScan = granted(Manifest.permission.ACCESS_FINE_LOCATION)
        // Android did not show its prompt (refused twice before): only the app's settings page can grant it
        if (scanPromptBlocked && !canScan) openAppSettings()
        scanPromptBlocked = false
        val next = afterPermission
        afterPermission = null
        if (next != null) {
            if (hasNetworkPermission()) next() else errorText = permissionDenied
        }
    }

    // Asks for the hotspot permissions that are still missing, then runs `action` (if any) when the
    // ones it needs were granted.
    fun withNetworkPermission(action: (() -> Unit)?) {
        val missing = DirectLink.permissions.filterNot(::granted)
        if (action != null && hasNetworkPermission()) {
            action()
        } else if (missing.isNotEmpty()) {
            afterPermission = action
            permissionLauncher.launch(missing.toTypedArray())
        }
    }

    val connectionFailed = stringResource(R.string.multiplayer_error_connection_failed)
    val lanInitFailed = stringResource(R.string.multiplayer_error_lan_init_failed)
    val discoveryFailed = stringResource(R.string.multiplayer_error_discovery_failed)
    val starting = stringResource(R.string.multiplayer_starting)
    val connecting = stringResource(R.string.multiplayer_connecting)
    val discovering = stringResource(R.string.multiplayer_discovering)
    val directStarting = stringResource(R.string.multiplayer_direct_starting)
    val directJoining = stringResource(R.string.multiplayer_direct_joining)
    val directFailed = stringResource(R.string.multiplayer_error_direct_failed)
    val name = playerName.ifBlank { defaultPlayerName }

    fun join(address: String) = runAction(connecting, connectionFailed) {
        MelonEmulator.lanJoin(name, address.trim()).also { ok ->
            if (!ok) MelonEmulator.lanStartDiscovery()
        }
    }

    fun hostHotspot() = withNetworkPermission {
        runAction(directStarting, directFailed) {
            DirectLink.host(context) != null &&
                MelonEmulator.lanHost(name, maxPlayers).also { if (!it) DirectLink.leave(context) }
        }
    }

    // joins the game network, then the LAN session at the host's address (no discovery)
    fun joinHotspot(ssid: String, password: String) = runAction(directJoining, directFailed) {
        val host = DirectLink.join(context, ssid, password)
        host != null && MelonEmulator.lanJoin(name, host).also { if (!it) DirectLink.leave(context) }
    }

    fun open(next: Screen) {
        errorText = null
        if (next == Screen.HOTSPOT) withNetworkPermission(null)
        screen = next
    }

    val goalTitle = when (goal) {
        Goal.NETPLAY -> R.string.multiplayer_option_netplay
        Goal.HOSTED -> R.string.multiplayer_option_hosted
        Goal.LAN -> R.string.multiplayer_lan_title
        Goal.USB -> R.string.multiplayer_option_usb
    }
    val showLobby = mode != MODE_NONE && !lanMenu
    val title = when {
        showLobby -> goalTitle
        screen == Screen.MENU || screen == Screen.SAME || screen == Screen.SWITCH || screen == Screen.LEAVE_GROUP -> R.string.multiplayer_role_title
        screen == Screen.USB -> R.string.multiplayer_option_usb
        screen == Screen.HOTSPOT || screen == Screen.HOST_HOTSPOT -> R.string.multiplayer_hotspot_title
        else -> goalTitle
    }

    fun leave() {
        scope.launch(Dispatchers.IO) { MelonEmulator.lanLeave() }
        DirectLink.leave(context)
    }

    val active = when {
        netplayKind == 2 -> Goal.HOSTED
        netplayKind == 1 -> Goal.NETPLAY
        lanMenu && mode != MODE_NONE -> Goal.LAN
        else -> null
    }

    fun enter(next: Goal) {
        goal = next
        lanMenu = false
        if (next == Goal.NETPLAY || next == Goal.HOSTED) maxPlayers = 2
        open(if (next == Goal.USB) Screen.USB else Screen.LAN)
    }

    fun pick(next: Goal) {
        pending = next
        when (active) {
            null -> enter(next)
            next -> open(Screen.SAME)
            else -> open(Screen.SWITCH)
        }
    }

    // Ends what is active: Netplay / Hosted restarts the game without the session (paused, the
    // dialog stays); LAN leaves the session and its game network. Then `then`.
    val endingText = stringResource(R.string.multiplayer_ending)
    fun endActive(then: () -> Unit) = scope.launch {
        errorText = null
        busyText = endingText
        ending = true
        if (netplayKind != 0) {
            onEndSession()
            netplayKind = MelonEmulator.netplayKind()
        } else {
            withContext(Dispatchers.IO) { MelonEmulator.lanLeave() }
            DirectLink.leave(context)
            mode = MODE_NONE
        }
        ending = false
        busyText = null
        then()
    }

    // ends the session; a Netplay one restarts the game, so the dialog closes onto it
    fun endOnly() {
        val wasNetplay = netplayKind != 0
        endActive { if (wasNetplay) onDismiss() else open(Screen.MENU) }
    }

    fun modeName(g: Goal) = when (g) {
        Goal.NETPLAY -> R.string.multiplayer_option_netplay
        Goal.HOSTED -> R.string.multiplayer_option_hosted
        Goal.LAN -> R.string.multiplayer_option_lan
        Goal.USB -> R.string.multiplayer_option_usb
    }

    // Netplay lobby: back / outside taps do nothing; it is left only by Cancel lobby / Leave or
    // the start, so nobody backs out thinking Netplay is broken and the paused game never runs
    // linked to the others over LAN
    val inNetplayLobby = netplayGoal && (mode == MODE_HOSTING || mode == MODE_JOINED)
    val dismiss = { if (!inNetplayLobby && busyText == null) onDismiss() }

    // Host: tell every guest to start, keep the lobby as the group, then start too
    val startingNetplay = stringResource(R.string.multiplayer_starting_netplay)
    fun hostStart(list: List<LanPlayer>) = scope.launch {
        busyText = startingNetplay
        val mode = if (goal == Goal.HOSTED) 1 else 0
        val srv = if (mode == 1 && list.any { it.id == server }) server else 0
        withContext(Dispatchers.IO) {
            MelonEmulator.lanStartSession(mode == 1, list.size, srv)
            MelonEmulator.lanToGroup()
        }
        onGroupCommand((srv shl 16) or (mode shl 8) or list.size)
    }

    // Group leader: send a command to everyone and run it here too
    fun hostCommand(cmdMode: Int, srv: Int) {
        val n = group.count { it.live() }
        MelonEmulator.groupSend(cmdMode, n, srv)
        onGroupCommand((srv shl 16) or (cmdMode shl 8) or n)
    }

    val leader = group.firstOrNull { it.id == MultiplayerGroup.server && it.live() }
    val inGroup = group.any { it.isLocal }
    val isLeader = leader?.isLocal == true

    BaseDialog(
        title = stringResource(title),
        onDismiss = dismiss,
        content = { padding ->
            Column(Modifier.padding(padding), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!showLobby && active != null && screen in listOf(Screen.MENU, Screen.SAME, Screen.SWITCH, Screen.LEAVE_GROUP)) {
                    val lanRole = stringResource(
                        when (mode) {
                            MODE_HOSTING -> R.string.multiplayer_lan_hosting
                            MODE_JOINED -> R.string.multiplayer_lan_joined
                            else -> R.string.multiplayer_lan_searching
                        }
                    )
                    val status = when (active) {
                        Goal.HOSTED -> stringResource(R.string.multiplayer_in_hosted)
                        Goal.LAN -> stringResource(R.string.multiplayer_in_lan, lanRole)
                        else -> stringResource(R.string.multiplayer_in_netplay)
                    }
                    Text(status, style = MaterialTheme.typography.subtitle2, color = MaterialTheme.colors.secondary)
                }
                when (if (showLobby) mode else MODE_NONE) {
                    MODE_NONE -> {
                        when (screen) {
                            Screen.MENU -> if (inGroup) {
                                val idle = busyText == null
                                Text(
                                    if (isLeader) stringResource(R.string.multiplayer_group_leader, group.count { it.live() })
                                    else stringResource(R.string.multiplayer_following, leader?.name ?: "?"),
                                    style = MaterialTheme.typography.subtitle1,
                                )
                                if (isLeader && group.count { it.live() } <= 1) {
                                    // everyone else left: the group stays open for them to rejoin, and picking a
                                    // mode here leaves it (ending any session) and starts that mode's setup
                                    fun leaveThen(next: Goal) {
                                        MelonEmulator.groupLeave()
                                        MultiplayerGroup.server = 0
                                        group = emptyList()
                                        pending = next
                                        if (active != null) endActive { enter(next) } else enter(next)
                                    }
                                    OptionRow(stringResource(R.string.multiplayer_option_netplay), stringResource(R.string.multiplayer_option_netplay_hint), idle) { leaveThen(Goal.NETPLAY) }
                                    OptionRow(stringResource(R.string.multiplayer_option_hosted), stringResource(R.string.multiplayer_option_hosted_hint), idle) { leaveThen(Goal.HOSTED) }
                                    OptionRow(stringResource(R.string.multiplayer_option_lan), stringResource(R.string.multiplayer_option_lan_hint), idle) { leaveThen(Goal.LAN) }
                                } else if (isLeader) {
                                    // Session: switch the whole group's mode, or change who runs a Hosted session
                                    Text(stringResource(R.string.multiplayer_session_section), style = MaterialTheme.typography.subtitle2)
                                    listOf(Goal.NETPLAY to 0, Goal.HOSTED to 1, Goal.LAN to 2).filter { it.first != active }.forEach { (g, m) ->
                                        OptionRow(stringResource(R.string.multiplayer_switch_to, stringResource(modeName(g))), stringResource(R.string.multiplayer_switch_to_hint), idle) {
                                            hostCommand(m, MultiplayerGroup.server)
                                        }
                                    }
                                    if (active == Goal.HOSTED) group.filter { it.live() && it.id != MultiplayerGroup.server }.forEach { p ->
                                        OptionRow(stringResource(R.string.multiplayer_make_host, p.name), stringResource(R.string.multiplayer_make_host_hint), idle) {
                                            hostCommand(1, p.id)
                                        }
                                    }
                                } else {
                                    Text(stringResource(R.string.multiplayer_following_hint), style = MaterialTheme.typography.body2)
                                }
                            } else {
                                val idle = busyText == null
                                OptionRow(stringResource(R.string.multiplayer_option_netplay), stringResource(R.string.multiplayer_option_netplay_hint), idle) { pick(Goal.NETPLAY) }
                                OptionRow(stringResource(R.string.multiplayer_option_hosted), stringResource(R.string.multiplayer_option_hosted_hint), idle) { pick(Goal.HOSTED) }
                                OptionRow(stringResource(R.string.multiplayer_option_lan), stringResource(R.string.multiplayer_option_lan_hint), idle) { pick(Goal.LAN) }
                                OptionRow(stringResource(R.string.multiplayer_option_usb), stringResource(R.string.multiplayer_option_usb_hint), idle) { pick(Goal.USB) }
                            }
                            Screen.SAME -> Text(stringResource(R.string.multiplayer_same_mode_hint), style = MaterialTheme.typography.body2)
                            Screen.LEAVE_GROUP -> Text(stringResource(R.string.multiplayer_leave_group_confirm), style = MaterialTheme.typography.body2)
                            Screen.SWITCH -> Text(
                                stringResource(R.string.multiplayer_switch_confirm, stringResource(modeName(active ?: Goal.LAN)), stringResource(modeName(pending))),
                                style = MaterialTheme.typography.body2,
                            )
                            Screen.USB -> Text(stringResource(R.string.multiplayer_usb_unavailable), style = MaterialTheme.typography.body2)
                            Screen.LAN -> {
                                OptionRow(
                                    stringResource(R.string.multiplayer_option_hotspot),
                                    stringResource(if (DirectLink.isSupported) R.string.multiplayer_option_hotspot_hint else R.string.multiplayer_hotspot_unsupported),
                                    enabled = DirectLink.isSupported,
                                ) { open(Screen.HOTSPOT) }
                                OptionRow(stringResource(R.string.multiplayer_option_wifi), stringResource(R.string.multiplayer_option_wifi_hint)) { open(Screen.WIFI) }
                            }
                            Screen.HOTSPOT -> HotspotContent(
                                networks = nearbyNetworks,
                                canScan = canScan,
                                locationOn = locationOn,
                                // Android's own prompt first (settings page only once it stops showing),
                                // or the Location switch when the permission is there but Location is off
                                onFixScan = {
                                    if (!canScan) {
                                        // the screen already asked once on opening; after one refusal Android
                                        // reports a rationale, after two it stops prompting and reports none
                                        // dialogs hand out a themed wrapper around the activity
                                        val activity = generateSequence(context) { (it as? ContextWrapper)?.baseContext }.firstNotNullOfOrNull { it as? Activity }
                                        scanPromptBlocked = activity == null ||
                                            !ActivityCompat.shouldShowRequestPermissionRationale(activity, Manifest.permission.ACCESS_FINE_LOCATION)
                                        permissionLauncher.launch(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION))
                                    } else {
                                        runCatching { context.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
                                    }
                                },
                                enabled = busyText == null,
                                onHost = { open(Screen.HOST_HOTSPOT) },
                                onNetworkSelected = { joinHotspot(it, DirectLink.PASSPHRASE) },
                                ssid = manualSsid,
                                onSsidChange = { manualSsid = it },
                                password = manualPassword,
                                onPasswordChange = { manualPassword = it },
                                playerName = playerName,
                                onPlayerNameChange = { playerName = it.take(10) },
                            )
                            Screen.HOST_HOTSPOT -> {
                                Text(stringResource(R.string.multiplayer_hotspot_confirm_hint), style = MaterialTheme.typography.body2)
                                Text(stringResource(R.string.multiplayer_hotspot_password, DirectLink.PASSPHRASE), style = MaterialTheme.typography.subtitle1)
                                StartContent(
                                    playerName = playerName,
                                    onPlayerNameChange = { playerName = it.take(10) },
                                    maxPlayers = maxPlayers,
                                    onMaxPlayersChange = { maxPlayers = it.coerceIn(2, 16) },
                                    sameNetworkHint = false,
                                )
                            }
                            Screen.WIFI -> StartContent(
                                playerName = playerName,
                                onPlayerNameChange = { playerName = it.take(10) },
                                maxPlayers = maxPlayers,
                                onMaxPlayersChange = { maxPlayers = it.coerceIn(2, 16) },
                                sameNetworkHint = true,
                            )
                        }
                    }
                    MODE_DISCOVERING -> DiscoveryContent(
                        sessions = sessions,
                        hostAddress = hostAddress,
                        onHostAddressChange = { hostAddress = it },
                        onSessionSelected = { join(it.address) },
                    )
                    else -> LobbyContent(
                        hosting = mode == MODE_HOSTING, players = players, network = DirectLink.hosted, netplayGoal = netplayGoal,
                        server = server,
                        // Hosted: the host may hand the server role (runs every console) to a stronger device
                        onPickServer = if (goal == Goal.HOSTED && mode == MODE_HOSTING) { id -> server = id } else null,
                    )
                }
                busyText?.let { Text(it, style = MaterialTheme.typography.caption) }
                errorText?.let { Text(it, color = MaterialTheme.colors.error, style = MaterialTheme.typography.caption) }
            }
        },
        buttons = {
            val idle = busyText == null
            when (if (showLobby) mode else MODE_NONE) {
                MODE_NONE -> {
                    val parent = when (screen) {
                        Screen.MENU -> null
                        Screen.USB, Screen.LAN, Screen.SAME, Screen.SWITCH, Screen.LEAVE_GROUP -> Screen.MENU
                        Screen.HOTSPOT, Screen.WIFI -> Screen.LAN
                        Screen.HOST_HOTSPOT -> Screen.HOTSPOT
                    }
                    parent?.let { DialogButton(stringResource(R.string.multiplayer_back), enabled = idle) { open(it) } }
                    when (screen) {
                        Screen.HOTSPOT -> DialogButton(stringResource(R.string.multiplayer_role_join), enabled = idle && manualSsid.isNotBlank() && manualSsid.trim() != DirectLink.NAME_PREFIX) {
                            joinHotspot(DirectLink.normalizeTyped(manualSsid), manualPassword)
                        }
                        Screen.HOST_HOTSPOT -> DialogButton(stringResource(R.string.multiplayer_confirm), enabled = idle) { hostHotspot() }
                        Screen.WIFI -> {
                            DialogButton(stringResource(R.string.multiplayer_role_host), enabled = idle) {
                                runAction(starting, lanInitFailed) { MelonEmulator.lanHost(name, maxPlayers) }
                            }
                            DialogButton(stringResource(R.string.multiplayer_role_join), enabled = idle) {
                                runAction(discovering, discoveryFailed) { MelonEmulator.lanStartDiscovery() }
                            }
                        }
                        Screen.MENU -> if (inGroup) {
                            if (isLeader && active != null) {
                                DialogButton(stringResource(R.string.multiplayer_end_session), enabled = idle) { hostCommand(3, MultiplayerGroup.server) }
                            }
                            DialogButton(stringResource(R.string.multiplayer_leave_group), enabled = idle) { open(Screen.LEAVE_GROUP) }
                        } else if (active != null) {
                            val endLabel = if (active == Goal.LAN) R.string.multiplayer_leave_lan else R.string.multiplayer_end_session
                            DialogButton(stringResource(endLabel), enabled = idle) { endOnly() }
                        }
                        Screen.SAME -> {
                            DialogButton(stringResource(if (active == Goal.LAN) R.string.multiplayer_leave_lan else R.string.multiplayer_end_session), enabled = idle) { endOnly() }
                            DialogButton(stringResource(R.string.multiplayer_back_to_session), enabled = idle) {
                                // LAN: its lobby (players, Leave, Resume); Netplay: back to the game
                                if (active == Goal.LAN) lanMenu = false else onDismiss()
                            }
                        }
                        Screen.LEAVE_GROUP -> DialogButton(stringResource(R.string.multiplayer_leave_group), enabled = idle) {
                            MelonEmulator.groupLeave()
                            MultiplayerGroup.server = 0
                            group = emptyList()
                            if (netplayKind != 0) endActive { onDismiss() } else open(Screen.MENU)
                        }
                        Screen.SWITCH -> DialogButton(stringResource(R.string.multiplayer_end_and_switch), enabled = idle) {
                            val next = pending
                            endActive { enter(next) }
                        }
                        else -> {}
                    }
                }
                MODE_DISCOVERING -> {
                    DialogButton(stringResource(R.string.multiplayer_back), enabled = idle) {
                        scope.launch(Dispatchers.IO) { MelonEmulator.lanLeave() }
                    }
                    DialogButton(stringResource(R.string.multiplayer_connect), enabled = idle && hostAddress.isNotBlank()) {
                        join(hostAddress)
                    }
                }
                else -> {
                    val leaveLabel = if (netplayGoal && mode == MODE_HOSTING) R.string.multiplayer_cancel_lobby else R.string.multiplayer_leave
                    DialogButton(stringResource(leaveLabel), enabled = idle) { leave() }
                    // Netplay: only the host starts, once everyone has joined; the guests follow
                    if (netplayGoal && mode == MODE_HOSTING) {
                        val netplay = netplayPlayers
                        DialogButton(stringResource(R.string.multiplayer_start), enabled = idle && netplay != null) {
                            netplay?.let { hostStart(it) }
                        }
                    }
                }
            }
            // in a Netplay lobby the game stays paused until the session starts
            if (!inNetplayLobby) {
                DialogButton(stringResource(R.string.multiplayer_resume), enabled = idle, onClick = onDismiss)
            }
        },
    )
}

@Composable
private fun OptionRow(title: String, hint: String, enabled: Boolean = true, onClick: () -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(vertical = 8.dp)
    ) {
        val color = if (enabled) MaterialTheme.colors.onSurface else MaterialTheme.colors.onSurface.copy(alpha = 0.5f)
        Text(title, style = MaterialTheme.typography.subtitle1, color = color)
        Text(hint, style = MaterialTheme.typography.caption, color = color)
    }
}

@Composable
private fun StartContent(
    playerName: String,
    onPlayerNameChange: (String) -> Unit,
    maxPlayers: Int,
    onMaxPlayersChange: (Int) -> Unit,
    sameNetworkHint: Boolean,
) {
    OutlinedTextField(
        value = playerName,
        onValueChange = onPlayerNameChange,
        label = { Text(stringResource(R.string.multiplayer_player_name)) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(stringResource(R.string.multiplayer_max_players), Modifier.weight(1f))
        // the dialog buttons' colour: the default (primary) is near-invisible on the dark theme
        TextButton(onClick = { onMaxPlayersChange(maxPlayers - 1) }, colors = melonTextButtonColors()) { Text("−", style = MaterialTheme.typography.h5) }
        Text(maxPlayers.toString(), style = MaterialTheme.typography.h6)
        TextButton(onClick = { onMaxPlayersChange(maxPlayers + 1) }, colors = melonTextButtonColors()) { Text("+", style = MaterialTheme.typography.h5) }
    }
    if (sameNetworkHint) {
        Text(stringResource(R.string.multiplayer_same_version_hint), style = MaterialTheme.typography.caption)
    }
}

@Composable
private fun HotspotContent(
    networks: List<String>,
    canScan: Boolean,
    locationOn: Boolean,
    onFixScan: () -> Unit,
    enabled: Boolean,
    onHost: () -> Unit,
    onNetworkSelected: (String) -> Unit,
    ssid: String,
    onSsidChange: (String) -> Unit,
    password: String,
    onPasswordChange: (String) -> Unit,
    playerName: String,
    onPlayerNameChange: (String) -> Unit,
) {
    OptionRow(stringResource(R.string.multiplayer_host_hotspot), stringResource(R.string.multiplayer_host_hotspot_hint), enabled, onHost)
    Text(stringResource(R.string.multiplayer_nearby_networks), style = MaterialTheme.typography.subtitle2)
    when {
        !canScan || !locationOn -> Text(
            stringResource(if (!canScan) R.string.multiplayer_location_needed else R.string.multiplayer_location_off),
            style = MaterialTheme.typography.body2,
            color = MaterialTheme.colors.primary,
            modifier = Modifier.fillMaxWidth().clickable(onClick = onFixScan).padding(vertical = 8.dp),
        )
        networks.isEmpty() -> Text(stringResource(R.string.multiplayer_no_networks), style = MaterialTheme.typography.body2)
    }
    networks.forEach { network ->
        Text(
            text = network,
            style = MaterialTheme.typography.body1,
            modifier = Modifier
                .fillMaxWidth()
                .clickable(enabled = enabled) { onNetworkSelected(network) }
                .padding(vertical = 8.dp),
        )
    }
    // manual entry: networks the scan does not show, or a fallback hotspot Android named
    OutlinedTextField(
        value = ssid,
        onValueChange = onSsidChange,
        label = { Text(stringResource(R.string.multiplayer_network_name)) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedTextField(
        value = password,
        onValueChange = onPasswordChange,
        label = { Text(stringResource(R.string.multiplayer_network_password)) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedTextField(
        value = playerName,
        onValueChange = onPlayerNameChange,
        label = { Text(stringResource(R.string.multiplayer_player_name)) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    Text(stringResource(R.string.multiplayer_permission_rationale), style = MaterialTheme.typography.caption)
}

@Composable
private fun DiscoveryContent(
    sessions: List<LanSession>,
    hostAddress: String,
    onHostAddressChange: (String) -> Unit,
    onSessionSelected: (LanSession) -> Unit,
) {
    Text(stringResource(R.string.multiplayer_discovered_peers_title), style = MaterialTheme.typography.subtitle2)
    if (sessions.isEmpty()) {
        Text(stringResource(R.string.multiplayer_searching), style = MaterialTheme.typography.body2)
    }
    sessions.forEach { session ->
        val status = stringResource(if (session.playing) R.string.multiplayer_session_playing else R.string.multiplayer_session_waiting)
        Text(
            text = "${session.name}  ·  ${session.players}/${session.maxPlayers}  ·  $status\n${session.address}",
            style = MaterialTheme.typography.body2,
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onSessionSelected(session) }
                .padding(vertical = 8.dp),
        )
    }
    OutlinedTextField(
        value = hostAddress,
        onValueChange = onHostAddressChange,
        label = { Text(stringResource(R.string.multiplayer_host_address)) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        modifier = Modifier.fillMaxWidth(),
    )
}

@Composable
private fun LobbyContent(hosting: Boolean, players: List<LanPlayer>, network: HostedNetwork?, netplayGoal: Boolean, server: Int, onPickServer: ((Int) -> Unit)?) {
    Text(
        stringResource(if (hosting) R.string.multiplayer_hosting else R.string.multiplayer_connected),
        style = MaterialTheme.typography.subtitle2,
    )
    players.forEach { player ->
        val status = stringResource(
            when (player.status) {
                PLAYER_HOST -> R.string.multiplayer_player_host
                PLAYER_CLIENT -> R.string.multiplayer_player_connected
                PLAYER_CONNECTING -> R.string.multiplayer_player_connecting
                else -> R.string.multiplayer_player_lost
            }
        )
        val where = if (player.isLocal) stringResource(R.string.multiplayer_player_you) else "${player.ping} ms  ·  ${player.address}"
        val runs = if (onPickServer != null && player.id == server) "  ·  " + stringResource(R.string.multiplayer_runs_consoles) else ""
        Text(
            text = "${player.id + 1}/${player.maxPlayers}  ${player.name}  ·  $status  ·  $where$runs",
            style = MaterialTheme.typography.body2,
            modifier = if (onPickServer != null) Modifier.fillMaxWidth().clickable { onPickServer(player.id) }.padding(vertical = 6.dp) else Modifier,
        )
    }
    network?.let {
        Text(stringResource(R.string.multiplayer_hosting_network, it.ssid, it.passphrase), style = MaterialTheme.typography.body2)
        // a plain local-only hotspot: Android chose the name, so players cannot find it by scanning
        if (!it.wifiDirect && !DirectLink.isOurName(it.ssid)) {
            Text(stringResource(R.string.multiplayer_hotspot_fallback), style = MaterialTheme.typography.caption)
        }
    }
    val hint = when {
        !netplayGoal -> R.string.multiplayer_lobby_hint
        hosting -> R.string.multiplayer_netplay_host_hint
        else -> R.string.multiplayer_netplay_guest_hint
    }
    Text(stringResource(hint), style = MaterialTheme.typography.body2)
    if (onPickServer != null) Text(stringResource(R.string.multiplayer_pick_server_hint), style = MaterialTheme.typography.caption)
}
