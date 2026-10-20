package xendroid.compose.ui.settings

import xendroid.compose.R
import androidx.compose.ui.res.stringResource
import android.app.Activity
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.Utils
import xendroid.compose.core.SessionLogs
import xendroid.compose.driver.DriverInfo
import xendroid.compose.driver.DriverRepository
import xendroid.compose.driver.DriverPackageIO
import xendroid.compose.driver.DriverPackageInstaller
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingsHost

@Composable
fun SettingRow(host: SettingsHost, s: Setting, modified: Boolean, raw: String? = null) {
    val contract = host.contract(s)
    Column(Modifier.fillMaxWidth()) {
        if (!contract.available) {
            // U03: an unavailable setting keeps its name and says why, instead of vanishing.
            Box(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
                RowTitle(settingTitle(s), modified = false, sub = contractText(contract))
            }
            return@Column
        }
        Text(contractText(contract), style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(start = 16.dp, top = 6.dp))
        when (s) {
    is Setting.Bool       -> BoolRow(host, s, modified)
    is Setting.IntRange   -> IntRow(host, s, modified)
    is Setting.ListChoice -> ListRow(host, s, modified)
    is Setting.Action     ->
        if (s.name == "dump_session_logs") ExportLogsRow(s)
        else DriverActionRow(host, s, modified, raw)
    is Setting.Text       -> TextRow(host, s, modified)
        }
    }
}

@Composable
private fun titleColor(modified: Boolean) =
    if (modified) MaterialTheme.colorScheme.tertiary else MaterialTheme.colorScheme.onSurface

@Composable
private fun RowTitle(text: String, modified: Boolean, sub: String? = null, desc: String = "") {
    Column {
        Text(text, color = titleColor(modified), style = MaterialTheme.typography.bodyLarge)
        if (desc.isNotEmpty()) {
            Text(
                desc,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        if (sub != null) {
            Text(
                sub,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary
            )
        }
    }
}

@Composable
private fun RowValue(value: String) {
    Text(
        value,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 12.dp)
    )
}

@Composable
private fun BoolRow(host: SettingsHost, s: Setting.Bool, modified: Boolean) {
    val checked = remember(modified) { host.currentBool(s) }
    var local by remember { mutableStateOf(checked) }

    Row(
        Modifier
            .fillMaxWidth()
            .clickable {
                local = !local
                host.onBoolChanged(s, local)
            }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.weight(1f)) {
            RowTitle(settingTitle(s), modified, desc = settingDesc(s))
        }

        Switch(
            checked = local,
            onCheckedChange = {
                local = it
                host.onBoolChanged(s, it)
            }
        )
    }
}

@Composable
private fun IntRow(host: SettingsHost, s: Setting.IntRange, modified: Boolean) {
    var showDialog by remember { mutableStateOf(false) }
    val current = host.currentInt(s)

    Row(
        Modifier
            .fillMaxWidth()
            .clickable { showDialog = true }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(Modifier.weight(1f)) {
            RowTitle(settingTitle(s), modified, desc = settingDesc(s))
        }

        RowValue(current.toString())
    }

    if (showDialog) {
        var slider by remember {
            mutableFloatStateOf(
                current.coerceIn(s.min, s.max).toFloat()
            )
        }

        AlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(settingTitle(s)) },
            text = {
                Column {
                    Text(
                        slider.toInt().toString(),
                        style = MaterialTheme.typography.titleLarge
                    )

                    Slider(
                        value = slider,
                        onValueChange = { slider = it },
                        valueRange = s.min.toFloat()..s.max.toFloat(),
                        steps = (s.max - s.min - 1).coerceAtLeast(0)
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        host.onIntChanged(s, slider.toInt())
                        showDialog = false
                    }
                ) {
                    Text(stringResource(R.string.common_ok))
                }
            },
            dismissButton = {
                TextButton(
                    onClick = { showDialog = false }
                ) {
                    Text(stringResource(R.string.common_cancel))
                }
            },
        )
    }
}

@Composable
private fun ListRow(host: SettingsHost, s: Setting.ListChoice, modified: Boolean) {
    var showDialog by remember { mutableStateOf(false) }
    val currentValue = host.currentListValue(s)

    val currentLabel =
        s.options.firstOrNull { it.value == currentValue }?.let { optionLabel(s, it.value, it.label) }
            ?: if (currentValue.isEmpty()) stringResource(R.string.set_default_paren) else currentValue

    Row(
        Modifier
            .fillMaxWidth()
            .clickable { showDialog = true }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Box(Modifier.weight(1f)) {
            RowTitle(settingTitle(s), modified, desc = settingDesc(s))
        }

        RowValue(currentLabel)
    }

    if (showDialog) {
        val listState = rememberLazyListState()
        val selectedIndex = s.options.indexOfFirst { it.value == currentValue }

        LaunchedEffect(Unit) {
            if (selectedIndex > 0) {
                listState.scrollToItem(selectedIndex)
            }
        }

        AlertDialog(
            onDismissRequest = { showDialog = false },
            title = { Text(settingTitle(s)) },
            text = {
                LazyColumn(
                    state = listState,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = 400.dp)
                ) {
                    items(s.options, key = { it.value }) { opt ->
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .selectable(
                                    selected = opt.value == currentValue,
                                    onClick = {
                                        host.onListChanged(s, opt.value)
                                        showDialog = false
                                    }
                                )
                                .padding(vertical = 10.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            RadioButton(
                                selected = opt.value == currentValue,
                                onClick = {
                                    host.onListChanged(s, opt.value)
                                    showDialog = false
                                }
                            )

                            Spacer(Modifier.width(8.dp))
                            Text(optionLabel(s, opt.value, opt.label))
                        }
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { showDialog = false }) {
                    Text(stringResource(R.string.common_cancel))
                }
            },
        )
    }
}

@Composable
private fun ExportLogsRow(s: Setting.Action) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }

    Row(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = !busy) {
                busy = true

                scope.launch {
                    val dest = withContext(Dispatchers.IO) {
                        runCatching {
                            SessionLogs.exportAll()
                        }.getOrNull()
                    }

                    Toast.makeText(
                        context,
                        dest?.let {
                            context.getString(R.string.set_logs_exported, it.name)
                        } ?: context.getString(R.string.set_logs_none),
                        Toast.LENGTH_LONG
                    ).show()

                    busy = false
                }
            }
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.weight(1f)) {
            RowTitle(
                settingTitle(s),
                false,
                sub = if (busy) stringResource(R.string.set_logs_exporting) else stringResource(R.string.set_logs_note),
                desc = settingDesc(s)
            )
        }
    }
}

@Composable
private fun DriverActionRow(
    host: SettingsHost,
    s: Setting.Action,
    modified: Boolean,
    raw: String?
) {
    if (!host.isCustomDriverSupported) return

    val context = LocalContext.current
    val activity = context as? Activity
    val scope = rememberCoroutineScope()
    val current = raw ?: host.currentDriverPath(s)

    val prefs = remember {
        context.getSharedPreferences(
            "xendroid_driver_manager",
            android.content.Context.MODE_PRIVATE
        )
    }

    var showManager by remember { mutableStateOf(false) }
    var drivers by remember { mutableStateOf<List<DriverInfo>>(emptyList()) }
    // 15p: the GitHub repositories the manager reads, and the driver suggested for this GPU.
    var sources by remember { mutableStateOf(xendroid.compose.driver.DriverSources.decode(prefs.getString("sources", null))) }
    var editingSources by remember { mutableStateOf(false) }
    val adreno = remember { xendroid.compose.driver.DriverSuggestion.adrenoModel(xendroid.compose.core.EmulatorRuntime.gpuDeviceName) }
    val suggested = remember(drivers) { xendroid.compose.driver.DriverSuggestion.suggest(drivers, xendroid.compose.core.EmulatorRuntime.gpuDeviceName) }
    var loading by remember { mutableStateOf(false) }
    var downloading by remember { mutableStateOf<String?>(null) }
    var progress by remember { mutableIntStateOf(0) }
    val previousKey = "previous_${host.persistenceKey}_${s.key}"
    val selectedAtKey = "selected_at_${host.persistenceKey}_${s.key}"
    var previousDriver by remember(previousKey) { mutableStateOf(prefs.getString(previousKey, null)) }
    var selectedAt by remember(selectedAtKey) { mutableStateOf(prefs.getLong(selectedAtKey, 0L).takeIf { it > 0 }) }

    // U03: the driver the latest run of this scope actually loaded, against the selection.
    var lastRun by remember { mutableStateOf<xendroid.compose.sessions.SessionRun?>(null) }
    LaunchedEffect(host.persistenceKey) {
        val title = host.persistenceKey.removePrefix("game:").takeIf { host.persistenceKey.startsWith("game:") }
        lastRun = withContext(Dispatchers.IO) {
            runCatching {
                xendroid.compose.sessions.SessionRuns.store().runs()
                    .firstOrNull { it.driver != null && (title == null || it.titleId.equals(title, ignoreCase = true)) }
            }.getOrNull()
        }
    }

    fun selectDriver(path: String) {
        if (path == current) return
        // History belongs to this global/title scope. Keep installed binaries untouched
        // so undoing a selection does not require another download or unsafe deletion.
        previousDriver = current
        val now = System.currentTimeMillis()
        selectedAt = now
        prefs.edit().putString(previousKey, current).putLong(selectedAtKey, now).apply()
        host.onDriverPathChanged(s, path)
    }

    val pickZip = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null && downloading == null) {
            downloading = "import"
            scope.launch {
                try {
                    val installed = DriverPackageIO.import(context, uri)
                    selectDriver(installed.library.absolutePath)
                    Toast.makeText(context, context.getString(R.string.drv_imported), Toast.LENGTH_SHORT).show()
                } catch (e: Exception) {
                    if (e is kotlinx.coroutines.CancellationException) throw e
                    Toast.makeText(context, context.getString(R.string.drv_import_failed, e.message), Toast.LENGTH_LONG).show()
                } finally { downloading = null }
            }
        }
    }

    fun loadDrivers() {
        if (loading || drivers.isNotEmpty()) return

        loading = true

        scope.launch {
            runCatching {
                DriverRepository.loadDrivers(sources)
            }.onSuccess { listing ->
                drivers = listing.drivers
                listing.failures.forEach { (source, reason) ->
                    Toast.makeText(context, context.getString(R.string.drv_source_failed, source, reason), Toast.LENGTH_LONG).show()
                }
            }.onFailure {
                Toast.makeText(
                    context,
                    context.getString(R.string.drv_load_failed, it.message),
                    Toast.LENGTH_LONG
                ).show()
            }

            loading = false
        }
    }

    Column(Modifier.fillMaxWidth()) {

        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(Modifier.weight(1f)) {
                RowTitle(
                    settingTitle(s),
                    modified,
                    sub = stringResource(R.string.drv_next_launch, current.ifEmpty { stringResource(R.string.drv_system) }),
                    desc = settingDesc(s)
                )
            }
        }

        xendroid.compose.driver.DriverIdentity.describeEffective(current, lastRun?.driver, lastRun?.startedAt, selectedAt)?.let { note ->
            Text(note, style = MaterialTheme.typography.bodySmall,
                color = if ("did not load" in note) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp))
        }

        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Start
        ) {
            TextButton(
                onClick = {
                    showManager = true
                    loadDrivers()
                }
            ) {
                Text(stringResource(R.string.drv_manager))
            }

            TextButton(
                enabled = downloading == null,
                onClick = {
                    selectDriver("")
                }
            ) {
                Text(stringResource(R.string.drv_use_default))
            }

            TextButton(
                enabled = downloading == null,
                onClick = {
                    pickZip.launch(arrayOf("application/zip"))
                }
            ) {
                Text(stringResource(R.string.drv_import_zip))
            }
        }
        previousDriver?.let { previous ->
            val available = previous.isEmpty() || java.io.File(previous).isFile
            TextButton(
                enabled = available && downloading == null,
                onClick = { selectDriver(previous) },
            ) {
                Text(if (available) stringResource(R.string.drv_use_previous) else stringResource(R.string.drv_previous_gone))
            }
        }
    }

    if (showManager) {
        AlertDialog(
            onDismissRequest = {
                if (downloading == null) {
                    showManager = false
                }
            },
            title = {
                Text(stringResource(R.string.drv_title))
            },
            text = {
                when {
                    loading -> {
                        Box(
                            Modifier.fillMaxWidth(),
                            contentAlignment = Alignment.Center
                        ) {
                            CircularProgressIndicator()
                        }
                    }

                    drivers.isEmpty() -> {
                        Column {
                            DriverSourcesLine(sources) { editingSources = true }
                            Text(stringResource(if (sources.isEmpty()) R.string.drv_sources_none else R.string.drv_none))
                        }
                    }

                    else -> {
                        LazyColumn(
                            modifier = Modifier
                                .fillMaxWidth()
                                .heightIn(max = 450.dp)
                        ) {
                            item(key = "sources") {
                                DriverSourcesLine(sources) { editingSources = true }
                                if (suggested != null && adreno != null) {
                                    Text(stringResource(R.string.drv_suggested_note), style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                                HorizontalDivider()
                            }
                            items(
                                listOfNotNull(suggested) + drivers.filter { it != suggested },
                                key = { "${it.name}_${it.url}" }
                            ) { driver ->

                                val installedPath =
                                    prefs.getString(
                                        "installed_${driver.url}",
                                        null
                                    )?.takeIf {
                                        java.io.File(it).isFile &&
                                            (driver.sha256.isEmpty() ||
                                                prefs.getString("digest_${driver.url}", "") == driver.sha256.lowercase())
                                    }

                                Column(
                                    Modifier
                                        .fillMaxWidth()
                                        .padding(vertical = 8.dp)
                                ) {
                                    Text(
                                        driver.name,
                                        style = MaterialTheme.typography.bodyLarge
                                    )

                                    if (driver == suggested && adreno != null) {
                                        Text(stringResource(R.string.drv_suggested, adreno),
                                            style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
                                    }

                                    Text(
                                        driver.version,
                                        style = MaterialTheme.typography.bodySmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant
                                    )

                                    if (driver.source.isNotEmpty() && sources.size > 1) {
                                        Text(stringResource(R.string.drv_from, driver.source), style = MaterialTheme.typography.labelSmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    }

                                    Text(
                                        if (driver.sha256.isBlank()) stringResource(R.string.drv_no_checksum)
                                        else stringResource(R.string.drv_checksum),
                                        style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )

                                    if (downloading == driver.url) {
                                        Spacer(Modifier.height(8.dp))

                                        LinearProgressIndicator(
                                            progress = { progress / 100f },
                                            modifier = Modifier.fillMaxWidth()
                                        )

                                        Text("$progress%")
                                    } else {
                                        TextButton(
                                            enabled = downloading == null && activity != null,
                                            onClick = {
                                                if (activity == null) {
                                                    return@TextButton
                                                }

                                                if (installedPath != null) {
                                                    val valid = runCatching { DriverPackageInstaller.validateArm64Library(java.io.File(installedPath)) }.isSuccess
                                                    if (!valid) {
                                                        Toast.makeText(context, context.getString(R.string.drv_damaged), Toast.LENGTH_LONG).show()
                                                        return@TextButton
                                                    }
                                                    selectDriver(installedPath)

                                                    Toast.makeText(
                                                        context,
                                                        context.getString(R.string.drv_selected),
                                                        Toast.LENGTH_SHORT
                                                    ).show()

                                                    return@TextButton
                                                }

                                                downloading = driver.url
                                                progress = 0

                                                scope.launch {
                                                    runCatching {
                                                        DriverRepository.downloadDriver(
                                                            context,
                                                            driver
                                                        ) {
                                                            progress = it
                                                        }
                                                    }.onSuccess { file ->

                                                        val installed = runCatching { DriverPackageIO.install(file, driver.sha256.takeIf { it.isNotBlank() }) }
                                                        installed.onSuccess { result ->
                                                            prefs.edit()
                                                                .putString("installed_${driver.url}", result.library.absolutePath)
                                                                .putString("digest_${driver.url}", result.sha256)
                                                                .apply()
                                                            selectDriver(result.library.absolutePath)
                                                            Toast.makeText(
                                                                context,
                                                                if (result.verifiedDownload) context.getString(R.string.drv_installed_verified) else context.getString(R.string.drv_installed_unverified),
                                                                Toast.LENGTH_SHORT
                                                            ).show()
                                                        }.onFailure { error ->
                                                            if (error is kotlinx.coroutines.CancellationException) throw error
                                                            Toast.makeText(context, context.getString(R.string.drv_install_failed, error.message), Toast.LENGTH_LONG).show()
                                                        }
                                                    }.onFailure {
                                                        Toast.makeText(
                                                            context,
                                                            context.getString(R.string.drv_download_failed, it.message),
                                                            Toast.LENGTH_LONG
                                                        ).show()
                                                    }

                                                    downloading = null
                                                    progress = 0
                                                }
                                            }
                                        ) {
                                            Text(
                                                if (installedPath != null) {
                                                    stringResource(R.string.drv_use)
                                                } else {
                                                    stringResource(R.string.drv_download_install)
                                                }
                                            )
                                        }
                                    }

                                    HorizontalDivider()
                                }
                            }
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(
                    enabled = downloading == null,
                    onClick = {
                        showManager = false
                    }
                ) {
                    Text(stringResource(R.string.common_close))
                }
            }
        )
    }
    if (editingSources) {
        DriverSourcesDialog(
            sources = sources,
            onChange = { next ->
                sources = next
                prefs.edit().putString("sources", xendroid.compose.driver.DriverSources.encode(next)).apply()
                drivers = emptyList()   // read again from the new list
                loadDrivers()
            },
            onDismiss = { editingSources = false },
        )
    }
}

/** 15p: the sources the manager reads, with the button that edits them. */
@Composable
private fun DriverSourcesLine(sources: List<String>, onEdit: () -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            if (sources.isEmpty()) stringResource(R.string.drv_sources_none)
            else stringResource(R.string.drv_sources, sources.joinToString(", ")),
            style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f),
        )
        TextButton(onClick = onEdit) { Text(stringResource(R.string.drv_sources_edit)) }
    }
}

/** 15p: add or remove the GitHub repositories the driver manager reads. */
@Composable
internal fun DriverSourcesDialog(sources: List<String>, onChange: (List<String>) -> Unit, onDismiss: () -> Unit) {
    var input by remember { mutableStateOf("") }
    var problem by remember { mutableStateOf<String?>(null) }
    val invalid = stringResource(R.string.drv_source_invalid)
    val duplicate = stringResource(R.string.drv_source_duplicate)
    val full = stringResource(R.string.drv_source_full, xendroid.compose.driver.DriverSources.MAX)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.drv_sources_title)) },
        text = {
            Column {
                Text(stringResource(R.string.drv_sources_note), style = MaterialTheme.typography.bodySmall)
                sources.forEach { source ->
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Text(source, modifier = Modifier.weight(1f))
                        TextButton(onClick = { onChange(xendroid.compose.driver.DriverSources.remove(sources, source)) }) {
                            Text(stringResource(R.string.common_remove))
                        }
                    }
                }
                if (sources.none { it.equals(xendroid.compose.driver.DriverSources.DEFAULT, ignoreCase = true) }) {
                    TextButton(onClick = {
                        onChange(xendroid.compose.driver.DriverSources.add(sources, xendroid.compose.driver.DriverSources.DEFAULT).first)
                    }) { Text(stringResource(R.string.drv_source_default)) }
                }
                OutlinedTextField(
                    value = input,
                    onValueChange = { input = it; problem = null },
                    label = { Text(stringResource(R.string.drv_source_hint)) },
                    singleLine = true,
                    isError = problem != null,
                    supportingText = problem?.let { { Text(it) } },
                    modifier = Modifier.fillMaxWidth(),
                )
                TextButton(onClick = {
                    val (next, result) = xendroid.compose.driver.DriverSources.add(sources, input)
                    problem = when (result) {
                        xendroid.compose.driver.DriverSources.Added.ADDED -> { onChange(next); input = ""; null }
                        xendroid.compose.driver.DriverSources.Added.INVALID -> invalid
                        xendroid.compose.driver.DriverSources.Added.ALREADY_THERE -> duplicate
                        xendroid.compose.driver.DriverSources.Added.FULL -> full
                    }
                }) { Text(stringResource(R.string.drv_source_add)) }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_close)) } },
    )
}

@Composable
fun OverrideRow(
    host: SettingsHost,
    s: Setting,
    overrideValue: String?,
    onOverrideToggle: (Boolean) -> Unit,
) {
    val overridden = overrideValue != null

    Row(
        Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Switch(
            checked = overridden,
            onCheckedChange = onOverrideToggle,
            modifier = Modifier.padding(start = 12.dp)
        )

        Box(Modifier.weight(1f)) {
            if (overridden) {
                key(overrideValue) {
                    SettingRow(host, s, modified = true)
                }
            } else {
                InheritedPreview(host, s)
            }
        }
    }
}

@Composable
private fun InheritedPreview(host: SettingsHost, s: Setting) {
    val grey = MaterialTheme.colorScheme.onSurfaceVariant

    when (s) {
        is Setting.Bool -> Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(
                    settingTitle(s),
                    color = grey,
                    style = MaterialTheme.typography.bodyLarge
                )

                if (settingDesc(s).isNotEmpty()) {
                    Text(
                        settingDesc(s),
                        color = grey,
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }

            Switch(
                checked = host.currentBool(s),
                onCheckedChange = null,
                enabled = false
            )
        }

        is Setting.IntRange ->
            InheritedTextRow(
                settingTitle(s),
                host.currentInt(s).toString(),
                settingDesc(s),
                grey
            )

        is Setting.ListChoice -> {
            val v = host.currentListValue(s)

            val label =
                s.options.firstOrNull { it.value == v }?.let { optionLabel(s, it.value, it.label) }
                    ?: v.ifEmpty { stringResource(R.string.set_default_paren) }

            InheritedTextRow(
                settingTitle(s),
                label,
                settingDesc(s),
                grey
            )
        }

        is Setting.Text ->
            InheritedTextRow(settingTitle(s), host.currentText(s).ifEmpty { s.placeholder }, settingDesc(s), grey)

        is Setting.Action ->
            InheritedTextRow(
                settingTitle(s),
                host.currentDriverPath(s).ifEmpty { stringResource(R.string.drv_default) },
                settingDesc(s),
                grey
            )
    }
}

@Composable
private fun InheritedTextRow(
    title: String,
    value: String,
    desc: String,
    grey: Color
) {
    Column(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 12.dp)
    ) {
        Text(
            title,
            color = grey,
            style = MaterialTheme.typography.bodyLarge
        )

        if (desc.isNotEmpty()) {
            Text(
                desc,
                color = grey,
                style = MaterialTheme.typography.bodySmall
            )
        }

        Text(
            value,
            color = grey,
            style = MaterialTheme.typography.bodySmall
        )
    }
}

@Composable
private fun TextRow(host: SettingsHost, s: Setting.Text, modified: Boolean) {
    var text by remember(s.key) { mutableStateOf(host.currentText(s)) }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
        RowTitle(settingTitle(s), modified, desc = settingDesc(s))
        OutlinedTextField(
            value = text,
            onValueChange = { text = it; host.onTextChanged(s, it) },
            singleLine = true,
            placeholder = { Text(s.placeholder) },
            modifier = Modifier.fillMaxWidth(),
        )
    }
}
