package xendroid.compose.saves

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import xendroid.compose.archive.ContentLease

class ContentTrashTest {
    @get:Rule val temp = TemporaryFolder()
    private var now = 1_700_000_000_000L
    private val title = "4D5307E6"
    private val dlc = 0x00000002
    private val tu = 0x000B0000

    private val root by lazy { temp.newFolder("content") }
    private val leaseDir by lazy { temp.newFolder("lease") }
    private fun trash(quota: Long = ContentTrash.DEFAULT_QUOTA) = ContentTrash(root, { now }, quota)
    private fun <T> leased(block: (ContentLease) -> T): T = ContentLease.acquire(leaseDir).use(block)

    private fun data(type: Int, pkg: String) = File(root, "0000000000000000/$title/%08X/$pkg".format(type))
    private fun header(type: Int, pkg: String) = File(root, "0000000000000000/$title/Headers/%08X/$pkg.header".format(type))

    /** An installed package: a data folder with two files and its header. */
    private fun install(type: Int, pkg: String, bytes: Int = 100): File {
        val dir = data(type, pkg).apply { mkdirs() }
        File(dir, "content.bin").writeBytes(ByteArray(bytes) { 7 })
        File(dir, "sub").mkdirs(); File(dir, "sub/more.bin").writeBytes(ByteArray(10))
        header(type, pkg).apply { parentFile!!.mkdirs(); writeBytes(ByteArray(20) { 1 }) }
        return dir
    }

    @Test fun removingIsAMoveThatRestoresExactly() {
        install(dlc, "MapPack1")
        val entry = leased { trash().moveToTrash(it, title, dlc, "MapPack1", "Map Pack 1") }
        assertFalse(data(dlc, "MapPack1").exists())
        assertFalse(header(dlc, "MapPack1").exists())
        assertEquals(130L, entry.bytes)
        assertEquals(listOf(entry), trash().list())
        assertEquals(130L, trash().usedBytes())
        // The emulator's listing (data folders under the type) no longer sees it.
        assertEquals(emptyList<String>(), data(dlc, "x").parentFile!!.list()!!.toList())

        assertNull(trash().restoreConflict(entry.id))
        leased { trash().restore(it, entry.id) }
        assertEquals(100, File(data(dlc, "MapPack1"), "content.bin").length().toInt())
        assertEquals(10, File(data(dlc, "MapPack1"), "sub/more.bin").length().toInt())
        assertEquals(20, header(dlc, "MapPack1").length().toInt())
        assertEquals(emptyList<TrashedContent>(), trash().list())
    }

    @Test fun aPackageKeptAsOneFileGoesToTheTrashAndBack() {
        // A title can install a package by copying the package file itself; it has no header.
        val file = data(dlc, "RawPack").apply { parentFile!!.mkdirs(); writeBytes(ByteArray(300) { 3 }) }
        val entry = leased { trash().moveToTrash(it, title, dlc, "RawPack", "Raw Pack") }
        assertFalse(file.exists())
        assertEquals(300L, entry.bytes)
        leased { trash().restore(it, entry.id) }
        assertTrue(file.isFile)
        assertEquals(300L, file.length())
        assertEquals(emptyList<TrashedContent>(), trash().list())
    }

    @Test fun restoreNeverReplacesAPackageInstalledAgain() {
        install(tu, "TU5")
        val entry = leased { trash().moveToTrash(it, title, tu, "TU5", "Title Update 5") }
        install(tu, "TU5", bytes = 999)                                   // installed again meanwhile
        val conflict = trash().restoreConflict(entry.id)!!
        assertEquals(RestoreRefusedException.Why.INSTALLED_AGAIN, conflict.why)
        assertEquals("Title Update 5", conflict.name)
        assertEquals("\"Title Update 5\" is installed again; remove that one first or delete this one for good", conflict.message)
        assertThrows(IllegalStateException::class.java) { leased { trash().restore(it, entry.id) } }
        assertEquals(999, File(data(tu, "TU5"), "content.bin").length().toInt())   // untouched
        assertEquals(listOf(entry.id), trash().list().map { it.id })              // still in the trash
    }

    @Test fun onlyManagedPackagesAndCleanNamesAreAccepted() {
        install(dlc, "ok")
        leased { l ->
            assertThrows(IllegalArgumentException::class.java) { trash().moveToTrash(l, title, 0x00000001, "ok", "save") }
            assertThrows(IllegalArgumentException::class.java) { trash().moveToTrash(l, "00000000", dlc, "ok", "x") }
            assertThrows(IllegalArgumentException::class.java) { trash().moveToTrash(l, title, dlc, "../escape", "x") }
            assertThrows(IllegalArgumentException::class.java) { trash().moveToTrash(l, title, dlc, "..", "x") }
            assertThrows(IllegalArgumentException::class.java) { trash().moveToTrash(l, title, dlc, "missing", "x") }
            assertThrows(IllegalArgumentException::class.java) { trash().restore(l, "../../etc") }
            assertThrows(IllegalArgumentException::class.java) { trash().purge(l, "4D5307E6-00000002-1") }
        }
        assertTrue(data(dlc, "ok").isDirectory)
    }

    @Test fun theQuotaAsksForAnExplicitCleanup() {
        install(dlc, "a", bytes = 600)
        install(dlc, "b", bytes = 600)
        val small = trash(quota = 1000)
        leased { small.moveToTrash(it, title, dlc, "a", "A") }
        val full = assertThrows(TrashFullException::class.java) { leased { small.moveToTrash(it, title, dlc, "b", "B") } }
        assertEquals(630L, full.usedBytes)
        assertEquals(630L, full.itemBytes)
        assertTrue(data(dlc, "b").isDirectory)                               // not moved
        assertEquals(1, leased { small.purgeAll(it) })
        now += 1
        leased { small.moveToTrash(it, title, dlc, "b", "B") }
        assertEquals(listOf("B"), small.list().map { it.displayName })
    }

    @Test fun purgingRemovesOnlyThatEntry() {
        install(dlc, "a"); install(dlc, "b")
        val a = leased { trash().moveToTrash(it, title, dlc, "a", "A") }
        now += 1
        val b = leased { trash().moveToTrash(it, title, dlc, "b", "B") }
        assertEquals(listOf(b.id, a.id), trash().list().map { it.id })     // newest first
        leased { trash().purge(it, a.id) }
        assertEquals(listOf(b.id), trash().list().map { it.id })
    }

    @Test fun aKilledTrashingIsRolledBackAndAKilledRestoreIsFinished() {
        // Killed between moving the data and committing: data in the staging entry, header still home.
        install(dlc, "half")
        val trashRoot = File(root, ".xendroid-trash/content").apply { mkdirs() }
        val id = "$title-00000002-${"%013d".format(now)}"
        val staging = File(trashRoot, "$id.partial").apply { mkdirs() }
        File(staging, "manifest.json").writeText(
            """{"id":"$id","titleId":"$title","contentType":2,"pkgDir":"half","displayName":"Half","bytes":130,"deletedAt":$now}""")
        Files.move(data(dlc, "half").toPath(), File(staging, "data").toPath())
        // A staging folder made before its manifest: nothing was moved, so it just goes.
        File(trashRoot, "$title-00000002-${"%013d".format(now + 5)}.partial").mkdirs()

        assertEquals(2, leased { trash().recover(it) })
        assertTrue(File(data(dlc, "half"), "content.bin").isFile)          // back where it was
        assertTrue(header(dlc, "half").isFile)
        assertEquals(emptyList<String>(), trashRoot.list()!!.toList())

        // Killed in the middle of a restore: header back, data still in the entry.
        val entry = leased { trash().moveToTrash(it, title, dlc, "half", "Half") }
        val restoring = File(trashRoot, "${entry.id}.restoring")
        Files.move(File(trashRoot, entry.id).toPath(), restoring.toPath())
        Files.move(File(restoring, "header").toPath(), header(dlc, "half").toPath())
        assertEquals(1, leased { trash().recover(it) })
        assertTrue(File(data(dlc, "half"), "content.bin").isFile)
        assertEquals(emptyList<String>(), trashRoot.list()!!.toList())
    }

    @Test fun aRestoreWhosePlaceWasTakenStaysInTheTrash() {
        install(dlc, "p")
        val entry = leased { trash().moveToTrash(it, title, dlc, "p", "P") }
        val trashRoot = File(root, ".xendroid-trash/content")
        Files.move(File(trashRoot, entry.id).toPath(), File(trashRoot, "${entry.id}.restoring").toPath())
        install(dlc, "p", bytes = 5)                                       // the place got taken
        leased { trash().recover(it) }
        assertEquals(listOf(entry.id), trash().list().map { it.id })
        assertEquals(5, File(data(dlc, "p"), "content.bin").length().toInt())
    }
}
