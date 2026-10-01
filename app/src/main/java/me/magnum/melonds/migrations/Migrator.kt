package me.magnum.melonds.migrations

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit
import androidx.core.content.pm.PackageInfoCompat
import me.magnum.melonds.utils.PackageManagerCompat

class Migrator(private val context: Context, private val sharedPreferences: SharedPreferences) {
    private val migrations = mutableListOf<Migration>()

    fun registerMigration(migration: Migration) {
        if (migrations.find { it.from == migration.from } != null) {
            throw Exception("Migration from version ${migration.from} already exists")
        }

        migrations.add(migration)
    }

    fun performMigrations() {
        migrateForcedSoftwareRenderer()
        if (!mustPerformMigrations())
            return

        getMigrationsToPerform().forEach {
            it.migrate()
        }
        sharedPreferences.edit {
            putLong("last_version", getCurrentVersion())
        }
    }

    // One-time, not version-gated: the native code used to force the software renderer
    // whatever the "video_renderer" setting said. Now that the setting is honoured, move
    // existing installs to software so nobody silently drops to a slower renderer.
    private fun migrateForcedSoftwareRenderer() {
        if (sharedPreferences.getBoolean("renderer_override_migrated", false))
            return

        sharedPreferences.edit {
            if (sharedPreferences.contains("video_renderer"))
                putString("video_renderer", "software")
            putBoolean("renderer_override_migrated", true)
        }
    }

    private fun mustPerformMigrations(): Boolean {
        return getLastVersion() < getCurrentVersion()
    }

    private fun getMigrationsToPerform(): List<Migration> {
        val fromVersion = getLastVersion()
        val toVersion = getCurrentVersion()

        return migrations
                .sortedBy { it.from }
                .filter { it.from >= fromVersion && it.to <= toVersion }
    }

    private fun getLastVersion(): Long {
        // 6 is the version at which migrations started being supported
        return sharedPreferences.getLong("last_version", 6)
    }

    private fun getCurrentVersion(): Long {
        val packageInfo = PackageManagerCompat.getPackageInfo(context.packageManager, context.packageName, 0)
        return PackageInfoCompat.getLongVersionCode(packageInfo)
    }
}