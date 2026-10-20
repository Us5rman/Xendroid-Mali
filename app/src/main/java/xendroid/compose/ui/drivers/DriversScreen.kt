package xendroid.compose.ui.drivers

import android.content.Context
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.R
import xendroid.compose.driver.DriverInfo
import xendroid.compose.driver.DriverPackageIO
import xendroid.compose.driver.DriverRepository
import xendroid.compose.driver.DriverSources
import xendroid.compose.driver.DriverSuggestion
import xendroid.compose.driver.InstalledDriverPackage
import xendroid.compose.driver.InstalledDrivers
import xendroid.compose.settings.SettingsSchema
import xendroid.compose.settings.SettingsViewModel
import xendroid.compose.settings.TurnipFlags
import xendroid.compose.settings.setRaw
import xendroid.compose.ui.design.BadgeTone
import xendroid.compose.ui.design.GameCover
import xendroid.compose.ui.design.LocalXdToast
import xendroid.compose.ui.design.NoteTone
import xendroid.compose.ui.design.XdToastState
import xendroid.compose.ui.design.Xd
import xendroid.compose.ui.design.XdArea
import xendroid.compose.ui.design.XdBadge
import xendroid.compose.ui.design.XdBar
import xendroid.compose.ui.design.XdButton
import xendroid.compose.ui.design.XdButtonKind
import xendroid.compose.ui.design.XdButtonSize
import xendroid.compose.ui.design.XdCard
import xendroid.compose.ui.design.XdEmpty
import xendroid.compose.ui.design.XdIcons
import xendroid.compose.ui.design.XdKv
import xendroid.compose.ui.design.XdListRow
import xendroid.compose.ui.design.XdNote
import xendroid.compose.ui.design.XdSection
import xendroid.compose.ui.design.XdSectionedScreen
import xendroid.compose.ui.design.XdSheet
import xendroid.compose.ui.design.XdSwitch
import xendroid.compose.ui.design.XdText
import xendroid.compose.ui.design.XdTextInput
import xendroid.compose.ui.library.formatSize
import xendroid.compose.ui.settings.driverName

private const val PREFS = "xendroid_driver_manager"
private const val KEY = "Vulkan|vulkan_lib_path"
private const val TURNIP = "Vulkan|turnip_debug"

/**
 * The driver manager's state: the choice of every game ([global] config), what the last run
 * loaded, the installed packages, what the sources offer, a download in flight. Same files and
 * preferences as the older manager (selection history per scope, installed downloads by URL).
 */
@Stable
class DriversState(private val context: Context, private val global: SettingsViewModel, private val scope: CoroutineScope,
                   private val toast: XdToastState) {
    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val root = xendroid.compose.Application.get_custom_driver_dir()
    var installed by mutableStateOf<List<InstalledDriverPackage>?>(null)
        private set
    var available by mutableStateOf<List<DriverInfo>?>(null)
        private set
    var loading by mutableStateOf(false)
        private set
    var downloading by mutableStateOf<String?>(null)
        private set
    var progress by mutableIntStateOf(0)
        private set
    var sources by mutableStateOf(DriverSources.decode(prefs.getString("sources", null)))
        private set
    var lastRun by mutableStateOf<xendroid.compose.sessions.SessionRun?>(null)
        private set
    var previous by mutableStateOf(prefs.getString("previous_global_$KEY", null))
        private set
    var selectedAt by mutableStateOf(prefs.getLong("selected_at_global_$KEY", 0L).takeIf { it > 0 })
        private set
    val gpu: String? = xendroid.compose.core.EmulatorRuntime.gpuDeviceName

    fun refresh() {
        scope.launch {
            installed = withContext(Dispatchers.IO) { runCatching { InstalledDrivers.list(root) }.getOrDefault(emptyList()) }
            lastRun = withContext(Dispatchers.IO) {
                runCatching { xendroid.compose.sessions.SessionRuns.store().runs().firstOrNull { it.driver != null } }.getOrNull()
            }
        }
    }

    /** Every game's driver from the next launch on; the previous choice stays one tap away. */
    fun select(path: String, current: String) {
        if (path == current) return
        val now = System.currentTimeMillis()
        previous = current
        selectedAt = now
        prefs.edit().putString("previous_global_$KEY", current).putLong("selected_at_global_$KEY", now).apply()
        SettingsSchema.byKey[KEY]?.let { global.setRaw(it, path) }
        toast.show(context.getString(R.string.drv_next_launch, driverName(context, path)))
    }

    fun loadAvailable(force: Boolean = false) {
        if (loading || (available != null && !force)) return
        loading = true
        scope.launch {
            runCatching { DriverRepository.loadDrivers(sources) }
                .onSuccess { listing ->
                    available = listing.drivers
                    listing.failures.forEach { (source, reason) -> toast.show(context.getString(R.string.drv_source_failed, source, reason)) }
                }
                .onFailure { toast.show(context.getString(R.string.drv_load_failed, it.message)) }
            loading = false
        }
    }

    /** The installed copy of a listed download, when it is still there and matches its checksum. */
    fun installedPath(driver: DriverInfo): String? = prefs.getString("installed_${driver.url}", null)?.takeIf {
        java.io.File(it).isFile && (driver.sha256.isEmpty() || prefs.getString("digest_${driver.url}", "") == driver.sha256.lowercase())
    }

    fun download(driver: DriverInfo, current: String) {
        if (downloading != null) return
        installedPath(driver)?.let { select(it, current); return }
        downloading = driver.url
        progress = 0
        scope.launch {
            runCatching { DriverRepository.downloadDriver(context, driver) { progress = it } }
                .onSuccess { file ->
                    runCatching { DriverPackageIO.install(file, driver.sha256.takeIf { it.isNotBlank() }) }
                        .onSuccess { result ->
                            prefs.edit().putString("installed_${driver.url}", result.library.absolutePath)
                                .putString("digest_${driver.url}", result.sha256).apply()
                            InstalledDrivers.forget()
                            select(result.library.absolutePath, current)
                            toast.show(context.getString(if (result.verifiedDownload) R.string.drv_installed_verified else R.string.drv_installed_unverified))
                        }
                        .onFailure { toast.show(context.getString(R.string.drv_install_failed, it.message)) }
                }
                .onFailure { toast.show(context.getString(R.string.drv_download_failed, it.message)) }
            downloading = null
            progress = 0
            refresh()
        }
    }

    fun import(uri: android.net.Uri, current: String) {
        if (downloading != null) return
        downloading = "import"
        scope.launch {
            try {
                val result = DriverPackageIO.import(context, uri)
                InstalledDrivers.forget()
                select(result.library.absolutePath, current)
                toast.show(context.getString(R.string.drv_imported))
            } catch (e: Exception) {
                if (e is kotlinx.coroutines.CancellationException) throw e
                toast.show(context.getString(R.string.drv_import_failed, e.message))
            } finally {
                downloading = null
                refresh()
            }
        }
    }

    /** Removes an installed package that nothing selects (the global choice is checked by the caller). */
    fun remove(pkg: InstalledDriverPackage) {
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    xendroid.compose.archive.ContentLease.acquire(root).use { xendroid.compose.archive.ArchiveFiles.deleteTree(pkg.dir) }
                }.isSuccess
            }
            InstalledDrivers.forget()
            toast.show(context.getString(if (ok) R.string.xd_drv_removed else R.string.xd_drv_remove_failed, pkg.name))
            refresh()
        }
    }

    fun changeSources(next: List<String>) {
        sources = next
        prefs.edit().putString("sources", DriverSources.encode(next)).apply()
        available = null
    }
}

/**
 * Drivers (docs/ui-redesign/bc, batch 1): what every game uses and what the last session really
 * loaded, the installed packages, the downloads of the sources, the sources and the Turnip flags.
 */
@Composable
fun DriversScreen(
    global: SettingsViewModel,
    onBack: () -> Unit,
    onGameSettings: ((String) -> Unit)? = null,
    gameName: (String) -> String? = { null },
    gameArt: (String) -> Any? = { null },
    initialSection: String = "drv:now",
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val toast = LocalXdToast.current
    val values by global.values.collectAsStateWithLifecycle()
    val gameValues by global.gameValues.collectAsStateWithLifecycle()
    val state = remember(global) { DriversState(context, global, scope, toast) }
    LaunchedEffect(state) { state.refresh() }
    val current = values[KEY]?.raw.orEmpty()
    var section by rememberSaveable { mutableStateOf(initialSection) }
    LaunchedEffect(section) { if (section == "drv:avail") state.loadAvailable() }
    val pickZip = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) state.import(uri, current) }
    var removing by remember { mutableStateOf<InstalledDriverPackage?>(null) }
    val gamesWithDriver = gameValues.filterValues { KEY in it }

    val sections = listOf(
        XdSection("drv:now", stringResource(R.string.xd_drv_now), XdIcons.chip, group = stringResource(R.string.xd_drv_group_driver)) {
            NowBody(state, current, gamesWithDriver, onGameSettings, gameName, gameArt, onSection = { section = it })
        },
        XdSection("drv:inst", stringResource(R.string.xd_drv_installed), XdIcons.check, badge = state.installed?.let { (it.size + 1).toString() }) {
            InstalledBody(state, current, gamesWithDriver, onImport = { pickZip.launch(arrayOf("application/zip")) }, onRemove = { removing = it })
        },
        XdSection("drv:avail", stringResource(R.string.xd_drv_available), XdIcons.download,
            badge = state.available?.count { state.installedPath(it) == null }?.takeIf { it > 0 }?.toString()) {
            AvailableBody(state, current)
        },
        XdSection("drv:src", stringResource(R.string.xd_drv_sources), XdIcons.cloud, group = stringResource(R.string.xd_drv_group_advanced),
            badge = state.sources.size.toString()) { SourcesBody(state) },
        XdSection("drv:tu", stringResource(R.string.xd_drv_turnip), XdIcons.flask) { TurnipBody(global, values[TURNIP]?.raw ?: "sysmem") },
    )
    XdSectionedScreen(
        title = stringResource(R.string.xd_drv_title),
        sections = sections, selected = section, onSelect = { section = it },
        area = XdArea.DRIVERS,
        subtitle = state.gpu?.let { stringResource(R.string.xd_drv_subtitle, it) },
        onBack = onBack, headIcon = XdIcons.chip,
    )
    removing?.let { pkg ->
        val users = gamesWithDriver.filterValues { it[KEY] == pkg.library.absolutePath }.keys
        XdSheet(onDismiss = { removing = null }, title = stringResource(R.string.xd_drv_remove_title, pkg.name),
            actions = {
                XdButton(stringResource(R.string.common_cancel), { removing = null }, kind = XdButtonKind.GHOST)
                XdButton(stringResource(R.string.common_remove), { removing = null; state.remove(pkg) }, kind = XdButtonKind.DANGER)
            }) {
            Text(stringResource(R.string.xd_drv_remove_text), style = XdText.bodySm, color = Xd.colors.fg2)
            if (users.isNotEmpty()) XdNote(pluralStringResource(R.plurals.xd_drv_remove_games, users.size, users.size,
                users.joinToString(", ") { gameName(it) ?: it }), tone = NoteTone.WARN)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun NowBody(
    state: DriversState,
    current: String,
    games: Map<String, Map<String, String>>,
    onGameSettings: ((String) -> Unit)?,
    gameName: (String) -> String?,
    gameArt: (String) -> Any?,
    onSection: (String) -> Unit,
) {
    val context = LocalContext.current
    val c = Xd.colors
    val pkg = state.installed?.firstOrNull { it.library.absolutePath == current }
    val last = state.lastRun
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (!xendroid.compose.driver.CustomDrivers.supported) XdNote(stringResource(R.string.xd_drv_unsupported), tone = NoteTone.WARN)
        XdCard(title = stringResource(R.string.xd_drv_chosen_all), icon = XdIcons.chip) {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(driverName(context, current), style = XdText.h2, color = c.fg, modifier = Modifier.align(Alignment.CenterVertically))
                when {
                    current.isBlank() -> XdBadge(stringResource(R.string.xd_drv_badge_system), Modifier.align(Alignment.CenterVertically))
                    pkg?.verifiedLayout == true -> XdBadge(stringResource(R.string.xd_drv_badge_checked), Modifier.align(Alignment.CenterVertically),
                        tone = BadgeTone.OK, icon = XdIcons.check)
                    pkg != null -> XdBadge(stringResource(R.string.xd_drv_older_import), Modifier.align(Alignment.CenterVertically), tone = BadgeTone.WARN)
                    state.installed != null -> XdBadge(stringResource(R.string.xd_drv_badge_missing), Modifier.align(Alignment.CenterVertically), tone = BadgeTone.ERROR)
                }
            }
            XdKv(listOfNotNull(
                state.selectedAt?.let { stringResource(R.string.xd_drv_chosen_at) to java.text.DateFormat.getDateTimeInstance(java.text.DateFormat.MEDIUM,
                    java.text.DateFormat.SHORT).format(java.util.Date(it)) },
                stringResource(R.string.xd_set_driver_last) to (last?.driver?.label ?: "—"),
            ))
            val loaded = last?.driver
            when (xendroid.compose.driver.DriverIdentity.effective(current, loaded, last?.startedAt, state.selectedAt)) {
                xendroid.compose.driver.DriverIdentity.Companion.Effective.NOT_RUN_YET ->
                    XdNote(stringResource(R.string.xd_drv_eff_not_yet, loaded?.label.orEmpty()), tone = NoteTone.INFO)
                xendroid.compose.driver.DriverIdentity.Companion.Effective.CUSTOM_DID_NOT_LOAD ->
                    XdNote(stringResource(R.string.xd_drv_eff_not_loaded), tone = NoteTone.WARN)
                xendroid.compose.driver.DriverIdentity.Companion.Effective.OTHER_LIBRARY ->
                    XdNote(stringResource(R.string.xd_drv_eff_other, current.substringAfterLast('/')), tone = NoteTone.WARN)
                xendroid.compose.driver.DriverIdentity.Companion.Effective.CUSTOM_THEN -> XdNote(stringResource(R.string.xd_drv_eff_custom_then), tone = NoteTone.INFO)
                xendroid.compose.driver.DriverIdentity.Companion.Effective.RAN -> XdNote(stringResource(R.string.xd_drv_eff_ran), tone = NoteTone.OK)
                null -> {}
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                state.previous?.takeIf { it != current }?.let { previous ->
                    val there = previous.isEmpty() || java.io.File(previous).isFile
                    XdButton(if (there) stringResource(R.string.xd_drv_back_to, driverName(context, previous)) else stringResource(R.string.drv_previous_gone),
                        { state.select(previous, current) }, size = XdButtonSize.SM, icon = XdIcons.reset, enabled = there)
                }
                if (current.isNotBlank()) XdButton(stringResource(R.string.xd_drv_use_system), { state.select("", current) }, size = XdButtonSize.SM,
                    kind = XdButtonKind.GHOST)
            }
        }
        val suggested = state.available?.let { DriverSuggestion.suggest(it, state.gpu) }
        val model = DriverSuggestion.adrenoModel(state.gpu)
        if (suggested != null && model != null) XdCard(title = stringResource(R.string.drv_suggested, model), icon = XdIcons.spark) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(suggested.name, style = XdText.label, color = c.fg)
                if (suggested.sha256.isNotBlank()) XdBadge(stringResource(R.string.xd_drv_badge_sha), tone = BadgeTone.OK)
            }
            Text(stringResource(R.string.drv_suggested_note), style = XdText.note, color = c.fg3)
            XdButton(stringResource(R.string.drv_download_install), { state.download(suggested, current) }, size = XdButtonSize.SM, icon = XdIcons.download,
                enabled = state.downloading == null)
        } else if (state.available == null) XdCard(title = stringResource(R.string.xd_drv_suggestion), icon = XdIcons.spark) {
            Text(stringResource(R.string.xd_drv_suggestion_note), style = XdText.note, color = c.fg3)
            XdButton(stringResource(R.string.xd_drv_look), { onSection("drv:avail") }, size = XdButtonSize.SM, kind = XdButtonKind.GHOST)
        }
        XdCard(title = stringResource(R.string.xd_drv_games), icon = XdIcons.gamepad, trailing = games.size.toString()) {
            if (games.isEmpty()) Text(stringResource(R.string.xd_drv_games_none), style = XdText.note, color = c.fg3)
            games.entries.forEachIndexed { i, (title, own) ->
                XdListRow(gameName(title) ?: title, divider = i < games.size - 1, subtitle = driverName(context, own.getValue(KEY)),
                    lead = { Box(Modifier.width(30.dp)) { GameCover(gameArt(title) ?: xendroid.compose.core.R.drawable.app_icon, gameName(title) ?: title, true, radius = 4.dp) } }) {
                    if (onGameSettings != null && gameName(title) != null) XdButton(stringResource(R.string.xd_set_overriding_open), { onGameSettings(title) },
                        size = XdButtonSize.SM, kind = XdButtonKind.GHOST)
                }
            }
        }
    }
}

@Composable
private fun InstalledBody(state: DriversState, current: String, games: Map<String, Map<String, String>>,
                          onImport: () -> Unit, onRemove: (InstalledDriverPackage) -> Unit) {
    val c = Xd.colors
    val context = LocalContext.current
    BoxWithConstraints {
        val narrow = maxWidth < NARROW
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            XdButton(stringResource(R.string.drv_import_zip), onImport, size = XdButtonSize.SM, icon = XdIcons.upload, enabled = state.downloading == null)
            if (state.downloading == "import") XdBar(0.3f)
            XdCard {
                XdListRow(stringResource(R.string.drv_system), icon = XdIcons.cpu, subtitle = stringResource(R.string.xd_drv_system_always),
                    badges = { if (current.isBlank()) XdBadge(stringResource(R.string.xd_drv_in_use), tone = BadgeTone.ACCENT) }) {
                    if (current.isNotBlank()) XdButton(stringResource(R.string.drv_use), { state.select("", current) }, size = XdButtonSize.SM)
                }
                val list = state.installed
                if (list == null) XdEmpty(stringResource(R.string.xd_game_loading))
                list?.forEachIndexed { i, d ->
                    val path = d.library.absolutePath
                    val inUse = path == current
                    val usedByGames = games.values.any { it[KEY] == path }
                    XdListRow(listOfNotNull(d.name, d.version).joinToString(" "), icon = XdIcons.chip, divider = i < list.lastIndex, actionsBelow = narrow,
                        subtitle = listOf(formatSize(d.bytes), java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM).format(java.util.Date(d.installedAt)))
                            .joinToString(" · "),
                        badges = {
                            if (inUse) XdBadge(stringResource(R.string.xd_drv_in_use), tone = BadgeTone.ACCENT)
                            if (d.verifiedLayout) XdBadge(stringResource(R.string.xd_drv_badge_checked), tone = BadgeTone.OK)
                            else XdBadge(stringResource(R.string.xd_drv_older_import), tone = BadgeTone.WARN)
                            if (usedByGames) XdBadge(stringResource(R.string.xd_drv_badge_games))
                        }) {
                        if (!inUse) {
                            XdButton(stringResource(R.string.drv_use), { state.select(path, current) }, size = XdButtonSize.SM)
                            XdButton(stringResource(R.string.common_remove), { onRemove(d) }, size = XdButtonSize.SM, kind = XdButtonKind.GHOST)
                        }
                    }
                }
            }
            Text(stringResource(R.string.xd_drv_installed_note), style = XdText.note, color = c.fg3)
            if (state.installed?.isEmpty() == true) Text(driverName(context, current), style = XdText.tiny, color = c.fg3)
        }
    }
}

@Composable
private fun AvailableBody(state: DriversState, current: String) {
    val c = Xd.colors
    // A phone in portrait: the download button goes under the name instead of squeezing it.
    BoxWithConstraints {
        val narrow = maxWidth < NARROW
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                XdButton(stringResource(R.string.xd_drv_refresh), { state.loadAvailable(force = true) }, size = XdButtonSize.SM, kind = XdButtonKind.GHOST,
                    icon = XdIcons.refresh, enabled = !state.loading)
                Text(pluralStringResource(R.plurals.xd_drv_from_sources, state.sources.size, state.sources.size, state.sources.joinToString(", ")),
                    style = XdText.note, color = c.fg3, modifier = Modifier.weight(1f))
            }
            val list = state.available
            val suggested = list?.let { DriverSuggestion.suggest(it, state.gpu) }
            when {
                state.loading -> XdBar(0.3f)
                list == null -> {}
                list.isEmpty() -> XdEmpty(stringResource(if (state.sources.isEmpty()) R.string.drv_sources_none else R.string.drv_none))
                else -> XdCard {
                    val ordered = listOfNotNull(suggested) + list.filter { it != suggested }
                    ordered.forEachIndexed { i, d ->
                        val installed = state.installedPath(d)
                        XdListRow(d.name, icon = XdIcons.download, divider = i < ordered.lastIndex, actionsBelow = narrow,
                            subtitle = listOfNotNull(d.version, d.source.takeIf { it.isNotEmpty() && state.sources.size > 1 }?.let { stringResource(R.string.drv_from, it) },
                                d.publishedAt.take(10).ifEmpty { null }).joinToString(" · "),
                            badges = {
                                if (d == suggested) DriverSuggestion.adrenoModel(state.gpu)?.let { XdBadge(stringResource(R.string.drv_suggested, it), tone = BadgeTone.ACCENT) }
                                if (d.sha256.isNotBlank()) XdBadge(stringResource(R.string.xd_drv_badge_sha), tone = BadgeTone.OK)
                                else XdBadge(stringResource(R.string.drv_no_checksum), tone = BadgeTone.WARN)
                            }) {
                            if (state.downloading == d.url) {
                                Column(Modifier.width(120.dp)) {
                                    XdBar(state.progress / 100f)
                                    Text("${state.progress}%", style = XdText.tiny, color = c.fg3)
                                }
                            } else XdButton(stringResource(if (installed != null) R.string.drv_use else R.string.drv_download_install), { state.download(d, current) },
                                size = XdButtonSize.SM, enabled = state.downloading == null && installed != current)
                        }
                    }
                }
            }
            Text(stringResource(R.string.xd_drv_available_note), style = XdText.note, color = c.fg3)
        }
    }
}

/** Below this width a row's buttons go under its text. */
private val NARROW = 520.dp

@Composable
private fun SourcesBody(state: DriversState) {
    val c = Xd.colors
    var input by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    val invalid = stringResource(R.string.drv_source_invalid)
    val duplicate = stringResource(R.string.drv_source_duplicate)
    val full = stringResource(R.string.drv_source_full, DriverSources.MAX)
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        XdCard {
            if (state.sources.isEmpty()) Text(stringResource(R.string.drv_sources_none), style = XdText.note, color = c.fg3)
            state.sources.forEachIndexed { i, s ->
                XdListRow(s, icon = XdIcons.cloud, divider = i < state.sources.lastIndex,
                    subtitle = stringResource(if (s.equals(DriverSources.DEFAULT, ignoreCase = true)) R.string.xd_drv_source_usual else R.string.xd_drv_source_yours)) {
                    XdButton(stringResource(R.string.common_remove), { state.changeSources(DriverSources.remove(state.sources, s)) }, size = XdButtonSize.SM,
                        kind = XdButtonKind.GHOST)
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            XdTextInput(input, { input = it; problem = null }, placeholder = stringResource(R.string.drv_source_hint), width = 300.dp,
                modifier = Modifier.weight(1f, fill = false))
            XdButton(stringResource(R.string.drv_source_add), {
                val (next, result) = DriverSources.add(state.sources, input)
                problem = when (result) {
                    DriverSources.Added.ADDED -> { state.changeSources(next); input = ""; null }
                    DriverSources.Added.INVALID -> invalid
                    DriverSources.Added.ALREADY_THERE -> duplicate
                    DriverSources.Added.FULL -> full
                }
            }, size = XdButtonSize.SM, icon = XdIcons.plus)
        }
        problem?.let { XdNote(it, tone = NoteTone.ERROR) }
        if (state.sources.none { it.equals(DriverSources.DEFAULT, ignoreCase = true) }) {
            XdButton(stringResource(R.string.drv_source_default), { state.changeSources(DriverSources.add(state.sources, DriverSources.DEFAULT).first) },
                size = XdButtonSize.SM, kind = XdButtonKind.GHOST)
        }
        Text(stringResource(R.string.drv_sources_note), style = XdText.note, color = c.fg3)
    }
}

@Composable
private fun TurnipBody(global: SettingsViewModel, raw: String) {
    val c = Xd.colors
    val s = SettingsSchema.byKey[TURNIP] ?: return
    val on = TurnipFlags.parse(raw)
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(stringResource(R.string.xd_drv_turnip_note), style = XdText.note, color = c.fg3)
        XdCard {
            TurnipFlags.KNOWN.forEachIndexed { i, flag ->
                XdListRow(flag.name, subtitle = turnipHelp(flag.name) ?: flag.help, divider = i < TurnipFlags.KNOWN.lastIndex,
                    lead = { Text("TU", style = XdText.monoSm, color = c.fg3) }) {
                    XdSwitch(flag.name in on, { global.setRaw(s, TurnipFlags.toggle(raw, flag.name)) }, contentDescription = flag.name)
                }
            }
        }
        Text(stringResource(R.string.xd_drv_turnip_result, on.joinToString(",").ifEmpty { stringResource(R.string.xd_drv_turnip_none) }),
            style = XdText.mono, color = c.fg2)
    }
}

/** The flags' help in the shown language (the English one stays in [TurnipFlags]). */
@Composable
private fun turnipHelp(flag: String): String? = when (flag) {
    "sysmem" -> stringResource(R.string.xd_tu_sysmem)
    "gmem" -> stringResource(R.string.xd_tu_gmem)
    "nolrz" -> stringResource(R.string.xd_tu_nolrz)
    "nolrzfc" -> stringResource(R.string.xd_tu_nolrzfc)
    "noubwc" -> stringResource(R.string.xd_tu_noubwc)
    "nomultipos" -> stringResource(R.string.xd_tu_nomultipos)
    "noconcurrentresolves" -> stringResource(R.string.xd_tu_noconcurrentresolves)
    "noconcurrentunresolves" -> stringResource(R.string.xd_tu_noconcurrentunresolves)
    "forcebin" -> stringResource(R.string.xd_tu_forcebin)
    "flushall" -> stringResource(R.string.xd_tu_flushall)
    "syncdraw" -> stringResource(R.string.xd_tu_syncdraw)
    else -> null
}
