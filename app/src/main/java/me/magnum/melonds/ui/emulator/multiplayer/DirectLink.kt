package me.magnum.melonds.ui.emulator.multiplayer

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.SoftApConfiguration
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.os.Looper
import androidx.annotation.RequiresApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.Executor
import kotlin.coroutines.resume

/** A game network this device created: what other players join, and the host's own address. */
data class HostedNetwork(val ssid: String, val passphrase: String, val hostAddress: String?, val wifiDirect: Boolean)

/**
 * Direct connection for LAN multiplayer: the host creates its own Wi-Fi network and the other
 * devices join it, so packets take one radio hop between the devices instead of going through a
 * router (in Shrek races this cuts the per-exchange round trip from ~5.5 ms to ~3.3 ms).
 *
 * Hosting (a normal app cannot start a classic hotspot with its own name):
 * 1. Wi-Fi Direct group named "DIRECT-XY" (Android 10+; the shortest name Android allows, so a
 *    joiner types two characters after the prefilled "DIRECT-"). The host stays on its
 *    normal Wi-Fi; the group shares that network's channel, so it can fail when that channel is
 *    one the radio cannot run a group on (e.g. a DFS channel).
 * 2. Fallback: local-only hotspot with the same name (hidden Android 13+ overload).
 * 3. Last resort: plain local-only hotspot; Android picks the name and password and the dialog
 *    shows them so players can type them in.
 * Joiners connect with a WifiNetworkSpecifier (one system approval prompt) and then talk to the
 * host's address directly: LAN session discovery (UDP broadcast) does not work across these
 * networks.
 *
 * "Only emulation packets": Wi-Fi Direct groups and local-only hotspots route no internet, and
 * on a joiner only this app's process is bound to the game network (bindProcessToNetwork); the
 * rest of the device keeps its normal network. No extra firewall.
 * ponytail: the whole app process is bound, not just the LAN sockets, so the app's own internet
 * use (e.g. RetroAchievements) is off on a joiner during the session; bind only the ENet socket
 * (android_setsocknetwork in JNI) if that matters.
 */
object DirectLink {
    const val NAME_PREFIX = "DIRECT-"
    // unambiguous when typed: no 0/O, 1/I/L
    private const val NAME_CHARS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
    private val OUR_NAME = Regex("DIRECT-[A-Z0-9]{2}")
    const val PASSPHRASE = "sereneDS"
    // Wi-Fi Direct group owners always use this address (Android convention); used only when the
    // network does not report its DHCP server / gateway
    private const val P2P_GROUP_OWNER_ADDRESS = "192.168.49.1"

    val isSupported get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q

    /**
     * Runtime permissions: Wi-Fi Direct and local-only hotspots need NEARBY_WIFI_DEVICES (Android
     * 13+, declared neverForLocation) or location (older). Listing nearby networks needs location
     * on every version: Android only gives Wi-Fi scan results to apps with location access.
     */
    val permissions: Array<String> get() = buildList {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.NEARBY_WIFI_DEVICES)
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        add(Manifest.permission.ACCESS_COARSE_LOCATION)
    }.toTypedArray()

    /** The network this device hosts, if any. Outlives the dialog, like the LAN session. */
    var hosted: HostedNetwork? = null
        private set

    private var p2p: WifiP2pManager? = null
    private var p2pChannel: WifiP2pManager.Channel? = null
    private var reservation: WifiManager.LocalOnlyHotspotReservation? = null
    private var clientCallback: ConnectivityManager.NetworkCallback? = null

    fun randomName() = NAME_PREFIX + (1..2).map { NAME_CHARS.random() }.joinToString("")

    /** A network this app could have created (other Wi-Fi Direct devices, e.g. printers, add a suffix). */
    fun isOurName(ssid: String) = OUR_NAME.matches(ssid)

    private fun wifi(context: Context) = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private fun connectivity(context: Context) = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager

    /** Creates the network this device hosts the session on (see the class comment). */
    suspend fun host(context: Context, name: String): HostedNetwork? {
        if (!isSupported) return null
        leave(context)
        val network = hostWifiDirect(context, name)
            ?: (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) hostHotspot(context, name) else null)
            ?: hostHotspot(context, null)
        hosted = network
        return network
    }

    @SuppressLint("MissingPermission")
    @RequiresApi(Build.VERSION_CODES.Q)
    private suspend fun hostWifiDirect(context: Context, name: String): HostedNetwork? {
        val manager = context.applicationContext.getSystemService(Context.WIFI_P2P_SERVICE) as? WifiP2pManager ?: return null
        val channel = manager.initialize(context.applicationContext, Looper.getMainLooper(), null) ?: return null
        p2p = manager
        p2pChannel = channel
        // a group left over from an earlier session would make createGroup fail
        p2pAction { manager.removeGroup(channel, it) }
        val ssid = name
        val config = WifiP2pConfig.Builder()
            .setNetworkName(ssid)
            .setPassphrase(PASSPHRASE)
            .enablePersistentMode(false)
            .build()
        if (!p2pAction { manager.createGroup(channel, config, it) }) {
            closeP2p()
            return null
        }
        // the group is formed shortly after createGroup succeeds
        val address = withTimeoutOrNull(10_000) {
            var groupOwner: String? = null
            while (groupOwner == null) {
                val info = suspendCancellableCoroutine { cont -> manager.requestConnectionInfo(channel) { cont.resume(it) } }
                if (info != null && info.groupFormed && info.isGroupOwner) groupOwner = info.groupOwnerAddress?.hostAddress
                if (groupOwner == null) delay(250)
            }
            groupOwner
        }
        if (address == null) {
            closeP2p()
            return null
        }
        return HostedNetwork(ssid, PASSPHRASE, address, wifiDirect = true)
    }

    private suspend fun p2pAction(call: (WifiP2pManager.ActionListener) -> Unit): Boolean = suspendCancellableCoroutine { cont ->
        call(object : WifiP2pManager.ActionListener {
            override fun onSuccess() { if (cont.isActive) cont.resume(true) }
            override fun onFailure(reason: Int) { if (cont.isActive) cont.resume(false) }
        })
    }

    private fun closeP2p() {
        val manager = p2p
        val channel = p2pChannel
        if (manager != null && channel != null) {
            manager.removeGroup(channel, null)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) channel.close()
        }
        p2p = null
        p2pChannel = null
    }

    /**
     * Local-only hotspot. With [name] (Android 13+) it uses that name and [PASSPHRASE] through a
     * hidden overload; without, Android picks both and they are read back from the reservation.
     */
    @SuppressLint("MissingPermission")
    @RequiresApi(Build.VERSION_CODES.Q)
    private suspend fun hostHotspot(context: Context, name: String?): HostedNetwork? = suspendCancellableCoroutine { cont ->
        val callback = object : WifiManager.LocalOnlyHotspotCallback() {
            override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                reservation = res
                val network = if (name != null) {
                    HostedNetwork(name, PASSPHRASE, null, wifiDirect = false)
                } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val config = res.softApConfiguration
                    @Suppress("DEPRECATION")
                    HostedNetwork(config.ssid.orEmpty().removeSurrounding("\""), config.passphrase.orEmpty(), null, wifiDirect = false)
                } else {
                    @Suppress("DEPRECATION")
                    val config = res.wifiConfiguration
                    HostedNetwork(config?.SSID.orEmpty().removeSurrounding("\""), config?.preSharedKey.orEmpty().removeSurrounding("\""), null, wifiDirect = false)
                }
                if (cont.isActive) cont.resume(network) else res.close()
            }
            override fun onFailed(reason: Int) { if (cont.isActive) cont.resume(null) }
        }
        val wifi = wifi(context)
        runCatching {
            if (name != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                WifiManager::class.java.getMethod(
                    "startLocalOnlyHotspot", SoftApConfiguration::class.java, Executor::class.java,
                    WifiManager.LocalOnlyHotspotCallback::class.java,
                ).invoke(wifi, hotspotConfig(name), context.mainExecutor, callback)
            } else {
                wifi.startLocalOnlyHotspot(callback, null)
            }
        }.onFailure { if (cont.isActive) cont.resume(null) }
    }

    @SuppressLint("PrivateApi")
    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    private fun hotspotConfig(name: String): SoftApConfiguration {
        val builderClass = Class.forName("android.net.wifi.SoftApConfiguration\$Builder")
        val builder = builderClass.getDeclaredConstructor().newInstance()
        builderClass.getMethod("setSsid", String::class.java).invoke(builder, name)
        builderClass.getMethod("setPassphrase", String::class.java, Int::class.javaPrimitiveType)
            .invoke(builder, PASSPHRASE, SoftApConfiguration.SECURITY_TYPE_WPA2_PSK)
        // either band: the driver's regulatory domain does not always allow a 5 GHz hotspot
        builderClass.getMethod("setBand", Int::class.javaPrimitiveType)
            .invoke(builder, SoftApConfiguration.BAND_2GHZ or SoftApConfiguration.BAND_5GHZ)
        return builderClass.getMethod("build").invoke(builder) as SoftApConfiguration
    }

    /** Asks Wi-Fi for a fresh scan (Android throttles these; results come later). */
    fun requestScan(context: Context) {
        @Suppress("DEPRECATION")
        runCatching { wifi(context).startScan() }
    }

    /** SereneDS networks in the latest scan results, strongest first. Empty without location access. */
    @SuppressLint("MissingPermission")
    fun nearbyNetworks(context: Context): List<String> = runCatching {
        @Suppress("DEPRECATION")
        wifi(context).scanResults
            .filter { isOurName(it.SSID) }
            .sortedByDescending { it.level }
            .map { it.SSID }
            .distinct()
    }.getOrDefault(emptyList())

    /**
     * Joins [ssid] (Android asks the user to allow it the first time) and binds this app's process
     * to it. Returns the host's address (the network's DHCP server / gateway), or null.
     * ponytail: WPA2 only; a pure-WPA3 fallback hotspot would need setWpa3Passphrase.
     */
    suspend fun join(context: Context, ssid: String, passphrase: String): String? {
        if (!isSupported) return null
        leave(context)
        val cm = connectivity(context)
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(WifiNetworkSpecifier.Builder().setSsid(ssid).setWpa2Passphrase(passphrase).build())
            .build()
        return suspendCancellableCoroutine { cont ->
            val callback = object : ConnectivityManager.NetworkCallback() {
                override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {
                    if (!cont.isActive) return
                    val host = hostAddressOf(linkProperties) ?: if (ssid.startsWith(NAME_PREFIX)) P2P_GROUP_OWNER_ADDRESS else return
                    cm.bindProcessToNetwork(network)
                    cont.resume(host)
                }
                override fun onUnavailable() { if (cont.isActive) cont.resume(null) }
            }
            clientCallback = callback
            // Wi-Fi only finds the network on its next scan (they can be ~20 s apart); ask for one now.
            requestScan(context)
            cm.requestNetwork(request, callback, 60_000)
        }
    }

    // the host runs the network's DHCP server and is its gateway
    private fun hostAddressOf(lp: LinkProperties): String? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) lp.dhcpServerAddress?.hostAddress?.let { return it }
        return lp.routes.firstOrNull { it.isDefaultRoute && it.gateway != null }?.gateway?.hostAddress
    }

    /** Ends hosting or joining: removes the group / stops the hotspot and unbinds the network. */
    fun leave(context: Context) {
        closeP2p()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) reservation?.close()
        reservation = null
        hosted = null
        clientCallback?.let {
            val cm = connectivity(context)
            cm.bindProcessToNetwork(null)
            runCatching { cm.unregisterNetworkCallback(it) }
        }
        clientCallback = null
    }
}
