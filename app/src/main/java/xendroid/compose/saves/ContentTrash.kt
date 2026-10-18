package xendroid.compose.saves

import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import xendroid.compose.archive.ArchiveFiles
import xendroid.compose.archive.ContentLease

/** One DLC or title update package in the trash. */
@Serializable
data class TrashedContent(
    val id: String,
    val titleId: String,
    val contentType: Int,
    val pkgDir: String,
    val displayName: String,
    val bytes: Long,
    val deletedAt: Long,
)

/** The trash would go over its quota: the caller offers emptying it or deleting for good. */
class TrashFullException(val usedBytes: Long, val quotaBytes: Long, val itemBytes: Long) :
    IllegalStateException("The trash is full")

/** Why a trashed package cannot go back now: [why] for the screen to say in its language (U02),
 *  the message in English for logs and tests; [name] is the package's. */
class RestoreRefusedException(val why: Why, val name: String = "") : IllegalStateException(why.english.format(name)) {
    enum class Why(val english: String) {
        GONE("This item is no longer in the trash"),
        INSTALLED_AGAIN("\"%s\" is installed again; remove that one first or delete this one for good"),
        HEADER_IN_USE("Another package uses the same header; remove it first"),
    }
}

/**
 * L12: removing an installed DLC or title update moves it into a trash beside the content
 * tree (content/.xendroid-trash/content, never a name the emulator lists), where it can be
 * restored or deleted for good. Only these managed packages are eligible; a game file in a
 * library folder is never touched by the app. Every mutation takes the storage lease (no
 * game or other content job running).
 *
 * A package is two things the core keeps apart: its data folder
 * (<root>/0000000000000000/<TID>/<TYPE>/<pkg>; a package a title copied as one file is that
 * file, which the core mounts as it is) and its header
 * (<root>/0000000000000000/<TID>/Headers/<TYPE>/<pkg>.header). Moving both is journaled by
 * the entry's own folder name: "<id>.partial" while trashing (rolled back by [recover]),
 * "<id>" once trashed, "<id>.restoring" while restoring (finished by [recover]). Each step
 * is an atomic rename on one filesystem, so a process killed at any point loses nothing.
 */
class ContentTrash(
    private val contentRoot: File,
    private val clock: () -> Long = System::currentTimeMillis,
    val quotaBytes: Long = DEFAULT_QUOTA,
) {
    private val trashRoot = File(contentRoot, ".xendroid-trash/content")
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    private val titlePattern = Regex("[0-9A-F]{8}")
    private val idPattern = Regex("[0-9A-F]{8}-[0-9A-F]{8}-[0-9]{13}")

    /** Data folder and header of a package, after checking that it is an eligible one. */
    private fun locate(titleId: String, contentType: Int, pkgDir: String): Pair<File, File> {
        val title = titleId.uppercase()
        require(titlePattern.matches(title) && title != "00000000") { "Invalid Title ID" }
        require(contentType in ELIGIBLE) { "Only DLC and title updates go to the trash" }
        require(pkgDir.isNotEmpty() && pkgDir.length <= 255 && pkgDir != "." && pkgDir != ".." &&
            !pkgDir.contains('/') && !pkgDir.contains('\\') && !pkgDir.contains('\u0000')) { "Invalid package folder" }
        val type = "%08X".format(contentType)
        val data = ArchiveFiles.resolve(contentRoot, "$MACHINE/$title/$type/$pkgDir")
        val header = ArchiveFiles.resolve(contentRoot, "$MACHINE/$title/Headers/$type/$pkgDir.header")
        return data to header
    }

    private fun entryDir(id: String, suffix: String = ""): File {
        require(idPattern.matches(id)) { "Invalid trash entry" }
        return File(trashRoot, id + suffix)
    }

    /** Moves one package to the trash. Throws [TrashFullException] past the quota. */
    fun moveToTrash(@Suppress("UNUSED_PARAMETER") lease: ContentLease, titleId: String, contentType: Int,
                    pkgDir: String, displayName: String): TrashedContent {
        val (data, header) = locate(titleId, contentType, pkgDir)
        require(Files.isDirectory(data.toPath(), LinkOption.NOFOLLOW_LINKS) ||
            Files.isRegularFile(data.toPath(), LinkOption.NOFOLLOW_LINKS)) { "Package not found" }
        val bytes = sizeOf(data) + (if (header.isFile) header.length() else 0L)
        val used = usedBytes()
        if (used + bytes > quotaBytes) throw TrashFullException(used, quotaBytes, bytes)
        check(trashRoot.isDirectory || trashRoot.mkdirs()) { "Cannot create the trash" }
        var time = clock()
        fun idAt(t: Long) = "${titleId.uppercase()}-${"%08X".format(contentType)}-${"%013d".format(t)}"
        while (entryDir(idAt(time)).exists() || entryDir(idAt(time), PARTIAL).exists() ||
            entryDir(idAt(time), RESTORING).exists()) time++
        val entry = TrashedContent(idAt(time), titleId.uppercase(), contentType, pkgDir, displayName.take(200), bytes, time)
        val staging = entryDir(entry.id, PARTIAL)
        check(staging.mkdir()) { "Cannot create the trash entry" }
        try {
            ArchiveFiles.atomicText(File(staging, MANIFEST), json.encodeToString(TrashedContent.serializer(), entry))
            move(data, File(staging, DATA))
            if (header.isFile) move(header, File(staging, HEADER))
            move(staging, entryDir(entry.id))
        } catch (e: Exception) {
            // Same as recovery after a kill: the package goes back, nothing is lost.
            runCatching { settlePartial(staging) }.onFailure(e::addSuppressed)
            throw e
        }
        return entry
    }

    /** Trashed packages, newest first; a damaged entry is skipped (and kept). */
    fun list(): List<TrashedContent> = trashRoot.listFiles().orEmpty()
        .filter { it.isDirectory && idPattern.matches(it.name) }
        .mapNotNull { readManifest(it)?.takeIf { entry -> entry.id == it.name } }
        .sortedByDescending { it.deletedAt }

    fun usedBytes(): Long = list().sumOf { it.bytes }

    /** Why [id] cannot be restored now, or null when it can. Nothing is changed. */
    fun restoreConflict(id: String): RestoreRefusedException? {
        val entry = readManifest(entryDir(id)) ?: return RestoreRefusedException(RestoreRefusedException.Why.GONE)
        val (data, header) = locate(entry.titleId, entry.contentType, entry.pkgDir)
        return when {
            Files.exists(data.toPath(), LinkOption.NOFOLLOW_LINKS) ->
                RestoreRefusedException(RestoreRefusedException.Why.INSTALLED_AGAIN, entry.displayName)
            Files.exists(header.toPath(), LinkOption.NOFOLLOW_LINKS) && File(entryDir(id), HEADER).exists() ->
                RestoreRefusedException(RestoreRefusedException.Why.HEADER_IN_USE, entry.displayName)
            else -> null
        }
    }

    /** Puts [id] back where it was installed; never merges into or replaces an installed one. */
    fun restore(@Suppress("UNUSED_PARAMETER") lease: ContentLease, id: String): TrashedContent {
        val source = entryDir(id)
        val entry = readManifest(source) ?: throw RestoreRefusedException(RestoreRefusedException.Why.GONE)
        restoreConflict(id)?.let { throw it }
        val restoring = entryDir(id, RESTORING)
        move(source, restoring)
        finishRestore(restoring, entry)
        return entry
    }

    /** Deletes [id] for good. */
    fun purge(@Suppress("UNUSED_PARAMETER") lease: ContentLease, id: String) = ArchiveFiles.deleteTree(entryDir(id))

    fun purgeAll(lease: ContentLease): Int = list().onEach { purge(lease, it.id) }.size

    /**
     * Finishes what a killed process left: a half-made trash entry is rolled back (the
     * package goes back where it was, so it simply looks not deleted), a half-made restore
     * is completed. Returns how many entries it settled.
     */
    fun recover(@Suppress("UNUSED_PARAMETER") lease: ContentLease): Int {
        var settled = 0
        for (dir in trashRoot.listFiles().orEmpty()) {
            val name = dir.name
            when {
                name.endsWith(PARTIAL) && idPattern.matches(name.removeSuffix(PARTIAL)) -> {
                    settlePartial(dir)
                    settled++
                }
                name.endsWith(RESTORING) && idPattern.matches(name.removeSuffix(RESTORING)) -> {
                    val entry = readManifest(dir)
                    if (entry != null) finishRestore(dir, entry)
                    settled++
                }
            }
        }
        return settled
    }

    /** Rolls back a half-made trash entry: the package goes back where it was. */
    private fun settlePartial(dir: File) {
        val entry = readManifest(dir)
        if (entry == null) {
            // Killed before the manifest: nothing was moved yet.
            if (dir.list().orEmpty().none { it == DATA || it == HEADER }) ArchiveFiles.deleteTree(dir)
            return
        }
        val (data, header) = locate(entry.titleId, entry.contentType, entry.pkgDir)
        val back = moveBackIfFree(File(dir, DATA), data) && moveBackIfFree(File(dir, HEADER), header)
        // Something took the original place: keep it as a normal trash entry instead.
        if (back) ArchiveFiles.deleteTree(dir) else move(dir, entryDir(entry.id))
    }

    private fun finishRestore(dir: File, entry: TrashedContent) {
        val (data, header) = locate(entry.titleId, entry.contentType, entry.pkgDir)
        val back = moveBackIfFree(File(dir, HEADER), header) && moveBackIfFree(File(dir, DATA), data)
        if (back) ArchiveFiles.deleteTree(dir)
        else move(dir, entryDir(entry.id))   // the place got taken meanwhile: stays in the trash
    }

    /** Moves [from] to [to] unless [to] is taken; true when [from] is gone (moved or absent). */
    private fun moveBackIfFree(from: File, to: File): Boolean {
        if (!Files.exists(from.toPath(), LinkOption.NOFOLLOW_LINKS)) return true
        if (Files.exists(to.toPath(), LinkOption.NOFOLLOW_LINKS)) return false
        check(to.parentFile!!.isDirectory || to.parentFile!!.mkdirs()) { "Cannot create ${to.parentFile!!.name}" }
        move(from, to)
        return true
    }

    private fun move(from: File, to: File) {
        Files.move(from.toPath(), to.toPath(), StandardCopyOption.ATOMIC_MOVE)
    }

    private fun readManifest(dir: File): TrashedContent? = runCatching {
        val file = File(dir, MANIFEST)
        if (!file.isFile || file.length() > 16 * 1024) null
        else json.decodeFromString(TrashedContent.serializer(), file.readText())
    }.getOrNull()

    private fun sizeOf(dir: File): Long = Files.walk(dir.toPath()).use { paths ->
        paths.filter { Files.isRegularFile(it, LinkOption.NOFOLLOW_LINKS) }.mapToLong { Files.size(it) }.sum()
    }

    companion object {
        /** Installed DLC and title updates can be large; past this the user empties the trash. */
        const val DEFAULT_QUOTA = 4L * 1024 * 1024 * 1024
        private const val MACHINE = "0000000000000000"
        private val ELIGIBLE = setOf(0x00000002, 0x000B0000)
        private const val PARTIAL = ".partial"
        private const val RESTORING = ".restoring"
        private const val MANIFEST = "manifest.json"
        private const val DATA = "data"
        private const val HEADER = "header"
    }
}
