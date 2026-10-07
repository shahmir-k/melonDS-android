package me.magnum.melonds.ui.emulator.multiplayer

import android.content.Context
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
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
import androidx.compose.material.Checkbox
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

/**
 * LAN multiplayer lobby, opened from the pause menu. The game stays paused while it is open, so
 * it pumps the native LAN backend itself; once connected, players use their game's own wireless
 * features (e.g. Union Room) and the emulator routes the DS wireless traffic over the network.
 * The session outlives the dialog: it ends on "Leave session" or when the emulator stops.
 */
@Composable
fun LanMultiplayerDialog(defaultPlayerName: String, onStartNetplay: (player: Int, peer: String) -> Unit, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var playerName by remember { mutableStateOf(defaultPlayerName) }
    var maxPlayers by remember { mutableIntStateOf(2) }
    var hostAddress by remember { mutableStateOf("") }
    var mode by remember { mutableIntStateOf(MODE_NONE) }
    var sessions by remember { mutableStateOf(emptyList<LanSession>()) }
    var players by remember { mutableStateOf(emptyList<LanPlayer>()) }
    var busyText by remember { mutableStateOf<String?>(null) }
    var errorText by remember { mutableStateOf<String?>(null) }
    var direct by remember { mutableStateOf(false) }
    // Netplay peer, remembered while both players are connected: once one side restarts into
    // Netplay it leaves the lobby, and the other side's list then no longer has its address
    var netplayPeer by remember { mutableStateOf<String?>(null) }
    // a Wi-Fi Direct action waiting on the runtime permission prompt
    var afterPermission by remember { mutableStateOf<(() -> Unit)?>(null) }

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
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val next = afterPermission
        afterPermission = null
        if (granted) next?.invoke() else errorText = permissionDenied
    }

    // Runs `action` once Wi-Fi Direct may be used (asking for the permission first if needed).
    fun withDirectPermission(action: () -> Unit) {
        val permission = DirectLink.requiredPermission
        if (ContextCompat.checkSelfPermission(context, permission) == PackageManager.PERMISSION_GRANTED) {
            action()
        } else {
            afterPermission = action
            permissionLauncher.launch(permission)
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

    fun join(address: String) = runAction(connecting, connectionFailed) {
        MelonEmulator.lanJoin(playerName.ifBlank { defaultPlayerName }, address.trim()).also { ok ->
            if (!ok) MelonEmulator.lanStartDiscovery()
        }
    }

    BaseDialog(
        title = stringResource(R.string.multiplayer_lan_title),
        onDismiss = onDismiss,
        content = { padding ->
            Column(Modifier.padding(padding), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                when (mode) {
                    MODE_NONE -> StartContent(
                        playerName = playerName,
                        onPlayerNameChange = { playerName = it.take(10) },
                        maxPlayers = maxPlayers,
                        onMaxPlayersChange = { maxPlayers = it.coerceIn(2, 16) },
                        direct = direct,
                        onDirectChange = { direct = it },
                    )
                    MODE_DISCOVERING -> DiscoveryContent(
                        sessions = sessions,
                        hostAddress = hostAddress,
                        onHostAddressChange = { hostAddress = it },
                        onSessionSelected = { join(it.address) },
                    )
                    else -> LobbyContent(hosting = mode == MODE_HOSTING, players = players)
                }
                busyText?.let { Text(it, style = MaterialTheme.typography.caption) }
                errorText?.let { Text(it, color = MaterialTheme.colors.error, style = MaterialTheme.typography.caption) }
            }
        },
        buttons = {
            val idle = busyText == null
            when (mode) {
                MODE_NONE -> {
                    val name = playerName.ifBlank { defaultPlayerName }
                    DialogButton(stringResource(R.string.multiplayer_role_host), enabled = idle) {
                        if (direct) withDirectPermission {
                            runAction(directStarting, directFailed) {
                                DirectLink.host(context) && MelonEmulator.lanHost(name, maxPlayers)
                            }
                        } else runAction(starting, lanInitFailed) { MelonEmulator.lanHost(name, maxPlayers) }
                    }
                    DialogButton(stringResource(R.string.multiplayer_role_join), enabled = idle) {
                        if (direct) withDirectPermission {
                            runAction(directJoining, directFailed) {
                                val host = DirectLink.join(context)
                                host != null && MelonEmulator.lanJoin(name, host)
                            }
                        } else runAction(discovering, discoveryFailed) { MelonEmulator.lanStartDiscovery() }
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
                    // Netplay: exactly two players; each device restarts the game running both consoles
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
private fun StartContent(
    playerName: String,
    onPlayerNameChange: (String) -> Unit,
    maxPlayers: Int,
    onMaxPlayersChange: (Int) -> Unit,
    direct: Boolean,
    onDirectChange: (Boolean) -> Unit,
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
    if (DirectLink.isSupported) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.clickable { onDirectChange(!direct) }) {
            Checkbox(checked = direct, onCheckedChange = onDirectChange)
            Column {
                Text(stringResource(R.string.multiplayer_direct), style = MaterialTheme.typography.body2)
                Text(stringResource(R.string.multiplayer_direct_hint), style = MaterialTheme.typography.caption)
            }
        }
    }
    Text(stringResource(R.string.multiplayer_same_version_hint), style = MaterialTheme.typography.caption)
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
private fun LobbyContent(hosting: Boolean, players: List<LanPlayer>) {
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
    Text(stringResource(R.string.multiplayer_lobby_hint), style = MaterialTheme.typography.caption)
}
