package me.magnum.melonds.ui.emulator.multiplayer

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.SoftApConfiguration
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import androidx.annotation.RequiresApi
import kotlinx.coroutines.suspendCancellableCoroutine
import java.util.concurrent.Executor
import kotlin.coroutines.resume

/**
 * Direct connection for LAN multiplayer: the host runs a local-only hotspot with a fixed name and
 * passphrase and the other devices join it, so packets take one radio hop between the devices
 * instead of going through a router (in Shrek races this cuts the per-exchange round trip from
 * ~5.5 ms to ~3.3 ms). A hotspot rather than Wi-Fi Direct because the RG DS radio can only use
 * one channel at a time, and Wi-Fi Direct must then share the router's channel, which fails on
 * DFS channels.
 *
 * Android only takes a custom hotspot config through a hidden overload of startLocalOnlyHotspot
 * (Android 13+, NEARBY_WIFI_DEVICES); the public one picks a random name and passphrase.
 *
 * ponytail: one fixed hotspot name, so two direct sessions within radio range collide; add a room
 * code to the name when that matters.
 */
object DirectLink {
    private const val SSID = "SereneDS-Direct"
    private const val PASSPHRASE = "serenedsdirect"

    val isSupported get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
    const val requiredPermission = Manifest.permission.NEARBY_WIFI_DEVICES

    private var reservation: WifiManager.LocalOnlyHotspotReservation? = null
    private var clientCallback: ConnectivityManager.NetworkCallback? = null

    @SuppressLint("PrivateApi")
    private fun hotspotConfig(): SoftApConfiguration {
        val builderClass = Class.forName("android.net.wifi.SoftApConfiguration\$Builder")
        val builder = builderClass.getDeclaredConstructor().newInstance()
        builderClass.getMethod("setSsid", String::class.java).invoke(builder, SSID)
        builderClass.getMethod("setPassphrase", String::class.java, Int::class.javaPrimitiveType)
            .invoke(builder, PASSPHRASE, SoftApConfiguration.SECURITY_TYPE_WPA2_PSK)
        // either band: the driver's regulatory domain does not always allow a 5 GHz hotspot
        builderClass.getMethod("setBand", Int::class.javaPrimitiveType)
            .invoke(builder, SoftApConfiguration.BAND_2GHZ or SoftApConfiguration.BAND_5GHZ)
        return builderClass.getMethod("build").invoke(builder) as SoftApConfiguration
    }

    /** Starts the hotspot this device hosts the session on. */
    @SuppressLint("MissingPermission")
    suspend fun host(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return false
        leave(context)
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        return suspendCancellableCoroutine { cont ->
            val callback = object : WifiManager.LocalOnlyHotspotCallback() {
                override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                    reservation = res
                    if (cont.isActive) cont.resume(true)
                }
                override fun onFailed(reason: Int) { if (cont.isActive) cont.resume(false) }
            }
            runCatching {
                WifiManager::class.java.getMethod(
                    "startLocalOnlyHotspot", SoftApConfiguration::class.java, Executor::class.java,
                    WifiManager.LocalOnlyHotspotCallback::class.java,
                ).invoke(wifi, hotspotConfig(), context.mainExecutor, callback)
            }.onFailure { if (cont.isActive) cont.resume(false) }
        }
    }

    /**
     * Joins the host's hotspot (Android asks the user to allow it the first time) and routes this
     * app's traffic through it. Returns the host's address, or null.
     */
    suspend fun join(context: Context): String? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return null
        leave(context)
        return joinHotspot(context)
    }

    @RequiresApi(Build.VERSION_CODES.TIRAMISU)
    private suspend fun joinHotspot(context: Context): String? {
        val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(WifiNetworkSpecifier.Builder().setSsid(SSID).setWpa2Passphrase(PASSPHRASE).build())
            .build()
        return suspendCancellableCoroutine { cont ->
            val callback = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    cm.bindProcessToNetwork(network)
                    // the hotspot owner (session host) runs the network's DHCP server
                    val host = cm.getLinkProperties(network)?.dhcpServerAddress?.hostAddress
                    if (cont.isActive) cont.resume(host)
                }
                override fun onUnavailable() { if (cont.isActive) cont.resume(null) }
            }
            clientCallback = callback
            // Wi-Fi only finds the hotspot on its next scan (they can be ~20 s apart); ask for one now.
            @Suppress("DEPRECATION")
            (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager).startScan()
            cm.requestNetwork(request, callback, 60_000)
        }
    }

    fun leave(context: Context) {
        reservation?.close()
        reservation = null
        clientCallback?.let {
            val cm = context.applicationContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            cm.bindProcessToNetwork(null)
            runCatching { cm.unregisterNetworkCallback(it) }
        }
        clientCallback = null
    }
}
