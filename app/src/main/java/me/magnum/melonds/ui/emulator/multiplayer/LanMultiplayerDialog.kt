package me.magnum.melonds.ui.emulator.multiplayer

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
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
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.R
import me.magnum.melonds.ui.common.component.dialog.BaseDialog
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

// Screens before a LAN session exists; once hosting/joined the lobby replaces them
private enum class Screen { MENU, USB, LAN, HOTSPOT, HOST_HOTSPOT, WIFI }

/**
 * Multiplayer dialog, opened from the pause menu: USB | Netplay | LAN, LAN = Hotspot | Wi-Fi.
 * Hotspot: one device creates a game network ([DirectLink]) and the others join it, then join the
 * LAN session by the host's address (session discovery does not cross these networks). Wi-Fi: the
 * LAN session over the current network, with discovery. Netplay starts from a connected 2-player
 * LAN session (Start Netplay on both devices), so the Netplay entry runs the same connect flow and
 * only changes the hints.
 *
 * The game stays paused while it is open, so it pumps the native LAN backend itself; once
 * connected, players use their game's own wireless features (e.g. Union Room) and the emulator
 * routes the DS wireless traffic over the network. The session outlives the dialog: it ends on
 * "Leave session" or when the emulator stops.
 */
@Composable
fun LanMultiplayerDialog(defaultPlayerName: String, onStartNetplay: (player: Int, peer: String) -> Unit, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var screen by remember { mutableStateOf(Screen.MENU) }
    var netplayGoal by remember { mutableStateOf(false) }
    var playerName by remember { mutableStateOf(defaultPlayerName) }
    var maxPlayers by remember { mutableIntStateOf(2) }
    var hostAddress by remember { mutableStateOf("") }
    var mode by remember { mutableIntStateOf(MODE_NONE) }
    var sessions by remember { mutableStateOf(emptyList<LanSession>()) }
    var players by remember { mutableStateOf(emptyList<LanPlayer>()) }
    var busyText by remember { mutableStateOf<String?>(null) }
    var errorText by remember { mutableStateOf<String?>(null) }
    var hotspotName by remember { mutableStateOf(DirectLink.randomName()) }
    var nearbyNetworks by remember { mutableStateOf(emptyList<String>()) }
    var manualSsid by remember { mutableStateOf("") }
    var manualPassword by remember { mutableStateOf(DirectLink.PASSPHRASE) }
    var canScan by remember { mutableStateOf(false) }
    // Netplay peer, remembered while both players are connected: once one side restarts into
    // Netplay it leaves the lobby, and the other side's list then no longer has its address
    var netplayPeer by remember { mutableStateOf<String?>(null) }
    // a hotspot action waiting on the runtime permission prompt
    var afterPermission by remember { mutableStateOf<(() -> Unit)?>(null) }

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

    LaunchedEffect(Unit) {
        while (true) {
            val (newMode, newSessions, newPlayers) = withContext(Dispatchers.IO) {
                MelonEmulator.lanTick()
                val m = MelonEmulator.lanGetMode()
                Triple(
                    m,
                    if (m == MODE_DISCOVERING) parseSessions(MelonEmulator.lanGetSessions()) else emptyList(),
                    if (m == MODE_HOSTING || m == MODE_JOINED) parsePlayers(MelonEmulator.lanGetPlayers()) else emptyList(),
                )
            }
            mode = newMode
            sessions = newSessions
            players = newPlayers
            newPlayers.singleOrNull { !it.isLocal && it.status != PLAYER_CONNECTING }
                ?.takeIf { newPlayers.size == 2 && it.address != "127.0.0.1" && it.address != "0.0.0.0" }
                ?.let { netplayPeer = it.address }
            delay(100)
        }
    }

    // Nearby SereneDS networks, while the Hotspot screen is open
    LaunchedEffect(screen, mode) {
        if (screen != Screen.HOTSPOT || mode != MODE_NONE) return@LaunchedEffect
        var polls = 0
        while (true) {
            canScan = granted(Manifest.permission.ACCESS_FINE_LOCATION)
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
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        canScan = granted(Manifest.permission.ACCESS_FINE_LOCATION)
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
            DirectLink.host(context, hotspotName) != null &&
                MelonEmulator.lanHost(name, maxPlayers).also { if (!it) DirectLink.leave(context) }
        }
    }

    // joins the game network, then the LAN session at the host's address (no discovery)
    fun joinHotspot(ssid: String, password: String) = runAction(directJoining, directFailed) {
        val host = DirectLink.join(context, ssid.trim(), password)
        host != null && MelonEmulator.lanJoin(name, host).also { if (!it) DirectLink.leave(context) }
    }

    fun open(next: Screen) {
        errorText = null
        if (next == Screen.HOTSPOT) withNetworkPermission(null)
        if (next == Screen.HOST_HOTSPOT) hotspotName = DirectLink.randomName()
        screen = next
    }

    val title = when {
        mode != MODE_NONE -> R.string.multiplayer_lan_title
        screen == Screen.MENU -> R.string.multiplayer_role_title
        screen == Screen.USB -> R.string.multiplayer_option_usb
        screen == Screen.HOTSPOT || screen == Screen.HOST_HOTSPOT -> R.string.multiplayer_hotspot_title
        netplayGoal -> R.string.multiplayer_netplay_title
        else -> R.string.multiplayer_lan_title
    }

    BaseDialog(
        title = stringResource(title),
        onDismiss = onDismiss,
        content = { padding ->
            Column(Modifier.padding(padding), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                when (mode) {
                    MODE_NONE -> {
                        if (netplayGoal && screen != Screen.MENU) {
                            Text(stringResource(R.string.multiplayer_netplay_connect_hint), style = MaterialTheme.typography.caption)
                        }
                        when (screen) {
                            Screen.MENU -> {
                                OptionRow(stringResource(R.string.multiplayer_option_usb), stringResource(R.string.multiplayer_option_usb_hint)) { open(Screen.USB) }
                                OptionRow(stringResource(R.string.multiplayer_option_netplay), stringResource(R.string.multiplayer_option_netplay_hint)) {
                                    netplayGoal = true
                                    maxPlayers = 2
                                    open(Screen.LAN)
                                }
                                OptionRow(stringResource(R.string.multiplayer_option_lan), stringResource(R.string.multiplayer_option_lan_hint)) {
                                    netplayGoal = false
                                    open(Screen.LAN)
                                }
                            }
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
                                Text(stringResource(R.string.multiplayer_hotspot_network, hotspotName), style = MaterialTheme.typography.subtitle1)
                                Text(stringResource(R.string.multiplayer_hotspot_password, DirectLink.PASSPHRASE), style = MaterialTheme.typography.subtitle1)
                                StartContent(
                                    playerName = playerName,
                                    onPlayerNameChange = { playerName = it.take(10) },
                                    maxPlayers = maxPlayers,
                                    onMaxPlayersChange = { maxPlayers = it.coerceIn(2, if (netplayGoal) 2 else 16) },
                                    sameNetworkHint = false,
                                )
                            }
                            Screen.WIFI -> StartContent(
                                playerName = playerName,
                                onPlayerNameChange = { playerName = it.take(10) },
                                maxPlayers = maxPlayers,
                                onMaxPlayersChange = { maxPlayers = it.coerceIn(2, if (netplayGoal) 2 else 16) },
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
                    else -> LobbyContent(hosting = mode == MODE_HOSTING, players = players, network = DirectLink.hosted, netplayGoal = netplayGoal)
                }
                busyText?.let { Text(it, style = MaterialTheme.typography.caption) }
                errorText?.let { Text(it, color = MaterialTheme.colors.error, style = MaterialTheme.typography.caption) }
            }
        },
        buttons = {
            val idle = busyText == null
            when (mode) {
                MODE_NONE -> {
                    val parent = when (screen) {
                        Screen.MENU -> null
                        Screen.USB, Screen.LAN -> Screen.MENU
                        Screen.HOTSPOT, Screen.WIFI -> Screen.LAN
                        Screen.HOST_HOTSPOT -> Screen.HOTSPOT
                    }
                    parent?.let { DialogButton(stringResource(R.string.multiplayer_back), enabled = idle) { open(it) } }
                    when (screen) {
                        Screen.HOTSPOT -> DialogButton(stringResource(R.string.multiplayer_role_join), enabled = idle && manualSsid.isNotBlank()) {
                            joinHotspot(manualSsid, manualPassword)
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
                    // Netplay: exactly two players; each device restarts the game running both consoles.
                    // The game network (if any) stays up: Netplay runs over it.
                    val peer = netplayPeer
                    if (peer != null) {
                        DialogButton(stringResource(R.string.multiplayer_start_netplay), enabled = idle) {
                            val player = if (mode == MODE_HOSTING) 0 else 1
                            scope.launch(Dispatchers.IO) {
                                MelonEmulator.lanLeave()
                                withContext(Dispatchers.Main) { onStartNetplay(player, peer) }
                            }
                        }
                    }
                    DialogButton(stringResource(R.string.multiplayer_leave), enabled = idle) {
                        scope.launch(Dispatchers.IO) { MelonEmulator.lanLeave() }
                        DirectLink.leave(context)
                    }
                }
            }
            DialogButton(stringResource(R.string.multiplayer_resume), enabled = idle, onClick = onDismiss)
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
        TextButton(onClick = { onMaxPlayersChange(maxPlayers - 1) }) { Text("−") }
        Text(maxPlayers.toString())
        TextButton(onClick = { onMaxPlayersChange(maxPlayers + 1) }) { Text("+") }
    }
    if (sameNetworkHint) {
        Text(stringResource(R.string.multiplayer_same_version_hint), style = MaterialTheme.typography.caption)
    }
}

@Composable
private fun HotspotContent(
    networks: List<String>,
    canScan: Boolean,
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
        !canScan -> Text(stringResource(R.string.multiplayer_location_needed), style = MaterialTheme.typography.body2)
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
private fun LobbyContent(hosting: Boolean, players: List<LanPlayer>, network: HostedNetwork?, netplayGoal: Boolean) {
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
        Text(
            text = "${player.id + 1}/${player.maxPlayers}  ${player.name}  ·  $status  ·  $where",
            style = MaterialTheme.typography.body2,
        )
    }
    network?.let {
        Text(stringResource(R.string.multiplayer_hosting_network, it.ssid, it.passphrase), style = MaterialTheme.typography.body2)
        // a plain local-only hotspot: Android chose the name, so players cannot find it by scanning
        if (!it.wifiDirect && !it.ssid.contains(DirectLink.NAME_PREFIX)) {
            Text(stringResource(R.string.multiplayer_hotspot_fallback), style = MaterialTheme.typography.caption)
        }
    }
    if (netplayGoal) {
        Text(stringResource(R.string.multiplayer_netplay_connect_hint), style = MaterialTheme.typography.caption)
    }
    Text(stringResource(R.string.multiplayer_lobby_hint), style = MaterialTheme.typography.caption)
}
