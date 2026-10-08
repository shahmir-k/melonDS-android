package me.magnum.melonds.impl

import android.content.Context
import java.io.File

/**
 * Games received from other players in Netplay: `<sha256>.nds` (`.part` while one arrives, kept to
 * resume). Native session setup fills it and keeps it under its size cap, least recently used first.
 * Not part of the ROM list.
 */
object NetplayRomCache {
    fun dir(context: Context) = File(context.externalCacheDir ?: context.cacheDir, "netplay_roms")
    fun size(context: Context) = dir(context).listFiles()?.sumOf { it.length() } ?: 0L
    fun clear(context: Context) = dir(context).listFiles()?.all { it.delete() } ?: true
}
