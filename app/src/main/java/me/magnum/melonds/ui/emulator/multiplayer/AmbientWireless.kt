package me.magnum.melonds.ui.emulator.multiplayer

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Base64
import android.util.Log
import me.magnum.melonds.MelonEmulator
import me.magnum.melonds.domain.repositories.SettingsRepository
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Just Play local wireless (native side: melonDS-android-lib src/net/AmbientLAN). While a game runs in the foreground,
 * the native presence thread announces this device on the subnet; this class adds what only Android can: the
 * per-install identity, the Wi-Fi multicast lock (drivers drop broadcast without it) and NSD / mDNS (`_sereneds._udp`),
 * whose finds the native side also reaches by unicast (routers that drop broadcast).
 */
object AmbientWireless {
    private const val TAG = "AmbientWireless"
    private const val SERVICE_TYPE = "_sereneds._udp."

    private var nsd: NsdManager? = null
    private var lock: WifiManager.MulticastLock? = null
    private var registration: NsdManager.RegistrationListener? = null
    private var discovery: NsdManager.DiscoveryListener? = null
    private var ownName = ""
    private val resolveQueue = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    /**
     * Per-install identity, as gba-link's: one Ed25519 keypair generated once (a random key where the platform has no
     * Ed25519, before Android 13); the device id = the first 8 bytes of SHA-256 of the public key.
     */
    fun deviceId(context: Context): Long {
        val prefs = context.getSharedPreferences("ambient_identity", Context.MODE_PRIVATE)
        var pub = prefs.getString("public_key", null)
        if (pub == null) {
            val pair = try { KeyPairGenerator.getInstance("Ed25519").generateKeyPair() } catch (e: Exception) { null }
            val pubBytes = pair?.public?.encoded ?: ByteArray(32).also { SecureRandom().nextBytes(it) }
            pub = Base64.encodeToString(pubBytes, Base64.NO_WRAP)
            prefs.edit()
                .putString("public_key", pub)
                .putString("private_key", pair?.private?.encoded?.let { Base64.encodeToString(it, Base64.NO_WRAP) })
                .apply()
        }
        val hash = MessageDigest.getInstance("SHA-256").digest(Base64.decode(pub, Base64.NO_WRAP))
        return ByteBuffer.wrap(hash, 0, 8).order(ByteOrder.LITTLE_ENDIAN).long
    }

    /** Before a game starts: the native side installs Just Play with it (and gives the console its MAC). */
    fun configure(context: Context, settings: SettingsRepository) {
        MelonEmulator.ambientConfigure(deviceId(context), settings.getFirmwareConfiguration().nickname, settings.getLocalWirelessMode())
        MelonEmulator.ambientSetAutoSwitch(!settings.getLocalWirelessAskBeforeSwitch())
    }

    /** After the game started: NSD + multicast lock, if Just Play runs. */
    fun start(context: Context) {
        stop()
        val port = MelonEmulator.ambientPort()
        if (port <= 0) return
        val app = context.applicationContext
        lock = (app.getSystemService(Context.WIFI_SERVICE) as WifiManager).createMulticastLock("sereneds-ambient").apply {
            setReferenceCounted(false)
            acquire()
        }
        val manager = app.getSystemService(Context.NSD_SERVICE) as NsdManager
        nsd = manager
        ownName = "SereneDS-%016x".format(deviceId(app))
        val info = NsdServiceInfo().apply {
            serviceName = ownName
            serviceType = SERVICE_TYPE
            setPort(port)
        }
        registration = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) { ownName = info.serviceName }
            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) { Log.w(TAG, "NSD register failed: $error") }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {}
        }
        discovery = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(type: String) {}
            override fun onDiscoveryStopped(type: String) {}
            override fun onStartDiscoveryFailed(type: String, error: Int) { Log.w(TAG, "NSD discovery failed: $error") }
            override fun onStopDiscoveryFailed(type: String, error: Int) {}
            override fun onServiceLost(info: NsdServiceInfo) {}
            override fun onServiceFound(info: NsdServiceInfo) {
                if (info.serviceName != ownName) synchronized(resolveQueue) { resolveQueue.addLast(info) }
                resolveNext()
            }
        }
        try {
            manager.registerService(info, NsdManager.PROTOCOL_DNS_SD, registration)
            manager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery)
        } catch (e: Exception) {
            Log.w(TAG, "NSD unavailable", e)
        }
    }

    // NsdManager resolves one service at a time (FAILURE_ALREADY_ACTIVE otherwise)
    private fun resolveNext() {
        val manager = nsd ?: return
        val next = synchronized(resolveQueue) {
            if (resolving || resolveQueue.isEmpty()) return
            resolving = true
            resolveQueue.removeFirst()
        }
        @Suppress("DEPRECATION")
        manager.resolveService(next, object : NsdManager.ResolveListener {
            override fun onServiceResolved(info: NsdServiceInfo) {
                @Suppress("DEPRECATION")
                info.host?.hostAddress?.let { MelonEmulator.ambientAddPeer(it, info.port) }
                synchronized(resolveQueue) { resolving = false }
                resolveNext()
            }
            override fun onResolveFailed(info: NsdServiceInfo, error: Int) {
                synchronized(resolveQueue) { resolving = false }
                resolveNext()
            }
        })
    }

    fun stop() {
        val manager = nsd
        try { registration?.let { manager?.unregisterService(it) } } catch (e: Exception) {}
        try { discovery?.let { manager?.stopServiceDiscovery(it) } } catch (e: Exception) {}
        registration = null
        discovery = null
        nsd = null
        synchronized(resolveQueue) { resolveQueue.clear(); resolving = false }
        lock?.release()
        lock = null
    }

    /** The app is in the foreground (announcing) or not (peers expire this device in 6 s). */
    fun setForeground(foreground: Boolean) {
        MelonEmulator.ambientSetForeground(foreground)
    }
}
