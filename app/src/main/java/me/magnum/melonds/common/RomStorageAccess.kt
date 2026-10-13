package me.magnum.melonds.common

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.FileNotFoundException

/** Access checks for ROMs opened through Android's external-storage document provider. */
object RomStorageAccess {
    private const val EXTERNAL_STORAGE_AUTHORITY = "com.android.externalstorage.documents"

    enum class ReadStatus { READABLE, ACCESS_DENIED, NOT_FOUND }

    /** Probe the actual provider, so a denied document is not reported as a missing ROM. */
    fun probe(context: Context, romUri: Uri): ReadStatus {
        if (romUri.authority != EXTERNAL_STORAGE_AUTHORITY || !DocumentsContract.isTreeUri(romUri)) {
            return ReadStatus.READABLE
        }
        return try {
            context.contentResolver.openFileDescriptor(romUri, "r")?.use { ReadStatus.READABLE }
                ?: ReadStatus.NOT_FOUND
        } catch (_: SecurityException) {
            ReadStatus.ACCESS_DENIED
        } catch (_: FileNotFoundException) {
            ReadStatus.NOT_FOUND
        }
    }

    fun needsTreeGrant(context: Context, romUri: Uri): Boolean = accessibleRomUri(context, romUri) == null

    /** Require the original tree, also used for saves beside the ROM. An ancestor grant is insufficient. */
    fun accessibleRomUri(context: Context, romUri: Uri): Uri? {
        if (romUri.authority != EXTERNAL_STORAGE_AUTHORITY || !DocumentsContract.isTreeUri(romUri)) {
            return romUri
        }
        val documentId = runCatching { DocumentsContract.getDocumentId(romUri) }.getOrNull() ?: return romUri
        val originalTreeId = runCatching { DocumentsContract.getTreeDocumentId(romUri) }.getOrNull() ?: return null
        val hasGrant = context.contentResolver.persistedUriPermissions.any { grant ->
            grant.isReadPermission && grant.isWritePermission && grant.uri.authority == romUri.authority &&
                grantMatchesRomTree(
                    runCatching { DocumentsContract.getTreeDocumentId(grant.uri) }.getOrNull(),
                    originalTreeId,
                    documentId
                )
        }
        return if (hasGrant) romUri else null
    }

    fun treeCoversRom(treeUri: Uri, romUri: Uri): Boolean {
        if (treeUri.authority != romUri.authority || treeUri.authority != EXTERNAL_STORAGE_AUTHORITY) return false
        val treeId = runCatching { DocumentsContract.getTreeDocumentId(treeUri) }.getOrNull()
        val documentId = runCatching { DocumentsContract.getDocumentId(romUri) }.getOrNull()
        val originalTreeId = runCatching { DocumentsContract.getTreeDocumentId(romUri) }.getOrNull()
        return grantMatchesRomTree(treeId, originalTreeId, documentId)
    }

    fun suggestedDirectory(romUri: Uri): Uri? {
        if (romUri.authority != EXTERNAL_STORAGE_AUTHORITY) return null
        if (DocumentsContract.isTreeUri(romUri)) {
            val treeId = runCatching { DocumentsContract.getTreeDocumentId(romUri) }.getOrNull()
            if (treeId != null) return DocumentsContract.buildTreeDocumentUri(romUri.authority!!, treeId)
        }
        val documentId = runCatching { DocumentsContract.getDocumentId(romUri) }.getOrNull() ?: return null
        val parentId = documentId.substringBeforeLast('/', missingDelimiterValue = "")
        if (parentId.isEmpty()) return null
        return DocumentsContract.buildTreeDocumentUri(romUri.authority!!, parentId)
    }

    internal fun treeCoversDocument(treeId: String?, documentId: String?): Boolean =
        treeId != null && documentId != null &&
            (documentId == treeId || documentId.startsWith("$treeId/"))

    internal fun grantMatchesRomTree(grantTreeId: String?, originalTreeId: String?, documentId: String?): Boolean =
        grantTreeId != null && grantTreeId == originalTreeId && treeCoversDocument(grantTreeId, documentId)
}
