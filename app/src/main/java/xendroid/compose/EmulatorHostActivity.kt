package xendroid.compose

import android.content.Intent
import android.content.ClipData
import android.annotation.SuppressLint
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.core.view.WindowCompat
import androidx.core.content.FileProvider
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import xendroid.compose.core.EmuProcessLink
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.core.FrontendLaunch
import xendroid.compose.core.EmulatorSession
import xendroid.compose.core.ScreenBrightnessSampler
import xendroid.compose.core.SessionLogs
import xendroid.compose.core.HudMetric
import xendroid.compose.core.PresentationState
import xendroid.compose.core.LsfgAssets
import xendroid.compose.core.BackgroundPolicy
import xendroid.compose.core.GyroCamera
import xendroid.compose.core.refreshChoices
import xendroid.compose.core.selectRefresh
import xendroid.compose.core.sustainedPerformance
import xendroid.compose.core.ExternalGameDisplay
import xendroid.compose.core.PresenterPerformanceHints
import kotlin.math.abs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.firstOrNull
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import androidx.compose.runtime.setValue
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.unit.IntSize
import kotlinx.coroutines.flow.first
import xendroid.compose.gamepad.atRect
import android.content.res.Configuration
import android.widget.Toast
import android.os.VibrationEffect
import android.os.Vibrator
import android.preference.PreferenceManager
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Alignment
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import kotlin.math.roundToInt
import kotlinx.coroutines.flow.StateFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.viewinterop.AndroidView
import xendroid.compose.ui.disc.DiscSwapPanel
import xendroid.compose.ui.keyboard.GuestKeyboardPanel
import xendroid.compose.ui.messagebox.GuestMessageBoxPanel
import xendroid.compose.ui.ingame.InGameAction
import xendroid.compose.ui.ingame.InGameMenu
import xendroid.compose.ui.ingame.InGameMenuEdge
import xendroid.compose.ui.ingame.InGameMenuModel
import xendroid.compose.ui.ingame.MenuValue
import xendroid.compose.ui.ingame.RowKind
import xendroid.compose.ui.ingame.InGameMenuState
import xendroid.compose.ui.ingame.InGamePage
import xendroid.compose.ui.theme.xendroidTheme
import xendroid.compose.gamepad.GamepadConfigDto
import xendroid.compose.gamepad.GamepadController
import xendroid.compose.gamepad.GamepadEditorScreen
import xendroid.compose.gamepad.GamepadOverlay
import xendroid.compose.gamepad.Kc
import xendroid.compose.gamepad.rememberAutoHide
import xendroid.compose.data.GameButtons
import xendroid.compose.data.KeymapStore
import xendroid.compose.settings.ConfigStore
import xendroid.compose.settings.FpsConfigSnapshot
import xendroid.compose.settings.InGameConfigRepository
import xendroid.compose.settings.InGameChanges
import xendroid.compose.settings.InGameChangesStore
import xendroid.compose.settings.InGamePrefs

/**
 * The :emu emulator host (separate process; see manifest). Reads game_uri from the Intent,
 * performs the PRE-surface native setup, hosts a Vulkan SurfaceView, and drives the exact
 * surface->boot ordering. onDestroy hard-kills the process.
 */
class EmulatorHostActivity : ComponentActivity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "EmuHost"
        // Round 2: the settings the in-game menu keeps for the game (config keys) and the live ones Undo puts back.
        private const val FPS_KEY = "GPU|framerate_limit"
        private const val SCALING_KEY = "Display|postprocess_scaling_and_sharpening"
        private const val AA_KEY = "Display|postprocess_antialiasing"
        private const val CAS_KEY = "Display|postprocess_ffx_cas_additional_sharpness"
        private const val DITHER_KEY = "Display|postprocess_dither"
        private const val TOUCH_KEY = "HID|show_touch_overlay"
        private const val VOLUME_KEY = "APU|volume"
        private const val IMAGE_LIVE = "image"
        private val MENU_CONFIG_KEYS = listOf(FPS_KEY, SCALING_KEY, AA_KEY, CAS_KEY, DITHER_KEY, TOUCH_KEY, VOLUME_KEY) +
            xendroid.compose.core.GpuLiveOption.entries.map { it.key }
        private val FPS_CHOICES = listOf(0, 30, 45, 60, 90, 120)
        private val HUD_DETAILS = listOf(xendroid.compose.core.HudDetail.COMPACT, xendroid.compose.core.HudDetail.FULL,
            xendroid.compose.core.HudDetail.PANEL)
        private const val HUD_SCALE_SPAN = xendroid.compose.core.HudPlacements.MAX_SCALE - xendroid.compose.core.HudPlacements.MIN_SCALE
        private const val KEYBOARD_POLL_MS = 150L
        const val EXTRA_GAME_URI = "game_uri"
        const val EXTRA_DISC_LABELS = "disc_labels"
        const val EXTRA_DISC_PATHS = "disc_paths"
        /** 15e: shown while the game starts; a path to one of this app's own cover files. */
        const val EXTRA_GAME_NAME = "game_name"
        const val EXTRA_GAME_ART = "game_art"
        /** 15e: "Try again" asks the main process to start the game again in a new :emu. */
        const val ACTION_RELAUNCH_GAME = "xendroid.intent.action.RELAUNCH_GAME"
        /** R4 "Start with…": command-line cvars for this launch only, honoured with this app's
         *  [xendroid.compose.core.LaunchToken] and after [xendroid.compose.core.LaunchOptions.sanitize]. */
        const val EXTRA_LAUNCH_ARGS = "launch_args"
        const val EXTRA_LAUNCH_TOKEN = "launch_token"
        /** The extras a relaunch carries over (the game, its discs, what the loading screen shows,
         *  the options of this launch). */
        val RELAUNCH_EXTRAS = listOf(EXTRA_GAME_URI, EXTRA_DISC_LABELS, EXTRA_DISC_PATHS, EXTRA_GAME_NAME, EXTRA_GAME_ART,
            EXTRA_LAUNCH_ARGS, EXTRA_LAUNCH_TOKEN)

        private const val KC_DPAD_LEFT = 0
        private const val KC_DPAD_UP = 1
        private const val KC_DPAD_RIGHT = 2
        private const val KC_DPAD_DOWN = 3
        private const val KC_A = 4
        private const val KC_B = 5
        private const val KC_X = 6
        private const val KC_Y = 7
        private const val KC_BACK = 8
        private const val KC_START = 9
        private const val KC_SHOULDER_L = 10
        private const val KC_SHOULDER_R = 11
        private const val KC_TRIGGER_L = 14
        private const val KC_TRIGGER_R = 15
        private const val KC_LTHUMB_LEFT = 16
        private const val KC_LTHUMB_UP = 17
        private const val KC_LTHUMB_RIGHT = 18
        private const val KC_LTHUMB_DOWN = 19
        private const val KC_RTHUMB_LEFT = 20
        private const val KC_RTHUMB_UP = 21
        private const val KC_RTHUMB_RIGHT = 22
        private const val KC_RTHUMB_DOWN = 23
        private const val KEY_VALUE_UNUSED = -1
        private const val AXIS_DEADZONE = 0.08f
        private const val FOCUS_PAUSE_DEBOUNCE_MS = 250L
        /** Running seconds without a new guest frame before the flight recorder notes a stall. */
        private const val STALL_EVENT_SECONDS = 3

        private const val DISPLAY_SETTINGS_PREFS = "display_settings"
        private const val FULLSCREEN_STRETCH_KEY =
            "fullscreen_stretch_enabled"
    }

    private val session = EmulatorSession()
    /** Guards enterImmersiveMode() against re-running on every focus-gain (see its comment). */
    private var immersiveModeApplied = false
    /** Durable record of this guest run (sessions/SessionRunStore); null if unavailable. */
    @Volatile private var runId: String? = null
    private var lastRunHeartbeatMs = 0L
    /** One-second samples of this run (main thread only), saved with the heartbeat and at the end. */
    private val runPerformance = xendroid.compose.sessions.RunPerformanceAccumulator()
    /** Flight recorder of this run (C01): rare host events, flushed beside the run record. */
    private val runEvents = xendroid.compose.sessions.RunEventRecorder(clock = android.os.SystemClock::elapsedRealtime)
    private var lastPresentationLabel: String? = null
    private var lastPresentCount = -1L
    private var stalledSeconds = 0
    private var lastThermalStatus = -1
    /** 15j: warns before the phone throttles for heat (advisory: it never changes a setting). */
    private val thermalWatch = xendroid.compose.core.ThermalWatch()
    private var thermalNoticeAt = Long.MIN_VALUE
    private var thermalTick = 0
    /** 15n: the last thermal headroom read (null where the device has none). */
    private var lastHeadroom: Float? = null
    private var driverRecorded = false
    /** Seconds with >= 100 ms spent creating pipelines (ns counter, pipelines alongside). */
    private val compileBursts = xendroid.compose.sessions.BurstTracker(threshold = 100_000_000, quietSeconds = 2)
    /** Seconds with concealed audio blocks; 5 quiet seconds end a burst, so sporadic dropouts stay one event pair. */
    private val audioBursts = xendroid.compose.sessions.BurstTracker(threshold = 1, quietSeconds = 5)
    /** Shown over the black screen until the first guest frame (U09); null afterwards. */
    private val bootStatus = mutableStateOf<xendroid.compose.ui.ingame.BootStatus?>(
        xendroid.compose.ui.ingame.BootStatus(xendroid.compose.ui.ingame.BootStatus.Stage.EMULATOR))
    /** U09: Cancel was pressed on the label; the start sequence stops at its next step. */
    private var bootCancelled = false
    /** 15e: the loading screen's picture and name (from the library, or the title's cover once known). */
    private val loadingArt = mutableStateOf<java.io.File?>(null)
    private var loadingName: String? = null
    /** Batch 2: who plays and what the game starts with, under the loading screen's stages. */
    private val loadingDetails = mutableStateOf<xendroid.compose.ui.ingame.LoadingDetails?>(null)
    /** Batch 2: the in-game menu's status line, refreshed each second while it is open. */
    private val menuStatus = mutableStateOf<List<xendroid.compose.ui.ingame.MenuStat>>(emptyList())
    /** Batch 2: why the game did not start, shown over everything (replaces the old dialog). */
    private val launchFailureState = mutableStateOf<xendroid.compose.ui.ingame.LaunchFailure?>(null)
    /** What Android's GameManager hears: loading, playing, paused under the menu, or nothing. */
    private val gameModeSignal by lazy { xendroid.compose.core.GameModeSignal(applicationContext) }
    private val createdAtMs = android.os.SystemClock.elapsedRealtime()
    /** Connected controllers by device id, described by vendor/product only. */
    private val controllers = mutableMapOf<Int, String>()
    /** I01: physical controllers -> players. P1 keeps the existing path; P2-P4 go through [slotInput]. */
    private val controllerSlots = xendroid.compose.gamepad.ControllerSlots()
    private val slotInput = xendroid.compose.gamepad.SlotInputRouter(AXIS_DEADZONE) { slot, key, pressed, value ->
        session.keyEventSlot(slot, key, pressed, value)
    }
    /** Device id -> descriptor of each known controller (a removed device can no longer be asked). */
    private val controllerKeys = mutableMapOf<Int, String>()
    /** I04/U08: guest rumble on physical controllers only (never the phone): each controller's own
     *  intensity (set in Test controllers), else the default the in-game menu changes. */
    private val rumbleSettings by lazy {
        mutableStateOf(xendroid.compose.gamepad.RumbleSettings.decode(
            getSharedPreferences(xendroid.compose.gamepad.RumbleSettings.PREFS, MODE_PRIVATE)
                .getString(xendroid.compose.gamepad.RumbleSettings.DEFAULT_KEY, null),
            getSharedPreferences(xendroid.compose.gamepad.RumbleSettings.DEVICES_PREFS, MODE_PRIVATE)
                .getString(xendroid.compose.gamepad.RumbleSettings.DEVICES_KEY, null)))
    }
    private val rumbleAmplitudes = mutableMapOf<Int, Int>()

    /**
     * I06–I09: other phones on the LAN as P2–P4. Off at every boot; the in-game menu turns it
     * on. Its callbacks run on socket threads: the slot table and the JNI input calls are
     * thread-safe (the native driver locks its key table), the flight recorder is not.
     */
    private val phoneControllers = xendroid.compose.companion.CompanionHostControl(
        newHost = { lan ->
            xendroid.compose.companion.CompanionHost(lan.address,
                claimSlot = { key, _ ->
                    // A generic label: a phone's own name can carry its owner's.
                    controllerSlots.connectRemote(key)?.also { session.setSlotConnected(it, true, "Phone P${it + 1}") }
                },
                releaseSlot = { key, slot ->
                    if (controllerSlots.disconnect(key) != null) session.setSlotConnected(slot, false, "Phone P${slot + 1}")
                },
                onInput = { slot, change -> session.keyEventSlot(slot, change.key, change.pressed, change.value) },
                onEvent = { message -> mainHandler.post { recordEvent("companion", message) } })
        },
        onChange = { mainHandler.post { refreshPhoneControllers() } },
    )
    /** Null until the first refresh (resources are not ready while the activity is constructed). */
    private val phoneControllersLabel = mutableStateOf<String?>(null)
    private val phoneControllersDetails = mutableStateOf<String?>(null)
    private var phoneControllersRecorded = "off"

    /** Menu texts; on, off and failures also go to the run's timeline (never the address or code). */
    private fun refreshPhoneControllers() {
        phoneControllersLabel.value = xendroid.compose.ui.ingame.phoneControllersLabel(this, phoneControllers.status)
        phoneControllersDetails.value = xendroid.compose.ui.ingame.phoneControllersDetails(this, phoneControllers.status)
        val status = when (val s = phoneControllers.status) {
            is xendroid.compose.companion.CompanionHostControl.Status.On -> "on (${s.network})"
            is xendroid.compose.companion.CompanionHostControl.Status.Failed -> "not started: ${s.reason}"
            xendroid.compose.companion.CompanionHostControl.Status.Off -> "off"
            else -> return
        }
        if (status != phoneControllersRecorded) {
            phoneControllersRecorded = status
            recordEvent("companion", "phone controllers $status")
        }
    }

    /** Plays each slot's guest rumble on the controller holding it; called every 50 ms while the game runs. */
    private fun driveRumble(state: LongArray) {
        controllerSlots.players.forEachIndexed { slot, key ->
            val deviceId = key?.let { k -> controllerKeys.entries.firstOrNull { it.value == k }?.key } ?: return@forEachIndexed
            val amplitude = xendroid.compose.gamepad.rumbleAmplitude(state.getOrElse(slot) { 0L }, rumbleSettings.value.forDevice(key))
            val vibrator = controllerVibrator(deviceId) ?: return@forEachIndexed
            if (amplitude > 0) {
                // Short overlapping shots: stops by itself if this loop does.
                vibrator.vibrate(VibrationEffect.createOneShot(100, amplitude))
            } else if ((rumbleAmplitudes[deviceId] ?: 0) > 0) {
                vibrator.cancel()
            }
            rumbleAmplitudes[deviceId] = amplitude
        }
    }

    private fun stopRumble() {
        rumbleAmplitudes.keys.toList().forEach { id -> if ((rumbleAmplitudes[id] ?: 0) > 0) controllerVibrator(id)?.cancel() }
        rumbleAmplitudes.clear()
    }

    private fun controllerVibrator(deviceId: Int): Vibrator? {
        val device = InputDevice.getDevice(deviceId) ?: return null
        val vibrator = if (Build.VERSION.SDK_INT >= 31) device.vibratorManager.defaultVibrator
        else @Suppress("DEPRECATION") device.vibrator
        return vibrator.takeIf { it.hasVibrator() }
    }
    private val thermalListener = android.os.PowerManager.OnThermalStatusChangedListener { noteThermal(it) }
    private val inputDeviceListener = object : android.hardware.input.InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = noteController(deviceId, "connected")
        override fun onInputDeviceRemoved(deviceId: Int) {
            controllers.remove(deviceId)?.let { recordEvent("controller", "disconnected ($it)", flush = true) }
            if (deviceId == touchHiddenBy) touchControlsBack("the controller disconnected")
            // Release only what the lost controller held (I04); its player slot frees up.
            controllerKeys.remove(deviceId)?.let { key ->
                controllerSlots.disconnect(key)?.let { slot ->
                    if (slot == 0) releaseGuestInput() else {
                        slotInput.release(slot)
                        session.setSlotConnected(slot, false, "Controller ${slot + 1}")
                    }
                }
            }
        }
        override fun onInputDeviceChanged(deviceId: Int) {}
    }
    /** Set when the launch failed visibly, so the run is recorded as FAILED. */
    private var launchFailure: String? = null
    /** The game this single-shot process boots; a later launch intent cannot switch it. */
    private var launchedGame: String? = null

    /** Lote 7: the disc mounted by the last swap (the launched one before any), marked in the next swap. */
    private var discInDrive: String? = null
    private var surfaceView: SurfaceView? = null
    private var surfaceAvailable = false
    private var externalDisplay: ExternalGameDisplay? = null
    /** Null until the TV output reports (it starts on the phone). */
    private val externalDisplayLabel = mutableStateOf<String?>(null)
    /** 15h: the TV margin (overscan), percent of the screen per side. */
    private val tvMargin = androidx.compose.runtime.mutableFloatStateOf(0f)
    /** The game is drawn on another display (the TV); the handset keeps only the controls. */
    private val gameOnExternalDisplay = mutableStateOf(false)
    /** 15c: a horizontal fold half open (tabletop), as (top, bottom) in window pixels. */
    private val windowFold = mutableStateOf<Pair<Int, Int>?>(null)
    /** 15c: the split last written to the run's timeline. */
    private var splitRecorded: String? = null
    private val scalingEffect = mutableIntStateOf(-1)
    /** The Image options tried from the in-game menu (each -1: the game's Settings value). */
    private val imageTuning = mutableStateOf(xendroid.compose.core.ImageTuning())
    private val performanceHints by lazy { PresenterPerformanceHints(applicationContext) }
    private val performanceHintsLabel = mutableStateOf("Presenter ADPF · Off")
    private var started = false

    private val mainHandler = Handler(Looper.getMainLooper())
    private val pauseOnFocusLost =
        Runnable {
            pauseForLifecycle()
        }

    private val gamepad by lazy {
        GamepadController(applicationContext)
    }

    private val brightnessSampler = ScreenBrightnessSampler()

    @Volatile
    private var overlayWantsBrightness = false

    private var hapticsEnabled = false

    private val bootedState = mutableStateOf(false)
    private val foregroundState = mutableStateOf(false)
    private val fpsLimitState = mutableIntStateOf(60)
    private val showFpsOverlay = mutableStateOf(false)
    private val performanceOverlayEnabled = mutableStateOf(false)
    /** 15n: FPS only, the chosen rows, or the performance panel. */
    private val hudDetail = mutableStateOf(xendroid.compose.core.HudDetail.FULL)
    /** 15n: the last seconds of frame times, and what the panel shows (built only while it is shown). */
    private val performancePanel = xendroid.compose.core.PerformancePanel()
    private val panelSnapshot = mutableStateOf<xendroid.compose.core.PerformancePanel.Snapshot?>(null)
    /** The settings this run booted with that differ from the defaults (14c), for the panel. */
    @Volatile private var bootChangedSettings: List<String>? = null
    private val hudMetrics = mutableStateOf(HudMetric.entries.toSet())
    /** 15g: the HUD's look for the running game (or the last one set). */
    private val hudLook = mutableStateOf(xendroid.compose.core.HudLook.BOX)
    /** Round 2: the HUD's layout, edge, opacity, colours and graph (every game). */
    private val hudStyle = mutableStateOf(xendroid.compose.core.HudStyle())
    /** Round 2: this game's HUD size as the menu set it (null: the pinch's own). */
    private val hudScale = mutableStateOf<Float?>(null)
    /** Round 2: the in-game menu's own preferences (pause on open, keep changes for the game). */
    private val inGamePrefs by lazy { InGamePrefs(getSharedPreferences(InGamePrefs.FILE, MODE_PRIVATE)) }
    private val pauseOnOpen = mutableStateOf(true)
    private val autoSave = mutableStateOf(true)
    /** Round 2: this session's changes for the running game; reads and writes on [changesThread]. */
    private var inGameChanges: InGameChanges? = null
    private val changesThread = Dispatchers.IO.limitedParallelism(1)
    // Not the activity's scope: a change made just before leaving the game still reaches the file.
    private val changesScope = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.SupervisorJob() + changesThread)
    private val sessionChanges = mutableIntStateOf(0)
    /** Round 2: the config keys this game has its own value for (the menu's "this game" tags). */
    private val gameOwnKeys = mutableStateOf<Set<String>>(emptySet())
    /** The in-game menu's GPU options as the core runs them (read at boot, then the menu's). */
    private val gpuLive = mutableStateOf<Map<xendroid.compose.core.GpuLiveOption, Int>>(emptyMap())
    /** Round 2: each changed setting's live value before the session's first change of it, for Undo. */
    private val liveBefore = mutableMapOf<String, Any?>()
    /** Round 2: the touch controls' look (the layout file's globals, mirrored for the menu's actions). */
    private val controlStyle = mutableStateOf(xendroid.compose.gamepad.ControlStyle.MODERN)

    // Fullscreen stretch:
    // false = preserve aspect ratio / black bars
    // true  = stretch game image to fill the display
    private val fullscreenStretchEnabled = mutableStateOf(false)

    private val showTouchOverlay = mutableStateOf<Boolean?>(null)
    /** The on-screen controls put aside by P1's physical controller (main thread only). */
    private val touchPresence = xendroid.compose.gamepad.TouchOverlayPresence()
    private val overlayHiddenByController = mutableStateOf(false)
    private var touchHiddenBy = -1
    /** The touch layout's "Hide while a controller plays P1", read outside composition. */
    private var hideTouchWithController = true
    private val menuState = mutableStateOf(InGameMenuState())
    private val menuPaused = mutableStateOf(false)
    private val editorOpen = mutableStateOf(false)
    private val menuLogSessions = mutableStateOf<List<SessionLogs.Session>>(emptyList())
    private val presentationState = mutableStateOf(PresentationState())
    private val fgGovernor = xendroid.compose.core.FrameGenerationGovernor()
    private val fgBudgetLabel = mutableStateOf<String?>(null)
    /** FG before it is on (what it would cost here) or while it runs (base → submitted). */
    private val fgNotes = mutableStateOf<List<String>>(emptyList())
    /** The guest's FPS in the last second it ran with the menu closed (the menu pauses it). */
    private var lastGuestFps = 0.0
    private var lastGenerated = -1L
    private var lastGeneratedNs = 0L
    private var generatedPerSecond = 0.0
    private val fgPreset = mutableIntStateOf(2)
    private val lsfgMultiplier = mutableIntStateOf(2)
    /** 15d: [xendroid.compose.core.FrameGenerationTarget] choice; off = the multiplier above, by hand. */
    private val lsfgTarget = mutableIntStateOf(xendroid.compose.core.FrameGenerationTarget.OFF)
    private val generationCap = xendroid.compose.core.GenerationCap()
    private val audioVolume = mutableIntStateOf(100)
    private val gpuLabel = mutableStateOf("")
    /** U01: the driver setting when the game started, and the Graphics tab's driver line. */
    private val driverAtBoot = mutableStateOf<String?>(null)
    private val driverLine = mutableStateOf(xendroid.compose.driver.DriverIdentity.InGame.UNKNOWN to "")
    private var volumeBeforeMute = 100
    private val backgroundPolicy = mutableStateOf(BackgroundPolicy.AUTO)
    private val gyroEnabled = mutableStateOf(false)
    private val gyroSensitivity = mutableIntStateOf(1)
    private val sustainedMode = mutableStateOf(false)
    private val sustainedAvailable = mutableStateOf(true)
    private val requestedRefresh = mutableStateOf<Float?>(null)
    /** Right-stick directions currently held on the touch overlay. */
    private val touchStickHeld = BooleanArray(24)
    /** When the gyroscope aims: always, or while P1 holds LT or LB (touch_options "gyro_aim"). */
    private val gyroAim = mutableStateOf(xendroid.compose.gamepad.GyroAim.ALWAYS)
    private var gyroAiming = false
    /** Controller sticks (and each touch gesture) delivered as they arrive, not once a frame. */
    private val unbufferedInput = mutableStateOf(true)
    private val gyroCamera by lazy { GyroCamera(applicationContext) { code, down, value ->
        // Hold-to-aim: with the chosen button up the camera stays still; the stick the
        // gyroscope was moving is let go once, never one a finger or a controller holds.
        if (!gyroAim.value.active(session::p1Holding)) {
            if (gyroAiming) {
                gyroAiming = false
                for (key in KC_RTHUMB_LEFT..KC_RTHUMB_DOWN) {
                    if (!axisPressed[key] && !touchStickHeld[key]) session.keyEvent(key, false, 0)
                }
            }
            return@GyroCamera
        }
        gyroAiming = true
        // A physical or touch right stick keeps priority over the optional camera sensor:
        // the sensor re-sends every sample, which would zero a held touch stick.
        if (!(KC_RTHUMB_LEFT..KC_RTHUMB_DOWN).any { axisPressed[it] || touchStickHeld[it] }) {
            session.keyEvent(code, down, value)
        }
    } }
    /** The input options every game starts with; the in-game menu changes them for the next ones too. */
    private val controlOptions by lazy { xendroid.compose.gamepad.ControlOptionsStore(applicationContext) }

    private fun applyControlOptions(options: xendroid.compose.gamepad.ControlOptions) {
        touchCamera.value = options.touchCamera
        gyroAim.value = options.gyroAim
        gyroSensitivity.intValue = options.gyroSensitivity.ordinal
        gyroCamera.sensitivity = options.gyroSensitivity.scale
        val unbuffered = unbufferedInput.value != options.unbufferedInput
        unbufferedInput.value = options.unbufferedInput
        if (unbuffered && window?.decorView?.isAttachedToWindow == true) applyUnbufferedInput()
        if (rumbleSettings.value.default != options.rumble) rumbleSettings.value = rumbleSettings.value.copy(default = options.rumble)
        val gyro = options.gyroCamera && gyroCamera.available
        if (gyro != gyroEnabled.value) {
            gyroEnabled.value = gyro
            if (gyro && foregroundState.value && !menuState.value.open && hasWindowFocus()) gyroCamera.start()
        }
    }

    /** Saves an in-game change for the next games (the shared store, never this process's preferences). */
    private fun saveControlOptions(change: (xendroid.compose.gamepad.ControlOptions) -> xendroid.compose.gamepad.ControlOptions) {
        lifecycleScope.launch {
            runCatching { controlOptions.update(change) }.onFailure { Log.w(TAG, "Saving the control options failed", it) }
        }
    }
    private val lsfgCache = mutableStateOf<String?>(null)
    private var importingLsfg = false
    private val lsfgPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null && !importingLsfg) {
            importingLsfg = true
            lifecycleScope.launch {
                try {
                    val cache = LsfgAssets.import(applicationContext, uri)
                    lsfgCache.value = cache.path
                    Toast.makeText(this@EmulatorHostActivity, "LSFG shaders imported locally", Toast.LENGTH_LONG).show()
                } catch (e: Exception) {
                    if (e is kotlinx.coroutines.CancellationException) throw e
                    Toast.makeText(this@EmulatorHostActivity, "LSFG import failed: ${e.message}", Toast.LENGTH_LONG).show()
                } finally { importingLsfg = false }
            }
        }
    }
    private val fpsConfig = mutableStateOf(FpsConfigSnapshot())
    private val adaptiveSticks = mutableStateOf(false)
    /** U07: the free right side of the screen drives the right stick by finger speed (opt-in, all games). */
    private val touchCamera = mutableStateOf(false)
    /** C07: scene markers placed in this run (for "Compare runs"). */
    private val sceneMarkers = mutableIntStateOf(0)
    private var controlsTitleId: String? = null
    /** The title the core reports running, for per-game touch layouts (U06). */
    private val activeTitleState = mutableStateOf<String?>(null)
    private val inGameConfig by lazy { InGameConfigRepository(ConfigStore(applicationContext)) }
    private var pausedByLifecycle = false
    private val consumedMenuKeys = mutableSetOf<Long>()
    private val sentGuestKeys = mutableMapOf<Long, Int>()
    private var sharingLogs = false

    private val keyboardRequestState =
        mutableStateOf<Emulator.KeyboardRequest?>(null)

    private val discRequestState =
        mutableStateOf<Emulator.DiscSwapRequest?>(null)

    private val messageBoxRequestState =
        mutableStateOf<Emulator.MessageBoxRequest?>(null)

    private val panelSelectedState = mutableIntStateOf(0)
    /** U10: the guest's text prompt as typed so far (grid highlight, caret, page, shift). */
    private val keyboardGrid = mutableStateOf(xendroid.compose.ui.keyboard.KeyboardGrid())
    private val gridRepeat = xendroid.compose.gamepad.NavRepeat()
    private var gridHatDirection = 4   // (dy + 1) * 3 + (dx + 1); 4 = centred

    @Volatile
    private var keyMap: Map<Int, Int> =
        GameButtons.DEFAULT_LOOKUP

    private var vibrator: Vibrator? = null
    private var lTriggerDown = false
    private var rTriggerDown = false

    /** 15l: the UI scale chosen in the library, for the in-game menu and panels (read per game). */
    private var uiScale = xendroid.compose.ui.theme.UiScale.DEFAULT

    /** 15l: before Android 13 the language chosen in the app is applied here. */
    override fun attachBaseContext(newBase: android.content.Context) {
        super.attachBaseContext(newBase)
        xendroid.compose.settings.AppLanguageStore.overrideFor(newBase)?.let { applyOverrideConfiguration(it) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        uiScale = xendroid.compose.ui.theme.UiScaleStore.read(this)
        // L02: the interface mode chosen in the library; read once per game process.
        menuState.value = menuState.value.copy(
            developer = xendroid.compose.settings.UiModeStore.read(this) == xendroid.compose.settings.UiMode.DEVELOPER)
        // What the game starts with for input (the Controls area's values): older builds' at once,
        // then the shared store's as soon as it is read.
        applyControlOptions(xendroid.compose.gamepad.ControlOptions.legacy(getSharedPreferences("touch_options", MODE_PRIVATE)))
        lifecycleScope.launch {
            runCatching { controlOptions.options.first() }
                .onFailure { Log.w(TAG, "Reading the control options failed", it) }
                .getOrNull()?.let(::applyControlOptions)
        }
        enterImmersiveMode()
        gameModeSignal.update(xendroid.compose.core.GamePhase.LOADING)
        watchFold()
        loadingName = intent?.getStringExtra(EXTRA_GAME_NAME)?.take(120)
        loadingArt.value = xendroid.compose.core.LaunchArt.fromExtra(intent?.getStringExtra(EXTRA_GAME_ART), listOf(filesDir, cacheDir))

        EmuProcessLink.bindToMainProcess(this)

        val gameUri =
            FrontendLaunch.resolveGamePath(
                this,
                intent
            )

        if (gameUri.isNullOrEmpty()) {
            Log.e(
                TAG,
                "No bootable game in launch intent; finishing"
            )

            Toast.makeText(
                this,
                getString(R.string.host_no_game),
                Toast.LENGTH_LONG
            ).show()

            leave()
            return
        }

        if (!EmulatorRuntime.supportsVulkan) {
            Toast.makeText(
                this,
                getString(R.string.host_no_vulkan),
                Toast.LENGTH_LONG
            ).show()

            leave()
            return
        }
        launchedGame = gameUri
        startEventSources()

        lifecycleScope.launch {
            val store = KeymapStore(applicationContext)

            keyMap =
                withContext(Dispatchers.IO) {
                    EmulatorRuntime.ensureLoaded()

                    store.androidToGameKey.firstOrNull()
                        ?: GameButtons.DEFAULT_LOOKUP
                }

            showFpsOverlay.value =
                withContext(Dispatchers.IO) {
                    readShowDebugOverlay()
                }

            performanceOverlayEnabled.value =
                getSharedPreferences(
                    "fps_overlay",
                    MODE_PRIVATE
                ).getBoolean(
                    "performance_overlay_enabled",
                    false
                )

            hudDetail.value = getSharedPreferences("fps_overlay", MODE_PRIVATE).let { prefs ->
                xendroid.compose.core.HudDetail.parse(prefs.getString("performance_overlay_detail", null),
                    prefs.getBoolean("performance_overlay_compact", false))
            }
            val overlayPrefs = getSharedPreferences("fps_overlay", MODE_PRIVATE)
            val savedMetrics = overlayPrefs.getStringSet("hud_metrics", null)?.let { saved ->
                // The power row became three (power, charge, battery time): who had it keeps seeing all of it.
                if (overlayPrefs.getBoolean("hud_power_split", false)) saved else {
                    val split = if (HudMetric.POWER.name in saved) saved + setOf(HudMetric.BATTERY_LEVEL.name, HudMetric.BATTERY_TIME.name) else saved
                    overlayPrefs.edit().putStringSet("hud_metrics", split).putBoolean("hud_power_split", true).apply()
                    split
                }
            }
            if (savedMetrics != null) hudMetrics.value = HudMetric.entries.filter { it.name in savedMetrics }.toSet()
            hudLook.value = xendroid.compose.core.HudPlacements.read(HudPreferences.of(this@EmulatorHostActivity), null).look
            hudStyle.value = xendroid.compose.core.HudStyle.read(HudPreferences.of(this@EmulatorHostActivity))
            pauseOnOpen.value = inGamePrefs.pauseOnOpen
            autoSave.value = inGamePrefs.autoSave

            fullscreenStretchEnabled.value =
                getSharedPreferences(
                    DISPLAY_SETTINGS_PREFS,
                    MODE_PRIVATE
                ).getBoolean(
                    FULLSCREEN_STRETCH_KEY,
                    withContext(Dispatchers.IO) {
                        readFullscreenStretch()
                    }
                )

            if (
                Build.VERSION.SDK_INT < Build.VERSION_CODES.R ||
                !Environment.isExternalStorageManager()
            ) {
                Toast.makeText(
                    this@EmulatorHostActivity,
                    getString(R.string.host_no_files_access),
                    Toast.LENGTH_LONG
                ).show()

                leave()
                return@launch
            }

            withContext(Dispatchers.IO) {
                runCatching {
                    SessionLogs.ensureCaptureRunning()
                }.onFailure {
                    Log.w(
                        TAG,
                        "Session log capture failed",
                        it
                    )
                }
            }

            // Lease + save-restore recovery before the guest can touch the content tree.
            // A failure is shown to the user instead of a silent finish().
            val storage = withContext(Dispatchers.IO) {
                runCatching {
                    session.prepareStorage(onWaiting = {
                        lifecycleScope.launch {
                            Toast.makeText(this@EmulatorHostActivity,
                                getString(R.string.host_waiting_storage), Toast.LENGTH_SHORT).show()
                        }
                    })
                }.getOrElse {
                    if (it is kotlinx.coroutines.CancellationException) throw it
                    Log.e(TAG, "Preparing game storage failed", it)
                    EmulatorSession.StorageResult.Unavailable(EmulatorSession.StorageResult.Unavailable.Reason.FAILED,
                        it.message ?: it.javaClass.simpleName, "Game data could not be prepared: ${it.message}")
                }
            }
            if (storage is EmulatorSession.StorageResult.Unavailable) {
                showLaunchFailure(when (storage.reason) {
                    EmulatorSession.StorageResult.Unavailable.Reason.BUSY -> getString(R.string.host_storage_busy)
                    EmulatorSession.StorageResult.Unavailable.Reason.RECOVERY_PENDING -> getString(R.string.host_storage_recovery, storage.detail)
                    EmulatorSession.StorageResult.Unavailable.Reason.FAILED -> getString(R.string.host_storage_failed, storage.detail)
                }, storage.english, when (storage.reason) {
                    EmulatorSession.StorageResult.Unavailable.Reason.BUSY -> xendroid.compose.ui.ingame.LaunchFailure.Kind.BUSY
                    EmulatorSession.StorageResult.Unavailable.Reason.RECOVERY_PENDING -> xendroid.compose.ui.ingame.LaunchFailure.Kind.RECOVERY
                    EmulatorSession.StorageResult.Unavailable.Reason.FAILED -> xendroid.compose.ui.ingame.LaunchFailure.Kind.OTHER
                })
                return@launch
            }
            recordEvent("storage", "ready")
            // U09: Cancel can come during any wait above; a start cancelled this early leaves
            // no run and boots nothing (the activity is already finishing).
            if (bootCancelled) return@launch

            // Runs left open by a :emu that died (crash, kill) are closed first, then this
            // run starts. Frontend-only launches never start the main process to do it.
            runId = withContext(Dispatchers.IO) {
                runCatching {
                    val runs = xendroid.compose.sessions.SessionRuns.store()
                    runs.reconcile(xendroid.compose.sessions.SessionRuns.fates(applicationContext))
                    runs.begin(origin, gameUri, BuildConfig.VERSION_NAME, Process.myPid()).runId
                }.onFailure { Log.w(TAG, "Session run record unavailable", it) }.getOrNull()
            }
            recordEvent("boot", "run started", flush = true)
            recordEvent("game mode", "system: ${gameModeSignal.systemMode()}")
            recordEvent("input", "unbuffered " + if (unbufferedInput.value) "on" else "off")
            if (bootCancelled) {
                // Cancelled while the run record was being written: cancelBoot had no run to end.
                runId?.let { id -> runCatching { xendroid.compose.sessions.SessionRuns.store().ending(id, "cancelled while starting") } }
                return@launch
            }
            // A fatal core error (GPU device lost...) aborts without UI: its message goes
            // beside the run record so the next reconcile can name the cause.
            runId?.let { id ->
                runCatching { session.setFatalReportPath(xendroid.compose.sessions.SessionRuns.store().fatalReportFile(id).path) }
                    .onFailure { Log.w(TAG, "Fatal report path unavailable", it) }
            }

            prepareNativeRealPath(gameUri)

            // Re-apply the saved display setting immediately before the
            // emulator session is allowed to boot.
            withContext(Dispatchers.IO) {
                persistFullscreenStretchConfig(
                    fullscreenStretchEnabled.value
                )
            }

            if (bootCancelled) return@launch
            installSurfaceView()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        recordEvent("focus", if (hasFocus) "gained" else "lost")

        if (hasFocus) {
            mainHandler.removeCallbacks(
                pauseOnFocusLost
            )

            enterImmersiveMode()

            resumeForLifecycle()
        } else {
            mainHandler.removeCallbacks(
                pauseOnFocusLost
            )

            mainHandler.postDelayed(
                pauseOnFocusLost,
                FOCUS_PAUSE_DEBOUNCE_MS
            )
        }
    }

    /** Blocking explanation before leaving: a Toast is easy to miss on a launch from an
     *  external frontend, and the user needs to know the game did not start and why. */
    /** singleTask: a frontend or shortcut launching while a game runs lands here. The
     *  core is single-shot per process, so say why the requested game did not start
     *  instead of silently showing the running one. */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        val requested = FrontendLaunch.resolveForHandOff(this, intent) ?: return
        if (requested != launchedGame) {
            Toast.makeText(this, getString(R.string.host_already_running),
                Toast.LENGTH_LONG).show()
        }
    }

    /** U09: where this game was started from, read once (the run record and the way out). */
    private val origin by lazy { launchSource() }

    /** Leaves the way [GameExit] says for [origin]; the process ends in onDestroy. */
    private fun leave() {
        when (xendroid.compose.core.GameExit.way(origin, isTaskRoot)) {
            xendroid.compose.core.GameExit.Way.FINISH -> finish()
            xendroid.compose.core.GameExit.Way.FINISH_AND_REMOVE_TASK -> finishAndRemoveTask()
            xendroid.compose.core.GameExit.Way.TASK_TO_BACK_THEN_FINISH -> {
                moveTaskToBack(true)
                finish()
            }
        }
    }

    /** U09: the player gave up waiting for the game to start. */
    private fun cancelBoot() {
        bootCancelled = true
        runId?.let { id -> runCatching { xendroid.compose.sessions.SessionRuns.store().ending(id, "cancelled while starting") } }
        recordEvent("exit", "cancelled while starting", flush = true)
        leave()
    }

    /** Where this launch came from, as far as Android tells us (local record only). */
    private fun launchSource(): String {
        val origin = referrer?.host
        return when {
            origin == packageName -> "library"
            intent?.action == Intent.ACTION_VIEW && origin == null -> "shortcut"
            origin != null -> "external:$origin"
            else -> "external"
        }
    }

    /** Flight recorder entry (C01). [flush] writes the log now, for events a crash or kill may follow. */
    private fun recordEvent(kind: String, detail: String = "", flush: Boolean = false) {
        runEvents.record(kind, detail)
        if (flush) flushRunEvents()
    }

    private fun flushRunEvents() {
        val id = runId ?: return
        val log = runEvents.snapshotIfChanged() ?: return
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching { xendroid.compose.sessions.SessionRuns.store().saveEvents(id, log) }
                .onFailure { Log.w(TAG, "Saving the run events failed", it) }
        }
    }

    /** Thermal status and controller changes for the flight recorder (main thread). */
    private fun startEventSources() {
        runCatching {
            getSystemService(android.os.PowerManager::class.java)?.let { power ->
                noteThermal(power.currentThermalStatus)
                power.addThermalStatusListener(mainExecutor, thermalListener)
            }
        }.onFailure { Log.w(TAG, "Thermal status unavailable", it) }
        runCatching {
            InputDevice.getDeviceIds().forEach { noteController(it, "present") }
            getSystemService(android.hardware.input.InputManager::class.java)
                ?.registerInputDeviceListener(inputDeviceListener, mainHandler)
        }.onFailure { Log.w(TAG, "Input device events unavailable", it) }
    }

    private fun stopEventSources() {
        runCatching { getSystemService(android.os.PowerManager::class.java)?.removeThermalStatusListener(thermalListener) }
        runCatching { getSystemService(android.hardware.input.InputManager::class.java)?.unregisterInputDeviceListener(inputDeviceListener) }
    }

    /** 15j: every other second, Android's thermal headroom (10 s ahead) against the device's own
     *  limit; the timeline gets each change and the player one notice per rise, five minutes apart. */
    private fun watchThermalHeadroom() {
        if (thermalTick++ % 2 != 0) return
        val headroom = if (Build.VERSION.SDK_INT >= 30) {
            runCatching { getSystemService(android.os.PowerManager::class.java)?.getThermalHeadroom(10) }.getOrNull()
        } else null
        lastHeadroom = headroom?.takeIf { it.isFinite() && it >= 0f }
        val level = thermalWatch.sample(headroom, lastThermalStatus) ?: return
        recordEvent("thermal watch", level.name.lowercase().replace('_', ' ') +
            (headroom?.takeIf { it.isFinite() }?.let { " (headroom %.2f)".format(java.util.Locale.ROOT, it) } ?: ""))
        val now = android.os.SystemClock.elapsedRealtime()
        if (level != xendroid.compose.core.ThermalWatch.Level.OK && foregroundState.value &&
            (thermalNoticeAt == Long.MIN_VALUE || now - thermalNoticeAt >= xendroid.compose.core.ThermalWatch.NOTICE_EVERY_MS)) {
            thermalNoticeAt = now
            Toast.makeText(this, getString(if (level == xendroid.compose.core.ThermalWatch.Level.THROTTLING)
                R.string.host_thermal_throttling else R.string.host_thermal_near), Toast.LENGTH_LONG).show()
        }
    }

    /** 15n: what the performance panel shows this second; settings values in the app's language. */
    private fun buildPanelSnapshot(fg: PresentationState): xendroid.compose.core.PerformancePanel.Snapshot {
        val run = runPerformance.snapshot()
        val changed = bootChangedSettings
        val (shown, total) = xendroid.compose.core.PerformancePanel.changedSettings(changed)
        val effect = scalingEffect.intValue
        val image = listOfNotNull(
            if (effect < 0) getString(R.string.menu_scaling_inherited)
            else listOf("Bilinear", "CAS", "FSR", "SGSR", "Lanczos", "CRT").getOrNull(effect),
            xendroid.compose.core.PerformancePanel.resolutionScale(changed)?.let { getString(R.string.panel_resolution, it) },
        ).joinToString(" · ")
        val frameGeneration = when {
            !fg.requested -> getString(R.string.menu_off)
            fg.engine == 1 -> "LSFG ×${fg.multiplier}"
            else -> "Win-FG"
        }
        val limit = fpsLimitState.intValue.let { if (it == 0) getString(R.string.menu_unlimited) else it.toString() } +
            " · " + getString(R.string.panel_screen_hz, currentOutputHz().roundToInt().toString())
        val mode = listOfNotNull(getString(R.string.panel_sustained).takeIf { sustainedMode.value },
            performanceHintsLabel.value).joinToString(" · ")
        val driver = runCatching { session.activeDriverIdentity()?.label }.getOrNull() ?: gpuLabel.value.ifEmpty { "?" }
        return xendroid.compose.core.PerformancePanel.Snapshot(
            recentSeconds = performancePanel.recentSeconds,
            recent = performancePanel.recent(),
            run = xendroid.compose.core.PerformancePanel.pacing(run.frameTimeHistogramMs),
            fpsMedian = run.fpsPercentile(0.5),
            fpsLow = run.fpsPercentile(0.05),
            pipelines = run.pipelineCreations,
            pipelineMs = run.pipelineCreationMs,
            audioConcealed = run.audioConcealedBlocks,
            audioBlocks = run.audioBlocks,
            thermal = thermalWatch.level,
            headroom = lastHeadroom,
            settings = listOf(
                xendroid.compose.core.PerformancePanel.Setting.DRIVER to driver,
                xendroid.compose.core.PerformancePanel.Setting.SCALING to image,
                xendroid.compose.core.PerformancePanel.Setting.FRAME_GENERATION to frameGeneration,
                xendroid.compose.core.PerformancePanel.Setting.FPS_LIMIT to limit,
                xendroid.compose.core.PerformancePanel.Setting.PERFORMANCE_MODE to mode,
            ),
            changed = shown,
            changedTotal = total,
        )
    }

    private fun noteThermal(status: Int) {
        if (status == lastThermalStatus) return
        lastThermalStatus = status
        recordEvent("thermal", xendroid.compose.sessions.thermalStatusName(status),
            flush = status >= android.os.PowerManager.THERMAL_STATUS_SEVERE)
    }

    private fun noteController(deviceId: Int, what: String) {
        val device = InputDevice.getDevice(deviceId) ?: return
        // The app's own check: a phone's built-in keys must not take player 1 from a real pad.
        if (!xendroid.compose.ui.design.Gamepads.isController(device)) return
        // IDs only: a Bluetooth name can carry its owner's name.
        val id = "vendor 0x%04X product 0x%04X".format(device.vendorId, device.productId)
        controllers[deviceId] = id
        val key = device.descriptor ?: "device:$deviceId"
        controllerKeys[deviceId] = key
        val slot = controllerSlots.connect(key)
        recordEvent("controller", "$what ($id) " + (slot?.let { "as P${it + 1}" } ?: "with no free player slot"))
        if (slot != null && slot > 0) session.setSlotConnected(slot, true, "Controller ${slot + 1}")
    }

    /** A press, or a push past half, from a physical controller: playing P1, it puts the
     *  on-screen controls aside (TouchOverlayPresence). Virtual devices never count. */
    private fun noteControllerUse(event: android.view.InputEvent) {
        if (overlayHiddenByController.value) return
        // Only a real controller: a phone's own "gamepad" keys must not hide the touch controls.
        if (!xendroid.compose.ui.design.Gamepads.isController(event.device)) return
        if (touchPresence.controllerInput(hideTouchWithController, playerSlot(event.deviceId))) {
            touchHiddenBy = event.deviceId
            overlayHiddenByController.value = true
            recordEvent("touch controls", "hidden: a controller is playing P1")
        }
    }

    private fun touchControlsBack(why: String) {
        touchHiddenBy = -1
        if (touchPresence.controllerGone()) recordEvent("touch controls", "shown: $why")
        overlayHiddenByController.value = false
    }

    /** L06: P1's profile as the config names it (the core signed it in at boot); null = nobody. */
    private fun firstPlayerXuid(): String? {
        val handle = xendroid.compose.settings.ConfigStore(applicationContext).openLiveSnapshot()
        return try {
            handle.getString(xendroid.compose.data.ProfileSlots.SECTION, xendroid.compose.data.ProfileSlots.key(0))
                ?.trim()?.ifEmpty { null }
        } finally { handle.closeDiscard() }
    }

    /** The player a device plays as; anything not assigned (keyboards, unknown devices) is P1, as before. */
    private fun playerSlot(deviceId: Int): Int =
        controllerKeys[deviceId]?.let { controllerSlots.slotOf(it) } ?: 0

    /** Before boot the core ignores slot changes: tell it which players exist once it runs. */
    private fun syncControllerSlots() {
        controllerSlots.players.forEachIndexed { slot, key ->
            if (slot > 0 && key != null) session.setSlotConnected(slot, true, "Controller ${slot + 1}")
        }
    }

    /** Per-second flight recorder checks: presentation state changes and guest stalls. */
    private fun notePresentation(state: PresentationState, running: Boolean, presentCount: Long) {
        val label = state.stateLabel
        val previousLabel = lastPresentationLabel
        lastPresentationLabel = label
        if (previousLabel != null && label != previousLabel) recordEvent("presentation", label)
        val previousCount = lastPresentCount
        lastPresentCount = presentCount
        if (previousCount < 0) return
        if (running && presentCount == previousCount) {
            stalledSeconds++
            if (stalledSeconds == STALL_EVENT_SECONDS) recordEvent("stall", "no new frame for $STALL_EVENT_SECONDS s", flush = true)
        } else {
            if (stalledSeconds >= STALL_EVENT_SECONDS) recordEvent("stall", "frames resumed after $stalledSeconds s")
            stalledSeconds = 0
        }
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        // 10 = TRIM_MEMORY_RUNNING_LOW and above: the kill this may precede loses no events.
        recordEvent("memory", "trim level $level", flush = level >= 10)
    }

    /** [shown] in the dialog, in the shown language; [logged] (English) in the run's record.
     *  15e (DroidDeck `02f356f`): the dialog stays with Back, Try again and Share logs. */
    private fun showLaunchFailure(shown: String, logged: String = shown,
                                  kind: xendroid.compose.ui.ingame.LaunchFailure.Kind = xendroid.compose.ui.ingame.LaunchFailure.Kind.OTHER) {
        launchFailure = logged
        recordEvent("error", logged, flush = true)
        if (isFinishing || isDestroyed) return
        lifecycleScope.launch {
            // The core's last words, on screen only (never shared from here).
            val log = withContext(Dispatchers.IO) {
                runCatching {
                    java.io.File(Utils.get_log_file_path()).takeIf { it.isFile }?.let { f ->
                        java.io.RandomAccessFile(f, "r").use { raf ->
                            val from = (raf.length() - 4096).coerceAtLeast(0)
                            raf.seek(from)
                            val bytes = ByteArray((raf.length() - from).toInt())
                            raf.readFully(bytes)
                            String(bytes, Charsets.UTF_8).lines().map { it.trimEnd() }.filter { it.isNotBlank() }.takeLast(6)
                                .map { it.take(160) }
                        }
                    }
                }.getOrNull().orEmpty()
            }
            // A core that did not start on a custom driver may start on the system one.
            val customDriver = withContext(Dispatchers.IO) {
                runCatching { inGameConfig.driverPath(activeTitleState.value) }.getOrNull()?.isNotBlank() == true
            }
            val shownKind = if (kind == xendroid.compose.ui.ingame.LaunchFailure.Kind.CORE && customDriver)
                xendroid.compose.ui.ingame.LaunchFailure.Kind.DRIVER else kind
            launchFailureState.value = xendroid.compose.ui.ingame.LaunchFailure(shown, shownKind, log)
            if (failureView == null) {
                failureView = ComposeView(this@EmulatorHostActivity).apply {
                    setContent {
                        val failure = launchFailureState.value ?: return@setContent
                        val mode = xendroid.compose.ui.design.InputModeStore.read(this@EmulatorHostActivity)
                            .resolve(xendroid.compose.ui.design.rememberControllerConnected())
                        xendroidTheme(mode = mode) {
                            xendroid.compose.ui.ingame.LaunchFailureScreen(failure, loadingArt.value,
                                onBack = ::leave, onRetry = { tryAgain() }, onShareLogs = { shareSessionLogs(null) },
                                onRetrySystemDriver = { tryAgain(systemDriver = true) })
                        }
                    }
                }
                addContentView(failureView, android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT))
            }
        }
    }

    /** The failure screen's own view (it may come before the game's views exist). */
    private var failureView: ComposeView? = null

    /** Batch 2: the menu's status line: FPS now, p99 of the last seconds, battery, the driver. */
    private fun buildMenuStatus(): List<xendroid.compose.ui.ingame.MenuStat> = buildList {
        val fps = session.averageFps().takeIf { it > 0 } ?: lastGuestFps
        if (fps > 0) add(xendroid.compose.ui.ingame.MenuStat(fps.roundToInt().toString(), "FPS"))
        performancePanel.recent()?.let { add(xendroid.compose.ui.ingame.MenuStat(it.p99UnderMs.toString(), "ms", "p99")) }
        xendroid.compose.sessions.SessionRuns.batteryCelsius(applicationContext)?.let {
            add(xendroid.compose.ui.ingame.MenuStat("%.0f".format(it), "°C"))
        }
        val level = runCatching {
            (getSystemService(BATTERY_SERVICE) as android.os.BatteryManager).getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY)
        }.getOrNull()?.takeIf { it in 0..100 }
        level?.let { add(xendroid.compose.ui.ingame.MenuStat("$it%", label = getString(R.string.xd_menu_battery))) }
        runCatching { session.activeDriverIdentity() }.getOrNull()?.let { d ->
            add(xendroid.compose.ui.ingame.MenuStat(listOf(d.driverName, d.driverInfo).filter { it.isNotBlank() }.joinToString(" ")
                .ifBlank { d.label }.take(40)))
        }
    }

    /** 15e: the core is single-shot per process, so the main process starts the game again in a
     *  new :emu once this one is gone. */
    private fun tryAgain(systemDriver: Boolean = false) {
        recordEvent("exit", if (systemDriver) "trying again with the system driver" else "trying again", flush = true)
        val source = intent
        val relaunch = Intent(this, MainActivity::class.java).apply {
            action = ACTION_RELAUNCH_GAME
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            RELAUNCH_EXTRAS.forEach { key ->
                source?.getStringExtra(key)?.let { putExtra(key, it) }
                source?.getStringArrayExtra(key)?.let { putExtra(key, it) }
            }
            if (systemDriver) {
                // This launch's own options, plus the system driver; the chosen one stays saved.
                val own = if (xendroid.compose.core.LaunchToken.matches(filesDir, source?.getStringExtra(EXTRA_LAUNCH_TOKEN)))
                    source?.getStringArrayExtra(EXTRA_LAUNCH_ARGS).orEmpty().filterNot { it.startsWith("--vulkan_lib_path=") } else emptyList()
                putExtra(EXTRA_LAUNCH_ARGS, (own + "--vulkan_lib_path=").toTypedArray())
                putExtra(EXTRA_LAUNCH_TOKEN, xendroid.compose.core.LaunchToken.get(filesDir))
            }
            // A game another app handed over keeps its read grant through the relaunch.
            source?.getStringExtra(EXTRA_GAME_URI)?.takeIf { it.startsWith("content://") }?.let { uri ->
                clipData = android.content.ClipData.newRawUri("game", android.net.Uri.parse(uri))
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
        }
        runCatching { startActivity(relaunch) }.onFailure { Log.w(TAG, "Starting the game again failed", it) }
        leave()
    }

    private fun enterImmersiveMode() {
        // onWindowFocusChanged(true) fires on every transient system-bar peek, notification
        // shade dismiss, dialog close, etc. -- not just once per activity lifecycle. Reapplying
        // the insets/window flags on each of those forces a surface relayout
        // (surfaceChanged -> session.changeSurface, a synchronous native swapchain rebuild),
        // which was stalling the main thread for hundreds of ms to multiple seconds per call.
        // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE already re-hides transient bars on its own, so
        // this only needs to run once per window.
        if (immersiveModeApplied) return
        immersiveModeApplied = true

        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        )
        // A TV or monitor that supports it switches to its low-latency game mode (ALLM).
        if (Build.VERSION.SDK_INT >= 30) window.setPreferMinimalPostProcessing(true)

        WindowCompat.setDecorFitsSystemWindows(
            window,
            false
        )

        WindowInsetsControllerCompat(
            window,
            window.decorView
        ).apply {
            systemBarsBehavior =
                WindowInsetsControllerCompat
                    .BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

            hide(
                WindowInsetsCompat.Type.systemBars()
            )
        }
    }

    private fun readShowDebugOverlay(): Boolean =
        runCatching {
            val handle =
                ConfigStore(applicationContext)
                    .openLiveSnapshot()

            try {
                handle.getBool(
                    "Display",
                    "show_debug_overlay",
                    false
                )
            } finally {
                handle.closeDiscard()
            }
        }.getOrDefault(false)

    private fun readFullscreenStretch(): Boolean =
        runCatching {
            val handle =
                ConfigStore(applicationContext)
                    .openLiveSnapshot()

            try {
                !handle.getBool(
                    "Display",
                    "present_letterbox",
                    true
                )
            } finally {
                handle.closeDiscard()
            }
        }.getOrDefault(false)

    private fun persistFullscreenStretchConfig(
        enabled: Boolean
    ): Boolean =
        runCatching {
            ConfigStore(applicationContext).editLiveConfig { handle ->
                handle.putBool(
                    "Display",
                    "present_letterbox",
                    !enabled
                )
            }
        }.onFailure {
            Log.w(
                TAG,
                "persisting fullscreen stretch failed",
                it
            )
        }.isSuccess

    private fun prepareNativeRealPath(
        absPath: String
    ) {
        session.setupContext(this)
        session.setupGamePathReal(absPath)

        session.setupLaunchArgs(
            arrayOf(
                "--storage_root=" +
                    Utils.get_storage_root_path(),

                "--config=" +
                    Application
                        .get_global_config_file()
                        .absolutePath,

                "--log_file=" +
                    Utils.get_log_file_path(),

                "--log_append=true",
            ) + launchOptionArgs().also { launchArgsUsed = it }.toTypedArray()
        )

        session.setupUriInfoListFile(
            Application
                .get_uri_info_list_file()
                .absolutePath
        )
    }

    /** The options this launch passed to the core (R4), for the loading screen. */
    private var launchArgsUsed: List<String> = emptyList()

    /** Batch 2: what the loading screen says about the launch once the title is known. */
    private fun loadingDetailsOf(title: String): xendroid.compose.ui.ingame.LoadingDetails {
        val own = runCatching {
            val handle = ConfigStore(applicationContext).openGameConfig(title)
            try { xendroid.compose.settings.SettingsSchema.allSettings.count { handle.getString(it.section, it.name) != null } }
            finally { handle.closeDiscard() }
        }.getOrDefault(0)
        val driver = runCatching { xendroid.compose.ui.settings.driverName(this, inGameConfig.driverPath(title)) }.getOrNull()
        val limit = runCatching { session.fpsLimit() }.getOrNull()?.let {
            if (it == 0) getString(R.string.menu_unlimited) else getString(R.string.xd_boot_limit, it)
        }
        val profile = runCatching {
            firstPlayerXuid()?.let { xuid ->
                xendroid.compose.core.EmulatorRuntime.emulator
                    ?.list_profiles(xendroid.compose.core.ContentPaths.contentRoot().absolutePath)
                    ?.firstOrNull { it.xuid.equals(xuid, ignoreCase = true) }?.gamertag?.ifBlank { null }
            }
        }.getOrNull()
        return xendroid.compose.ui.ingame.LoadingDetails(profile = profile, titleId = title, badges = listOfNotNull(driver, limit),
            ownSettings = own, withOptions = launchArgsUsed.isNotEmpty())
    }

    /** R4: this launch's own options, when the library sent them (its token); every other launch
     *  boots as configured. */
    private fun launchOptionArgs(): List<String> {
        val requested = intent?.getStringArrayExtra(EXTRA_LAUNCH_ARGS) ?: return emptyList()
        if (!xendroid.compose.core.LaunchToken.matches(filesDir, intent?.getStringExtra(EXTRA_LAUNCH_TOKEN))) {
            Log.w(TAG, "Launch options without this app's token ignored")
            return emptyList()
        }
        val args = xendroid.compose.core.LaunchOptions.sanitize(requested, Application.get_custom_driver_dir())
        if (args.size != requested.size) Log.w(TAG, "${requested.size - args.size} launch options dropped")
        if (args.isNotEmpty()) recordEvent("launch options", args.joinToString(" ") { it.substringBefore('=') })
        return args
    }

    /** 15c: follows the fold of a foldable (Jetpack WindowManager); a horizontal one half open is
     *  the tabletop posture "Split screen" waits for. Devices without one report none. */
    private fun watchFold() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                try {
                    androidx.window.layout.WindowInfoTracker.getOrCreate(this@EmulatorHostActivity)
                        .windowLayoutInfo(this@EmulatorHostActivity)
                        .collect { info ->
                            val fold = info.displayFeatures.filterIsInstance<androidx.window.layout.FoldingFeature>().firstOrNull {
                                it.state == androidx.window.layout.FoldingFeature.State.HALF_OPENED &&
                                    it.orientation == androidx.window.layout.FoldingFeature.Orientation.HORIZONTAL
                            }
                            windowFold.value = fold?.bounds?.let { it.top to it.bottom }
                        }
                } catch (e: Exception) {
                    if (e is kotlinx.coroutines.CancellationException) throw e
                    Log.w(TAG, "The fold state is unavailable", e)
                }
            }
        }
    }

    private fun installSurfaceView() {
        val sv =
            SurfaceView(this).apply {
                isFocusable = true
                isFocusableInTouchMode = true

                holder.addCallback(
                    this@EmulatorHostActivity
                )

                setOnGenericMotionListener { _, ev ->
                    onGenericMotion(ev)
                }
            }

        surfaceView = sv
        externalDisplay = ExternalGameDisplay(this, sv, this, detach = {
            surfaceAvailable = false
            pauseForLifecycle()
            if (started) session.detachSurface()
        }, statusChanged = {
            externalDisplayLabel.value = it
            gameOnExternalDisplay.value = externalDisplay?.activeDisplay != null
        })
        tvMargin.floatValue = getSharedPreferences(DISPLAY_SETTINGS_PREFS, MODE_PRIVATE).getFloat("tv_margin_percent", 0f)
            .coerceIn(0f, xendroid.compose.core.TvMargin.MAX)
        externalDisplay?.setMargin(tvMargin.floatValue)

        val compose =
            ComposeView(this).apply {
                setContent {
                    // Batch 2: the menu and panels in touch or controller layout, like the app.
                    val hostMode = xendroid.compose.ui.design.InputModeStore.read(this@EmulatorHostActivity)
                        .resolve(xendroid.compose.ui.design.rememberControllerConnected())
                    val cfg by gamepad.config.collectAsState(
                        initial = GamepadConfigDto()
                    )

                    val landscape =
                        resources.configuration.orientation ==
                            Configuration.ORIENTATION_LANDSCAPE

                    val booted by bootedState

                    val playingTitle by activeTitleState
                    val controls =
                        remember(
                            cfg,
                            landscape,
                            playingTitle
                        ) {
                            // The running game's own layout when it has one (U06).
                            gamepad.controlsFor(
                                cfg,
                                landscape,
                                playingTitle
                            )
                        }

                    val (visible, poke) =
                        rememberAutoHide(
                            cfg.globals.autoHideSeconds
                        )

                    val alpha by animateFloatAsState(
                        if (visible) {
                            cfg.globals.opacity
                        } else {
                            0f
                        },
                        tween(500),
                        label = "padAlpha"
                    )

                    LaunchedEffect(cfg.globals.hideWithController) {
                        hideTouchWithController = cfg.globals.hideWithController
                        if (!hideTouchWithController && overlayHiddenByController.value) {
                            touchPresence.disabled()
                            touchHiddenBy = -1
                            overlayHiddenByController.value = false
                        }
                    }

                    val padVisible =
                        showTouchOverlay.value == true && !overlayHiddenByController.value

                    val overlayActive =
                        booted &&
                            foregroundState.value &&
                            cfg.globals.enabled &&
                            padVisible &&
                            !menuState.value.open

                    DisposableEffect(
                        overlayActive
                    ) {
                        overlayWantsBrightness =
                            overlayActive

                        if (overlayActive) {
                            brightnessSampler.start(sv)
                        }

                        onDispose {
                            brightnessSampler.stop()
                        }
                    }

                    val contrastState =
                        rememberOverlayContrast(
                            brightnessSampler.brightness
                        )

                    LaunchedEffect(
                        cfg.globals.hapticsEnabled
                    ) {
                        configureHaptics(
                            cfg.globals.hapticsEnabled
                        )
                    }

                    // 15c: where the layout is in the window (to place a fold) and how big it is.
                    var layoutBox by remember { mutableStateOf<Pair<IntSize, Int>?>(null) }
                    val fold by windowFold
                    val split = layoutBox?.let { (size, top) ->
                        val hinge = fold?.let { (foldTop, foldBottom) ->
                            xendroid.compose.gamepad.SplitScreen.hingeInLayout(foldTop, foldBottom, top, size.height)
                        }
                        // Without a fold the split is for the touch controls: a controller playing
                        // (or the controls off) gives the picture the whole screen back.
                        val touchShown = cfg.globals.enabled && padVisible
                        if (gameOnExternalDisplay.value || (hinge == null && !touchShown)) null
                        else xendroid.compose.gamepad.SplitScreen.layout(size.width, size.height,
                            xendroid.compose.gamepad.SplitScreenMode.parse(cfg.globals.splitScreen), hinge)
                    }
                    LaunchedEffect(split) {
                        val text = split?.let { "game ${it.game.width}x${it.game.height}, controls ${it.controls.width}x${it.controls.height}" }
                        if (text != splitRecorded && (text != null || splitRecorded != null)) recordEvent("split", text ?: "off")
                        splitRecorded = text
                    }

                    Box(Modifier.fillMaxSize().onGloballyPositioned { c ->
                        val next = c.size to c.positionInWindow().y.roundToInt()
                        if (next != layoutBox) layoutBox = next
                    }) {
                            AndroidView(
                                factory = {
                                    sv
                                },
                                modifier =
                                    split?.let { Modifier.atRect(it.game) } ?: Modifier.fillMaxSize()
                            )

                            // 15e: the loading screen until the first guest frame, then a short fade.
                            val loading = bootStatus.value
                            var lastLoading by remember { mutableStateOf(loading) }
                            if (loading != null) lastLoading = loading
                            androidx.compose.animation.AnimatedVisibility(
                                visible = loading != null,
                                enter = androidx.compose.animation.EnterTransition.None,
                                exit = androidx.compose.animation.fadeOut(androidx.compose.animation.core.tween(400)),
                            ) {
                                lastLoading?.let { status ->
                                    xendroidTheme(scale = uiScale, mode = hostMode) {
                                        xendroid.compose.ui.ingame.GameLoadingScreen(status, loadingArt.value, loadingName,
                                            onCancel = if (menuState.value.open || loading == null) null else ::cancelBoot,
                                            details = loadingDetails.value)
                                    }
                                }
                            }

                            if (
                                booted &&
                                foregroundState.value &&
                                cfg.globals.enabled &&
                                padVisible &&
                                !menuState.value.open &&
                                bootStatus.value == null
                            ) {
                                GamepadOverlay(
                                    style = xendroid.compose.gamepad.ControlStyle.parse(cfg.globals.style),
                                    controls =
                                        controls,
                                    adaptiveSticks = adaptiveSticks.value,
                                    touchCamera = touchCamera.value,
                                    cameraSensitivity = cfg.globals.cameraSensitivity,
                                    cameraAreaStart = cfg.globals.cameraAreaStart,
                                    slideButtons = cfg.globals.slideButtons,
                                    slideSticks = cfg.globals.slideSticks,

                                    opacity =
                                        alpha,

                                    contrast = {
                                        contrastState.value
                                    },

                                    onUserInteraction =
                                        poke,

                                    onKeyEvent = {
                                        kc,
                                        pressed,
                                        v ->

                                        if (
                                            pressed &&
                                            v ==
                                                Kc.VALUE_UNUSED
                                        ) {
                                            maybeVibrate()
                                        }
                                        if (kc in KC_RTHUMB_LEFT..KC_RTHUMB_DOWN) touchStickHeld[kc] = pressed

                                        session.keyEvent(
                                            kc,
                                            pressed,
                                            v
                                        )
                                    },

                                    modifier =
                                        split?.let { Modifier.atRect(it.controls) } ?: Modifier.fillMaxSize(),
                                )
                            }

                            LaunchedEffect(
                                booted
                            ) {
                                if (!booted) {
                                    return@LaunchedEffect
                                }
                                // Guest rumble (I04): only while the game itself runs, never under the
                                // menu or a pause, where the last requested strength would linger.
                                // Phone controllers (I09) get their slot's rumble raw: each phone
                                // applies its owner's intensity. Their input is held meanwhile.
                                launch {
                                    while (isActive) {
                                        delay(50)
                                        // U10: the same for the keyboard grid, in two dimensions.
                                        if (keyboardRequestState.value != null && gridHatDirection != 4 &&
                                            gridRepeat.press(gridHatDirection, SystemClock.uptimeMillis())) {
                                            keyboardGrid.value = keyboardGrid.value.move(gridHatDirection % 3 - 1, gridHatDirection / 3 - 1)
                                        }
                                        // U04: a stick or hat held on a menu keeps moving at the menu's pace.
                                        val heldDirection = if (panelNavPrev) -1 else if (panelNavNext) 1 else 0
                                        if (heldDirection != 0) panelNav()?.let { nav ->
                                            if (navRepeat.press(heldDirection, SystemClock.uptimeMillis())) movePanelSelection(nav, heldDirection)
                                        }
                                        // Round 2: held left or right on a menu row keeps changing its value.
                                        val heldAdjust = if (panelNavLeft) -1 else if (panelNavRight) 1 else 0
                                        if (heldAdjust != 0 && menuRowsActive() && navRepeat.press(heldAdjust * 2, SystemClock.uptimeMillis())) {
                                            adjustSelectedRow(heldAdjust)
                                        }
                                        val playing = foregroundState.value && !menuState.value.open && !session.isPaused()
                                        val phones = phoneControllers.host != null
                                        val controllerRumble = rumbleSettings.value.anyOn
                                        val guest = if (playing && (controllerRumble || phones)) session.rumbleState() else null
                                        if (phones) runCatching { phoneControllers.tick(playing, guest) }
                                        val state = if (controllerRumble) guest else null
                                        if (state == null) stopRumble() else runCatching { driveRumble(state) }
                                    }
                                }
                                syncControllerSlots()

                                while (isActive) {
                                    showTouchOverlay.value =
                                        session
                                            .showTouchOverlayEnabled()
                                    gameModeSignal.update(xendroid.compose.core.gamePhase(foregroundState.value,
                                        firstFrame = bootStatus.value == null,
                                        menuOrPaused = menuState.value.open || session.isPaused()))
                                    // The controller that put the touch controls aside now plays P2–P4.
                                    if (overlayHiddenByController.value && touchHiddenBy >= 0 && playerSlot(touchHiddenBy) != 0) {
                                        touchControlsBack("the controller now plays another player")
                                    }

                                    val activeTitle = session.activeTitleId()
                                    performanceHints.update(session.presenterWork(),
                                        1_000_000_000L / session.fpsLimit().coerceAtLeast(30),
                                        foregroundState.value && !menuState.value.open)
                                    performanceHintsLabel.value = performanceHints.status
                                    val activeRun = runId
                                    if (activeTitle != controlsTitleId) {
                                        controlsTitleId = activeTitle
                                        activeTitleState.value = activeTitle
                                        // 15g: this game's own HUD look, when it has one.
                                        hudLook.value = xendroid.compose.core.HudPlacements.read(
                                            HudPreferences.of(this@EmulatorHostActivity), activeTitle).look
                                        hudScale.value = null
                                        // Round 2: its changes start empty; its own display mode, filter and refresh rate apply.
                                        startGameChanges(activeTitle)
                                        if (bootStatus.value != null && loadingArt.value == null && activeTitle != null) {
                                            loadingArt.value = withContext(Dispatchers.IO) {
                                                runCatching {
                                                    xendroid.compose.core.LaunchArt.forTitle(
                                                        xendroid.compose.data.CoverStore(java.io.File(filesDir, "covers")), activeTitle)
                                                }.getOrNull()
                                            }
                                        }
                                        val driver = session.activeDriverIdentity()
                                        recordEvent("title", activeTitle?.let { "$it running" } ?: "none active", flush = true)
                                        if (driver != null && !driverRecorded) {
                                            driverRecorded = true
                                            recordEvent("driver", driver.label)
                                        }
                                        if (activeTitle != null) lifecycleScope.launch(Dispatchers.IO) {
                                            runCatching { SessionLogs.noteTitle(activeTitle, BuildConfig.VERSION_NAME) }
                                                .onFailure { Log.w(TAG, "Writing diagnostic session context failed", it) }
                                            val profile = runCatching { firstPlayerXuid() }
                                                .onFailure { Log.w(TAG, "Reading P1's profile failed", it) }.getOrNull()
                                            val modules = runCatching { session.moduleHashes() }.getOrDefault(emptyList())
                                            val settings = runCatching { session.changedSettings() }
                                                .onFailure { Log.w(TAG, "Reading the changed settings failed", it) }.getOrNull()
                                            bootChangedSettings = settings
                                            if (activeRun != null) runCatching { xendroid.compose.sessions.SessionRuns.store().running(activeRun, activeTitle, driver, profile, modules, settings) }
                                                .onFailure { Log.w(TAG, "Recording the running title failed", it) }
                                        }
                                        // C07: the vblank cap this run booted with, for comparisons; U01: the
                                        // driver setting it started with, for the Graphics tab.
                                        if (activeTitle != null) lifecycleScope.launch {
                                            withContext(Dispatchers.IO) {
                                                runCatching { inGameConfig.guestRefreshCap(activeTitle) }
                                                    .onFailure { Log.w(TAG, "Reading the vblank cap failed", it) }.getOrNull()
                                            }?.let { runPerformance.guestRefreshCap(it) }
                                            driverAtBoot.value = withContext(Dispatchers.IO) {
                                                runCatching { inGameConfig.driverPath(activeTitle) }
                                                    .onFailure { Log.w(TAG, "Reading the driver setting failed", it) }.getOrNull()
                                            }
                                            gpuLive.value = withContext(Dispatchers.IO) {
                                                runCatching { inGameConfig.gpuLiveValues(activeTitle) }
                                                    .onFailure { Log.w(TAG, "Reading the GPU options failed", it) }.getOrNull()
                                            } ?: emptyMap()
                                            if (bootStatus.value != null) loadingDetails.value = withContext(Dispatchers.IO) {
                                                runCatching { loadingDetailsOf(activeTitle) }
                                                    .onFailure { Log.w(TAG, "Reading what the game starts with failed", it) }.getOrNull()
                                            }
                                        }
                                        adaptiveSticks.value = activeTitle != null &&
                                            getSharedPreferences("touch_options", MODE_PRIVATE)
                                                .getBoolean("adaptive_$activeTitle", false)
                                    }
                                    // One sample per second for the run summary (C02); paused or
                                    // background seconds count as idle, never as FPS.
                                    val fgNow = session.presentationState()
                                    val frameTimes = session.guestFrameTimeHistogram()
                                    val guestFrames = frameTimes?.sum()
                                    val runningNow = foregroundState.value && !session.isPaused()
                                    runPerformance.sample(
                                        running = runningNow,
                                        guestFps = session.averageFps(),
                                        presentCount = session.hostPresentSubmissionCount(),
                                        generatedCount = fgNow.generated,
                                        frameGenerationActive = fgNow.requested && fgNow.state == 2,
                                        guestFrames = guestFrames,
                                        fpsLimit = session.fpsLimit(),
                                        displayHz = currentOutputHz(),
                                    )
                                    runPerformance.frameTimes(frameTimes)
                                    performancePanel.frameTimes(frameTimes)
                                    runPerformance.frameGeneration(session.frameGenerationGpuHistogram(), fgNow.lateSkips, fgNow.dropped, fgNow.syntheticSlots)
                                    // F04, advisory only: a verdict change goes to the timeline.
                                    fgGovernor.sample(xendroid.compose.core.FrameGenerationGovernor.Second(
                                        active = fgNow.requested && fgNow.state == 2, displayHz = fgNow.hz,
                                        multiplier = if (fgNow.engine == 1) fgNow.multiplier else 2,
                                        guestFps = session.averageFps(), gpuMs = fgNow.gpuMs,
                                        slots = fgNow.syntheticSlots, lateSkips = fgNow.lateSkips,
                                        thermalStatus = lastThermalStatus,
                                    ))?.let { recordEvent("fg budget", it.text) }
                                    // What frame generation does, or would do, at this refresh rate.
                                    if (runningNow && !menuState.value.open) {
                                        session.averageFps().takeIf { it > 0 }?.let { lastGuestFps = it }
                                        val generatedNs = android.os.SystemClock.elapsedRealtimeNanos()
                                        if (lastGenerated >= 0 && generatedNs > lastGeneratedNs) {
                                            generatedPerSecond = (fgNow.generated - lastGenerated).coerceAtLeast(0) *
                                                1e9 / (generatedNs - lastGeneratedNs)
                                        }
                                        lastGenerated = fgNow.generated
                                        lastGeneratedNs = generatedNs
                                    } else {
                                        lastGenerated = -1L
                                    }
                                    fgNotes.value = frameGenerationNotes(fgNow)
                                    fgBudgetLabel.value = fgGovernor.current.takeIf {
                                        it.verdict != xendroid.compose.core.FrameGenerationGovernor.Verdict.OFF
                                    }?.let { "Budget (advisory, never acts): ${it.text}" }
                                    notePresentation(fgNow, runningNow, guestFrames ?: session.hostPresentSubmissionCount())
                                    if (runningNow) watchThermalHeadroom()
                                    val compileStats = session.shaderCompileStats()
                                    if (bootStatus.value != null) {
                                        val elapsed = (android.os.SystemClock.elapsedRealtime() - createdAtMs) / 1000
                                        if ((guestFrames ?: 0L) > 0L) {
                                            bootStatus.value = null
                                            runPerformance.firstFrame(elapsed.toInt())
                                            recordEvent("boot", "first guest frames after $elapsed s")
                                        } else {
                                            bootStatus.value = xendroid.compose.ui.ingame.bootStatus(activeTitle != null,
                                                compileStats?.getOrNull(0) ?: 0L, compileStats?.getOrNull(2) ?: 0L, elapsed)
                                        }
                                    }
                                    compileStats?.let { stats ->
                                        runPerformance.compiles(stats)
                                        when (val burst = compileBursts.sample(stats[1], stats[0])) {
                                            // Driver crashes during compiles happen: keep the start on disk.
                                            xendroid.compose.sessions.BurstTracker.Burst.Started ->
                                                recordEvent("compile", "pipeline creation burst started", flush = true)
                                            is xendroid.compose.sessions.BurstTracker.Burst.Ended -> recordEvent("compile",
                                                "${burst.events} pipelines, ${burst.amount / 1_000_000} ms over ${burst.seconds} s")
                                            null -> {}
                                        }
                                    }
                                    session.audioRunStats()?.let { audio ->
                                        runPerformance.audio(audio)
                                        when (val burst = audioBursts.sample(audio[2], audio[1])) {
                                            xendroid.compose.sessions.BurstTracker.Burst.Started ->
                                                recordEvent("audio", "underruns started")
                                            is xendroid.compose.sessions.BurstTracker.Burst.Ended -> recordEvent("audio",
                                                "${burst.amount} of ${burst.events} blocks concealed, ${burst.seconds} s with underruns")
                                            null -> {}
                                        }
                                    }
                                    if (menuState.value.open) menuStatus.value = buildMenuStatus()
                                    // 15n: the performance panel, built only while it is on screen.
                                    panelSnapshot.value = if (performanceOverlayEnabled.value &&
                                        hudDetail.value == xendroid.compose.core.HudDetail.PANEL) {
                                        runCatching { buildPanelSnapshot(fgNow) }
                                            .onFailure { Log.w(TAG, "Building the performance panel failed", it) }.getOrNull()
                                    } else null
                                    // Bounds the play time of a run that later dies without finishing.
                                    val nowMs = android.os.SystemClock.elapsedRealtime()
                                    if (activeRun != null && nowMs - lastRunHeartbeatMs >= 30_000) {
                                        lastRunHeartbeatMs = nowMs
                                        runPerformance.battery(xendroid.compose.sessions.SessionRuns.batteryCelsius(applicationContext))
                                        val summary = runPerformance.snapshot()
                                        val driver = session.activeDriverIdentity()
                                        if (driver != null && !driverRecorded) {
                                            driverRecorded = true
                                            recordEvent("driver", driver.label)
                                        }
                                        lifecycleScope.launch(Dispatchers.IO) {
                                            val modules = runCatching { session.moduleHashes() }.getOrDefault(emptyList())
                                            runCatching { xendroid.compose.sessions.SessionRuns.store().heartbeat(activeRun, summary, driver, modules) }
                                        }
                                        flushRunEvents()
                                    }

                                    // FG can stop by itself (failure, cadence over Hz) while the
                                    // menu is closed: hand the temporary cap back then too.
                                    if (generationCap.active && !menuState.value.open) {
                                        val state = session.presentationState()
                                        if (!state.requested) { presentationState.value = state; restoreGenerationCap() }
                                    }
                                    if (menuState.value.open) {
                                        presentationState.value = session.presentationState()
                                        if (!presentationState.value.requested) restoreGenerationCap()
                                        audioVolume.intValue = session.audioVolume()
                                        gpuLabel.value = session.activeGpuLabel()
                                        menuPaused.value = session.isPaused()
                                        fpsLimitState.intValue = session.fpsLimit()
                                        if (activeTitle != fpsConfig.value.titleId) refreshFpsConfig()
                                        // Players joining or leaving and their latency.
                                        refreshPhoneControllers()
                                    }

                                    delay(1000)
                                }
                            }

                            FpsOverlay(
                                session = session,

                                visible =
                                    booted &&
                                        foregroundState.value && !menuState.value.open &&
                                        performanceOverlayEnabled.value,
                                detail = hudDetail.value,
                                panel = panelSnapshot.value,
                                metrics = hudMetrics.value,
                                titleId = activeTitleState.value,
                                look = hudLook.value,
                                style = hudStyle.value,
                                scaleOverride = hudScale.value,

                                modifier =
                                    Modifier.fillMaxSize(),
                            )

                            if (editorOpen.value) {
                                xendroidTheme(mode = hostMode) { GamepadEditorScreen(
                                    controller = gamepad,
                                    onDone = { editorOpen.value = false },
                                    inGame = true,
                                    titleId = activeTitleState.value,
                                    gameName = loadingName,
                                ) }
                            } else if (menuState.value.open) {
                                xendroidTheme(scale = uiScale, mode = hostMode) {
                                    // Round 2: the menu's values from this activity's state ([menuModel]).
                                    LaunchedEffect(cfg.globals.style) { controlStyle.value = xendroid.compose.gamepad.ControlStyle.parse(cfg.globals.style) }
                                    InGameMenu(
                                        state = menuState.value,
                                        model = menuModel(cfg),
                                        onPage = { page -> menuState.value = menuState.value.copy(page = page, chip = 0) },
                                        onSelect = { index -> menuState.value = menuState.value.select(index) },
                                        onAction = ::performMenuAction,
                                        onAdjust = ::adjustMenuAction,
                                        onChoose = ::chooseMenuOption,
                                        onSet = ::setMenuValue,
                                        onSetDone = ::finishMenuValue,
                                        onLogChoice = ::chooseLogSession,
                                        onQuitChoice = ::chooseMenuQuit,
                                    )
                                }
                            } else if (
                                booted && keyboardRequestState.value == null &&
                                discRequestState.value == null && messageBoxRequestState.value == null
                            ) {
                                // Round 2: no button over the game; a drag in from the left edge opens the menu.
                                InGameMenuEdge(
                                    onOpen = { openMenu() },
                                    modifier = Modifier.align(Alignment.CenterStart),
                                )
                            }

                            val keyboardRequest by
                                keyboardRequestState

                            LaunchedEffect(
                                booted
                            ) {
                                if (!booted) {
                                    return@LaunchedEffect
                                }

                                while (isActive) {
                                    if (
                                        keyboardRequestState.value ==
                                            null
                                    ) {
                                        session
                                            .keyboardRequest()
                                            ?.let { req ->

                                                val maxUnits = if (req.maxLength <= 0) Int.MAX_VALUE else req.maxLength
                                                keyboardGrid.value = xendroid.compose.ui.keyboard.KeyboardGrid(maxUnits = maxUnits)
                                                    .withText(req.defaultText.orEmpty())
                                                gridHatDirection = 4
                                                gridRepeat.release()

                                                panelSelectedState.intValue =
                                                    0

                                                keyboardRequestState.value =
                                                    req
                                                recordEvent("guest-ui", "text entry prompt")
                                            }
                                    }

                                    delay(
                                        KEYBOARD_POLL_MS
                                    )
                                }
                            }

                            keyboardRequest?.let { req ->
                                xendroidTheme(scale = uiScale, mode = hostMode) {
                                    GuestKeyboardPanel(
                                        request = req,

                                        grid = keyboardGrid.value,

                                        onGridChange = { keyboardGrid.value = it },

                                        onAccept = {
                                            text ->
                                            acceptKeyboard(
                                                text
                                            )
                                        },

                                        onCancel = {
                                            cancelKeyboard()
                                        },

                                        modifier =
                                            Modifier.fillMaxSize(),

                                        gameName = loadingName,
                                        art = loadingArt.value,
                                    )
                                }
                            }

                            val messageBoxRequest by
                                messageBoxRequestState

                            LaunchedEffect(
                                booted
                            ) {
                                if (!booted) {
                                    return@LaunchedEffect
                                }

                                while (isActive) {
                                    if (
                                        messageBoxRequestState.value ==
                                            null
                                    ) {
                                        session
                                            .messageBoxRequest()
                                            ?.let { req ->

                                                panelSelectedState.intValue =
                                                    req.activeButton

                                                messageBoxRequestState.value =
                                                    req
                                                recordEvent("guest-ui", "message box (${req.buttons?.size ?: 0} buttons)")
                                            }
                                    }

                                    delay(
                                        KEYBOARD_POLL_MS
                                    )
                                }
                            }

                            messageBoxRequest?.let { req ->
                                xendroidTheme(scale = uiScale, mode = hostMode) {
                                    GuestMessageBoxPanel(
                                        request = req,

                                        selected =
                                            panelSelectedState.intValue,

                                        onChoose = {
                                            button ->
                                            answerMessageBox(
                                                button
                                            )
                                        },

                                        modifier =
                                            Modifier.fillMaxSize(),

                                        gameName = loadingName,
                                        art = loadingArt.value,
                                    )
                                }
                            }

                            val discRequest by
                                discRequestState

                            LaunchedEffect(
                                booted
                            ) {
                                if (!booted) {
                                    return@LaunchedEffect
                                }

                                while (isActive) {
                                    if (
                                        discRequestState.value ==
                                            null
                                    ) {
                                        session
                                            .discRequest()
                                            ?.let { req ->

                                                // The disc the game asks for first, by the number in its label.
                                                panelSelectedState.intValue =
                                                    xendroid.compose.ui.disc.requestedDiscIndex(req)

                                                discRequestState.value =
                                                    req
                                                recordEvent("guest-ui", "disc ${req.discNumber} requested", flush = true)
                                            }
                                    }

                                    delay(
                                        KEYBOARD_POLL_MS
                                    )
                                }
                            }

                            discRequest?.let { req ->
                                xendroidTheme(scale = uiScale, mode = hostMode) {
                                    DiscSwapPanel(
                                        request = req,

                                        selected =
                                            panelSelectedState.intValue,

                                        onChoose = {
                                            path ->
                                            chooseDisc(
                                                path
                                            )
                                        },

                                        onCancel = {
                                            cancelDisc()
                                        },

                                        modifier =
                                            Modifier.fillMaxSize(),

                                        gameName = loadingName,
                                        art = loadingArt.value,
                                        current = discInDrive ?: launchedGame,
                                    )
                                }
                            }
                    }

                    val menuOpen = menuState.value.open

                    val keyboardOpen =
                        keyboardRequestState.value !=
                            null

                    val discOpen =
                        discRequestState.value !=
                            null

                    val messageBoxOpen =
                        messageBoxRequestState.value !=
                            null

                    BackHandler(
                        enabled = keyboardOpen
                    ) {
                        cancelKeyboard()
                    }

                    BackHandler(
                        enabled = discOpen
                    ) {
                        cancelDisc()
                    }

                    BackHandler(
                        enabled = messageBoxOpen
                    ) {
                        messageBoxRequestState.value
                            ?.let {
                                answerMessageBox(
                                    it.activeButton
                                )
                            }
                    }

                    BackHandler(
                        enabled =
                            !menuOpen &&
                                !keyboardOpen &&
                                !discOpen &&
                                !messageBoxOpen
                    ) {
                        openMenu()
                    }

                    BackHandler(
                        enabled = menuOpen && !editorOpen.value && !keyboardOpen && !discOpen && !messageBoxOpen
                    ) {
                        backMenu()
                    }
                    BackHandler(enabled = editorOpen.value && !keyboardOpen && !discOpen && !messageBoxOpen) {
                        editorOpen.value = false
                    }
                }
            }

        setContentView(compose)
        sv.requestFocus()
    }

    private fun configureHaptics(
        globalsFlag: Boolean
    ) {
        val legacy =
            PreferenceManager
                .getDefaultSharedPreferences(this)
                .getBoolean(
                    "enable_vibrator",
                    false
                )

        hapticsEnabled =
            globalsFlag || legacy

        if (
            hapticsEnabled &&
            vibrator == null
        ) {
            vibrator =
                getSystemService(
                    VIBRATOR_SERVICE
                ) as Vibrator
        }
    }

    private fun maybeVibrate() {
        if (!hapticsEnabled) return

        vibrator?.vibrate(
            VibrationEffect.createOneShot(
                25,
                VibrationEffect.DEFAULT_AMPLITUDE
            )
        )
    }

    override fun surfaceCreated(
        holder: SurfaceHolder
    ) {
        if (externalDisplay?.owns(holder) == false) return
        surfaceAvailable = true
        recordEvent("surface", if (started) "recreated" else "created, booting the core")
        if (!started) {
            started = true

            session.attachSurface(
                holder.surface
            )

            try {
                val labels =
                    intent.getStringArrayExtra(
                        EXTRA_DISC_LABELS
                    )

                val paths =
                    intent.getStringArrayExtra(
                        EXTRA_DISC_PATHS
                    )

                if (
                    labels != null &&
                    paths != null &&
                    labels.isNotEmpty()
                ) {
                    session.discSetKnown(
                        labels.toList(),
                        paths.toList()
                    )
                }

                session.bootOnce()
                bootedState.value = true
            } catch (
                t: RuntimeException
            ) {
                Log.e(
                    TAG,
                    "boot failed",
                    t
                )

                val detail = t.message ?: t.javaClass.simpleName
                showLaunchFailure(getString(R.string.host_core_failed, detail), "The emulator core did not start: $detail",
                    xendroid.compose.ui.ingame.LaunchFailure.Kind.CORE)
            }
        } else {
            session.attachSurface(
                holder.surface
            )

            resumeForLifecycle()
        }
    }

    override fun surfaceChanged(
        holder: SurfaceHolder,
        format: Int,
        width: Int,
        height: Int
    ) {
        if (externalDisplay?.owns(holder) == false) return
        if (!started) return
        if (width == 0 || height == 0) return
        recordEvent("surface", "${width}x$height")

        session.changeSurface(
            width,
            height
        )
    }

    override fun surfaceDestroyed(
        holder: SurfaceHolder
    ) {
        if (externalDisplay?.owns(holder) == false) return
        surfaceAvailable = false
        if (!started) return
        recordEvent("surface", "destroyed", flush = true)

        pauseForLifecycle()

        session.detachSurface()
    }

    override fun onPause() {
        super.onPause()
        session.flushGpuCaches()
    }

    override fun onStop() {
        super.onStop()
        foregroundState.value = false
        // The background is where the system kills a game: keep the log current.
        recordEvent("lifecycle", "background", flush = true)

        mainHandler.removeCallbacks(
            pauseOnFocusLost
        )

        pauseForLifecycle()

        brightnessSampler.stop()

        // Some OEM launchers/recents strip the window's immersive insets while backgrounded;
        // let the next onWindowFocusChanged(true) reapply them once.
        immersiveModeApplied = false

        EmuProcessLink.setEmuForeground(
            false
        )
    }

    override fun onStart() {
        super.onStart()
        foregroundState.value = true
        recordEvent("lifecycle", "foreground")

        EmuProcessLink.setEmuForeground(
            true
        )

        if (overlayWantsBrightness) {
            surfaceView?.let {
                brightnessSampler.start(it)
            }
        }

        resumeForLifecycle()
    }

    private fun pauseForLifecycle() {
        gyroCamera.stop()
        performanceHints.close()
        releaseGuestInput()
        if (backgroundPolicy.value != BackgroundPolicy.AUTO && surfaceAvailable) return
        if (!session.booted) return
        releaseGuestInput()
        if (!session.isPaused()) {
            session.pause()
            pausedByLifecycle = true
            recordEvent("pause", "guest paused by the app lifecycle")
        }
        menuPaused.value = session.isPaused()
    }

    private fun resumeForLifecycle() {
        if (pausedByLifecycle && session.booted && surfaceAvailable && hasWindowFocus() &&
            !menuState.value.pausedByMenu &&
            lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) {
            session.resumeIfPaused()
            pausedByLifecycle = false
            menuPaused.value = session.isPaused()
            recordEvent("pause", "guest resumed")
        }
        if (gyroEnabled.value && foregroundState.value && !menuState.value.open && hasWindowFocus()) gyroCamera.start()
    }

    override fun onDestroy() {
        gameModeSignal.update(xendroid.compose.core.GamePhase.NONE)
        performanceHints.close()
        gyroCamera.stop()
        stopEventSources()
        externalDisplay?.close()
        // The process dies below: tell the phones now (bounded wait, sockets off this thread).
        phoneControllers.close(timeoutMs = 300)
        super.onDestroy()

        keyboardRequestState.value = null
        session.keyboardCancelAll()

        discRequestState.value = null
        session.discCancelAll()

        messageBoxRequestState.value = null
        session.messageBoxCancelAll()

        // The one place this process finalizes its run; a death before this point is
        // finalized later from the platform's exit reason (SessionRunStore.reconcile).
        runId?.let { id ->
            runCatching {
                val failure = launchFailure
                runPerformance.battery(xendroid.compose.sessions.SessionRuns.batteryCelsius(applicationContext))
                runPerformance.frameTimes(session.guestFrameTimeHistogram())
                session.presentationState().let { fg ->
                    runPerformance.frameGeneration(session.frameGenerationGpuHistogram(), fg.lateSkips, fg.dropped, fg.syntheticSlots)
                }
                runPerformance.compiles(session.shaderCompileStats())
                runPerformance.audio(session.audioRunStats())
                runEvents.record("exit", if (isFinishing) "activity finished" else "activity destroyed by the system")
                runCatching { xendroid.compose.sessions.SessionRuns.store().saveEvents(id, runEvents.snapshot()) }
                    .onFailure { Log.w(TAG, "Saving the run events failed", it) }
                xendroid.compose.sessions.SessionRuns.store().finish(id,
                    if (failure != null) xendroid.compose.sessions.RunState.FAILED else xendroid.compose.sessions.RunState.ENDED,
                    failure ?: if (isFinishing) "activity finished" else "activity destroyed by the system",
                    runPerformance.snapshot())
            }.onFailure { Log.w(TAG, "Finishing the session run record failed", it) }
        }

        Process.killProcess(
            Process.myPid()
        )
    }

    // Public platform input callback; only the inherited AndroidX implementation
    // class is library-restricted. Keep the restriction exception local to this override.
    @SuppressLint("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0 && isControllerEvent(event) &&
            event.keyCode != KeyEvent.KEYCODE_BACK) noteControllerUse(event)
        // U10: a controller types on the grid; the focused text field must not eat its D-pad.
        if (keyboardRequestState.value != null && !editorOpen.value && isControllerEvent(event)) {
            return when (event.action) {
                KeyEvent.ACTION_DOWN -> onKeyDown(event.keyCode, event)
                KeyEvent.ACTION_UP -> onKeyUp(event.keyCode, event)
                else -> true
            }
        }
        if (editorOpen.value && !hasGuestPrompt()) {
            val code = xendroid.compose.gamepad.MenuButtons.frontendKey(event.keyCode, swapConfirm)
                ?: return super.dispatchKeyEvent(event)
            return super.dispatchKeyEvent(KeyEvent(event.downTime, event.eventTime, event.action, code,
                event.repeatCount, event.metaState, event.deviceId, event.scanCode, event.flags, event.source))
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onKeyDown(
        keyCode: Int,
        event: KeyEvent
    ): Boolean {
        if (editorOpen.value && !hasGuestPrompt()) return super.onKeyDown(keyCode, event)
        // Let Android/Compose dispatch Back to the highest-priority BackHandler.
        // Consuming it with other menu keys leaves the pause sheet impossible to close.
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, event)

        val identity = keyIdentity(event)
        val direction = xendroid.compose.gamepad.MenuButtons.direction(keyCode)
        if (event.repeatCount > 0 && consumedMenuKeys.contains(identity)) {
            // U04: a held D-pad moves at a readable pace, not at the key repeat rate.
            if (direction == 0 || !navRepeat.press(direction, event.eventTime)) return true
        } else if (direction != 0 && event.repeatCount == 0) {
            // A fresh press restarts the pace, even if the last release never arrived.
            navRepeat.release()
            navRepeat.press(direction, event.eventTime)
        }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_MODE && !hasGuestPrompt()) {
            if (event.repeatCount == 0) {
                if (menuState.value.open) backMenu() else openMenu()
            }
            consumedMenuKeys.add(identity)
            return true
        }

        if (keyboardRequestState.value != null && keyboardKeyDown(keyCode, event)) {
            consumedMenuKeys.add(identity)
            return true
        }

        val nav = panelNav()

        if (nav != null) {
            if (menuState.value.open && !hasGuestPrompt() &&
                !menuState.value.confirmingQuit && !menuState.value.logPicker &&
                (keyCode == KeyEvent.KEYCODE_BUTTON_L1 || keyCode == KeyEvent.KEYCODE_BUTTON_R1)
            ) {
                if (event.repeatCount == 0) {
                    menuState.value = menuState.value.changePage(
                        if (keyCode == KeyEvent.KEYCODE_BUTTON_L1) -1 else 1
                    )
                }
                consumedMenuKeys.add(identity)
                return true
            }
            if (
                panelKeyDown(
                    nav,
                    keyCode
                )
            ) {
                consumedMenuKeys.add(identity)
                return true
            }

            if (consumeIfGamepad(event)) {
                consumedMenuKeys.add(identity)
                return true
            }
            if (menuState.value.open && !hasGuestPrompt()) {
                consumedMenuKeys.add(identity)
                return true
            }
        }

        val gameKey =
            keyMap[keyCode]
                ?: return consumeIfGamepad(event) ||
                    super.onKeyDown(
                        keyCode,
                        event
                    )

        // P2-P4 controllers play their own slot (I03); P1 continues below unchanged.
        val player = playerSlot(event.deviceId)
        if (player > 0) {
            if (event.repeatCount == 0) slotInput.keyDown(player, identity, gameKey)
            return true
        }

        if (event.repeatCount == 0) {
            sentGuestKeys[identity] = gameKey
            session.keyEvent(
                gameKey,
                true,
                KEY_VALUE_UNUSED
            )

            return true
        }

        return super.onKeyDown(
            keyCode,
            event
        )
    }

    override fun onKeyUp(
        keyCode: Int,
        event: KeyEvent
    ): Boolean {
        if (editorOpen.value && !hasGuestPrompt()) return super.onKeyUp(keyCode, event)
        val identity = keyIdentity(event)
        if (xendroid.compose.gamepad.MenuButtons.direction(keyCode) != 0) navRepeat.release()
        if (consumedMenuKeys.remove(identity)) return true
        val player = playerSlot(event.deviceId)
        if (player > 0 && slotInput.keyUp(player, identity)) return true

        sentGuestKeys.remove(identity)?.let { gameKey ->
            session.keyEvent(gameKey, false, KEY_VALUE_UNUSED)
            return true
        }

        if (
            panelNav() != null &&
            (
                isPanelKey(keyCode) ||
                    consumeIfGamepad(event)
            )
        ) {
            return true
        }

        return consumeIfGamepad(event) || super.onKeyUp(keyCode, event)
    }

    private fun keyIdentity(event: KeyEvent): Long =
        (event.deviceId.toLong() shl 32) or (event.keyCode.toLong() and 0xffffffffL)

    private fun consumeIfGamepad(
        event: KeyEvent
    ): Boolean =
        event.keyCode != KeyEvent.KEYCODE_BACK &&
            (
                event.source and
                    InputDevice.SOURCE_GAMEPAD ==
                    InputDevice.SOURCE_GAMEPAD ||
                    event.source and
                    InputDevice.SOURCE_JOYSTICK ==
                    InputDevice.SOURCE_JOYSTICK
            )

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        applyUnbufferedInput()
    }

    /** Controller sticks as they arrive instead of batched to the next frame, up to a frame
     *  less latency (Bannerlator 3b08d65e); Android 11+. Key presses are never batched. */
    private fun applyUnbufferedInput() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.decorView.requestUnbufferedDispatch(if (unbufferedInput.value) InputDevice.SOURCE_CLASS_JOYSTICK else 0)
        }
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        // Each touch gesture unbatched too, for the on-screen controls.
        if (unbufferedInput.value && event.actionMasked == MotionEvent.ACTION_DOWN) {
            window.decorView.requestUnbufferedDispatch(event)
        }
        return super.dispatchTouchEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK &&
            xendroid.compose.gamepad.TouchOverlayPresence.pushed(
                event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
                event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
                event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE), event.getAxisValue(MotionEvent.AXIS_GAS),
                event.getAxisValue(MotionEvent.AXIS_HAT_X), event.getAxisValue(MotionEvent.AXIS_HAT_Y))
        ) noteControllerUse(event)
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onGenericMotionEvent(
        event: MotionEvent
    ): Boolean =
        onGenericMotion(event)

    private fun onGenericMotion(
        event: MotionEvent
    ): Boolean {
        if (editorOpen.value && !hasGuestPrompt()) return super.onGenericMotionEvent(event)
        if (keyboardRequestState.value != null &&
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) {
            keyboardHat(event)
            return true
        }
        panelNav()?.let { nav ->
            panelHat(
                nav,
                event
            )

            return true
        }

        val player = playerSlot(event.deviceId)
        if (player > 0 && event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK) {
            // Same axes and rules as P1 below; a D-pad that also sends keys has no hat here.
            val hat = isNonDpadSource(event)
            slotInput.motion(player,
                event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
                event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
                maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)),
                maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)),
                if (hat) event.getAxisValue(MotionEvent.AXIS_HAT_X) else 0f,
                if (hat) event.getAxisValue(MotionEvent.AXIS_HAT_Y) else 0f)
            return true
        }

        val hatHandled =
            isNonDpadSource(event) &&
                handleHat(event)

        if (
            event.source and
                InputDevice.SOURCE_JOYSTICK !=
                InputDevice.SOURCE_JOYSTICK
        ) {
            return hatHandled ||
                super.onGenericMotionEvent(
                    event
                )
        }

        emitAxisPair(
            event.getAxisValue(
                MotionEvent.AXIS_X
            ),
            negKey = KC_LTHUMB_LEFT,
            posKey = KC_LTHUMB_RIGHT,
            invert = false
        )

        emitAxisPair(
            event.getAxisValue(
                MotionEvent.AXIS_Y
            ),
            negKey = KC_LTHUMB_UP,
            posKey = KC_LTHUMB_DOWN,
            invert = true
        )

        emitAxisPair(
            event.getAxisValue(
                MotionEvent.AXIS_Z
            ),
            negKey = KC_RTHUMB_LEFT,
            posKey = KC_RTHUMB_RIGHT,
            invert = false
        )

        emitAxisPair(
            event.getAxisValue(
                MotionEvent.AXIS_RZ
            ),
            negKey = KC_RTHUMB_UP,
            posKey = KC_RTHUMB_DOWN,
            invert = true
        )

        lTriggerDown =
            emitTrigger(
                maxOf(
                    event.getAxisValue(
                        MotionEvent.AXIS_LTRIGGER
                    ),
                    event.getAxisValue(
                        MotionEvent.AXIS_BRAKE
                    )
                ),
                KC_TRIGGER_L,
                lTriggerDown
            )

        rTriggerDown =
            emitTrigger(
                maxOf(
                    event.getAxisValue(
                        MotionEvent.AXIS_RTRIGGER
                    ),
                    event.getAxisValue(
                        MotionEvent.AXIS_GAS
                    )
                ),
                KC_TRIGGER_R,
                rTriggerDown
            )

        return true
    }

    private fun emitTrigger(
        value: Float,
        gameKey: Int,
        wasDown: Boolean
    ): Boolean {
        val down =
            value > 0.5f

        if (down != wasDown) {
            session.keyEvent(
                gameKey,
                down,
                KEY_VALUE_UNUSED
            )
        }

        return down
    }

    private fun emitAxisPair(
        axis: Float,
        negKey: Int,
        posKey: Int,
        invert: Boolean
    ) {
        val raw =
            if (invert) {
                -axis
            } else {
                axis
            }

        val v =
            if (
                abs(raw) <
                    AXIS_DEADZONE
            ) {
                0f
            } else {
                raw
            }

        when {
            v < 0f -> {
                emitAxis(
                    posKey,
                    false,
                    0
                )

                emitAxis(
                    negKey,
                    true,
                    (v * 32768f).toInt()
                )
            }

            v > 0f -> {
                emitAxis(
                    negKey,
                    false,
                    0
                )

                emitAxis(
                    posKey,
                    true,
                    (v * 32767f).toInt()
                )
            }

            else -> {
                emitAxis(
                    negKey,
                    false,
                    0
                )

                emitAxis(
                    posKey,
                    false,
                    0
                )
            }
        }
    }

    private val axisPressed =
        BooleanArray(24)

    private val axisValue =
        IntArray(24) {
            Int.MIN_VALUE
        }

    private fun emitAxis(
        code: Int,
        pressed: Boolean,
        value: Int
    ) {
        if (
            axisPressed[code] == pressed &&
            axisValue[code] == value
        ) {
            return
        }

        axisPressed[code] = pressed
        axisValue[code] = value

        session.keyEvent(
            code,
            pressed,
            value
        )
    }

    private var hatLeft = false
    private var hatUp = false
    private var hatRight = false
    private var hatDown = false

    private fun handleHat(
        event: MotionEvent
    ): Boolean {
        val hx =
            event.getAxisValue(
                MotionEvent.AXIS_HAT_X
            )

        val hy =
            event.getAxisValue(
                MotionEvent.AXIS_HAT_Y
            )

        val left =
            hx < -0.5f

        val right =
            hx > 0.5f

        val up =
            hy < -0.5f

        val down =
            hy > 0.5f

        if (left != hatLeft) {
            session.keyEvent(
                KC_DPAD_LEFT,
                left,
                KEY_VALUE_UNUSED
            )

            hatLeft = left
        }

        if (right != hatRight) {
            session.keyEvent(
                KC_DPAD_RIGHT,
                right,
                KEY_VALUE_UNUSED
            )

            hatRight = right
        }

        if (up != hatUp) {
            session.keyEvent(
                KC_DPAD_UP,
                up,
                KEY_VALUE_UNUSED
            )

            hatUp = up
        }

        if (down != hatDown) {
            session.keyEvent(
                KC_DPAD_DOWN,
                down,
                KEY_VALUE_UNUSED
            )

            hatDown = down
        }

        return left ||
            right ||
            up ||
            down
    }

    private fun isNonDpadSource(
        event: MotionEvent
    ): Boolean =
        event.source and
            InputDevice.SOURCE_DPAD !=
            InputDevice.SOURCE_DPAD

    private class PanelNav(
        val count: Int,
        val activate: (Int) -> Unit,
        val cancel: () -> Unit,
    )

    private fun hasGuestPrompt(): Boolean =
        discRequestState.value != null || messageBoxRequestState.value != null ||
            keyboardRequestState.value != null

    private fun panelNav(): PanelNav? {
        discRequestState.value?.let { req ->
            val paths =
                req.discPaths ?: emptyArray()

            val discs =
                minOf(
                    req.discLabels?.size ?: 0,
                    paths.size
                )

            return PanelNav(
                discs + 1,

                { i ->
                    if (i < discs) {
                        chooseDisc(
                            paths[i]
                        )
                    } else {
                        cancelDisc()
                    }
                },

                {
                    cancelDisc()
                }
            )
        }

        messageBoxRequestState.value?.let { req ->
            return PanelNav(
                req.buttons?.size ?: 0,

                { i ->
                    answerMessageBox(
                        i
                    )
                },

                {
                    answerMessageBox(
                        req.activeButton
                    )
                }
            )
        }

        keyboardRequestState.value?.let {
            return PanelNav(
                2,

                { i ->
                    if (i == 0) {
                        acceptKeyboard(
                            keyboardGrid.value.text
                        )
                    } else {
                        cancelKeyboard()
                    }
                },

                {
                    cancelKeyboard()
                }
            )
        }

        if (menuState.value.open) {
            return PanelNav(
                menuState.value.count,

                { i ->
                    menuState.value = menuState.value.select(i)
                    if (menuState.value.confirmingQuit) {
                        chooseMenuQuit(i == 1)
                    } else if (menuState.value.logPicker) {
                        chooseLogSession(i)
                    } else {
                        menuState.value.action?.let(::performMenuAction)
                    }
                },

                {
                    backMenu()
                }
            )
        }

        return null
    }

    private fun toggleTouchOverlay() {
        // Put aside by a controller: showing them overrules it until that controller goes,
        // and the setting (on) stays as it is.
        if (overlayHiddenByController.value && showTouchOverlay.value == true) {
            touchPresence.shownByUser()
            overlayHiddenByController.value = false
            recordEvent("touch controls", "shown from the menu while a controller plays P1")
            return
        }
        val next =
            showTouchOverlay.value != true
        if (next) {
            touchPresence.shownByUser()
            overlayHiddenByController.value = false
        }

        showTouchOverlay.value =
            next

        session.setShowTouchOverlay(
            next
        )

        // Live/session-only. The settings screens expose explicit global/per-game
        // persistence, so opening the menu cannot silently overwrite either scope.
    }

    /** Opens the in-game menu; it pauses the game when the player keeps "Pause when the menu opens" on (round 2). */
    private fun openMenu() {
        gyroCamera.stop()
        if (menuState.value.open || hasGuestPrompt()) return
        releaseGuestInput()
        val pausedHere = pauseOnOpen.value && session.booted && !session.isPaused()
        if (pausedHere) session.pause()
        recordEvent("menu", if (pausedHere) "opened, guest paused" else "opened")
        menuPaused.value = session.isPaused()
        fpsLimitState.intValue = session.fpsLimit()
        presentationState.value = session.presentationState()
        menuState.value = menuState.value.show(pausedHere)
        menuStatus.value = runCatching { buildMenuStatus() }.getOrDefault(emptyList())
        refreshFpsConfig()
        refreshDriverLine()
        refreshGameOwnKeys()
        lifecycleScope.launch {
            lsfgCache.value = withContext(Dispatchers.IO) { runCatching { LsfgAssets.cache(applicationContext)?.path }.getOrNull() }
        }
    }

    private fun closeMenuAndResume() {
        val resume = menuState.value.pausedByMenu || pausedByLifecycle
        menuState.value = menuState.value.hide()
        recordEvent("menu", "closed")
        panelNavPrev = false
        panelNavNext = false
        if (resume) {
            pausedByLifecycle = true
            resumeForLifecycle()
        }
        if (gyroEnabled.value && hasWindowFocus()) gyroCamera.start()
    }

    private fun backMenu() {
        if (menuState.value.logPicker) menuState.value = menuState.value.closeLogs()
        else if (menuState.value.confirmingQuit) menuState.value = menuState.value.cancelQuit()
        else closeMenuAndResume()
    }

    private fun chooseMenuQuit(quit: Boolean) {
        if (quit && fpsConfig.value.saving) {
            Toast.makeText(this, getString(R.string.host_wait_config_save), Toast.LENGTH_SHORT).show()
        } else if (quit) {
            runId?.let { id -> runCatching { xendroid.compose.sessions.SessionRuns.store().ending(id, "user exit") } }
            recordEvent("exit", "user exit")
            leave()
        } else menuState.value = menuState.value.cancelQuit()
    }

    @Suppress("DEPRECATION")
    private fun currentOutputHz(): Float = externalDisplay?.activeDisplay?.refreshRate ?:
        (if (Build.VERSION.SDK_INT >= 30) display?.refreshRate else windowManager.defaultDisplay.refreshRate) ?: 60f

    private fun frameGenerationNotes(state: PresentationState): List<String> {
        val hz = currentOutputHz()
        return if (state.requested && state.state == 2) {
            xendroid.compose.core.FrameGenerationReadout.running(lastGuestFps, generatedPerSecond,
                if (state.engine == 1) state.multiplier else 2, hz)
        } else {
            // 15d: with a target, LSFG runs at the planned multiplier from the planned cap.
            val target = xendroid.compose.core.FrameGenerationTarget.plan(lsfgTarget.intValue, hz,
                generationCap.playerLimit(session.fpsLimit()))
            listOfNotNull(
                xendroid.compose.core.FrameGenerationReadout.preview("Win-FG", 2, lastGuestFps, hz),
                lsfgCache.value?.let {
                    xendroid.compose.core.FrameGenerationReadout.preview("LSFG Native", target?.multiplier ?: lsfgMultiplier.intValue,
                        target?.let { plan -> minOf(lastGuestFps, plan.cap.toDouble()) } ?: lastGuestFps, hz)
                },
            )
        }
    }

    private fun prepareGenerationCap(hz: Float, multiplier: Int = 2) {
        generationCap.prepare(session.fpsLimit(), hz, multiplier)?.let { capped ->
            session.setFpsLimit(capped)
            fpsLimitState.intValue = session.fpsLimit()
        }
    }

    /** 15d: the game at exactly [cap] FPS while LSFG meets a target. */
    private fun prepareExactCap(cap: Int) {
        generationCap.prepareExact(session.fpsLimit(), cap)?.let { capped ->
            session.setFpsLimit(capped)
            fpsLimitState.intValue = session.fpsLimit()
        }
    }

    /** 15d: "120 FPS → 2× with the game at 60 FPS", or "off: multiplier by hand", in the shown language. */
    @androidx.compose.runtime.Composable
    private fun lsfgTargetText(): String {
        val choice = lsfgTarget.intValue
        if (choice == xendroid.compose.core.FrameGenerationTarget.OFF) return stringResource(R.string.menu_lsfg_target_off)
        val hz = currentOutputHz()
        val name = if (choice == xendroid.compose.core.FrameGenerationTarget.SCREEN)
            stringResource(R.string.menu_lsfg_target_screen, hz.roundToInt())
        else stringResource(R.string.menu_lsfg_target_fps, choice)
        val plan = xendroid.compose.core.FrameGenerationTarget.plan(choice, hz, generationCap.playerLimit(fpsLimitState.intValue))
            ?: return name
        return stringResource(R.string.menu_lsfg_target_plan, name, plan.multiplier, plan.cap) +
            (if (plan.limitedByDisplay) " " + stringResource(R.string.menu_lsfg_target_display, plan.target) else "") +
            (if (plan.belowTarget) " " + stringResource(R.string.menu_lsfg_target_below, plan.output) else "")
    }

    private fun restoreGenerationCap() {
        generationCap.restore(session.fpsLimit())?.let { before ->
            session.setFpsLimit(before)
            fpsLimitState.intValue = session.fpsLimit()
        }
    }

    /** Round 2: what each row of the in-game menu shows, from this activity's state. */
    @androidx.compose.runtime.Composable
    private fun menuModel(cfg: GamepadConfigDto): InGameMenuModel {
        val on = stringResource(R.string.menu_on)
        val off = stringResource(R.string.menu_off)
        val fromSettings = stringResource(R.string.menu_from_settings)
        val unavailable = stringResource(R.string.menu_unavailable)
        val own = gameOwnKeys.value
        val presentation = presentationState.value
        val image = imageTuning.value
        val style = hudStyle.value
        val percent = java.text.NumberFormat.getPercentInstance()
        val number = java.text.NumberFormat.getNumberInstance()
        val gyroAvailable = gyroCamera.available
        val rumbleNames = xendroid.compose.gamepad.RumbleIntensity.entries.associateWith { xendroid.compose.ui.rumbleLabel(it) }
        val chips = hudChipMetrics()
        val chipLabels = chips.map { metric ->
            when (metric) {
                null -> stringResource(R.string.menu_chip_graph)
                HudMetric.GPU_MEMORY -> stringResource(R.string.menu_chip_gpu_memory)
                HudMetric.BATTERY_TEMPERATURE -> stringResource(R.string.menu_chip_battery)
                HudMetric.SOC_TEMPERATURE -> "SoC"
                HudMetric.POWER -> stringResource(R.string.menu_chip_power)
                HudMetric.BATTERY_LEVEL -> stringResource(R.string.menu_chip_charge)
                HudMetric.BATTERY_TIME -> stringResource(R.string.menu_chip_battery_time)
                HudMetric.HOST_SUBMISSIONS -> "Vulkan"
                else -> metric.label
            }
        }
        val hudScaleNow = hudScale.value ?: xendroid.compose.core.HudPlacements.read(HudPreferences.of(this), activeTitleState.value).scale
        val fps = fpsConfig.value
        val values = mapOf(
            // Image
            InGameAction.DISPLAY_MODE to MenuValue(options = listOf(R.string.menu_opt_fit, R.string.menu_opt_fill, R.string.menu_opt_stretch,
                R.string.menu_opt_integer).map { stringResource(it) }, selected = presentation.displayMode.coerceAtLeast(0),
                own = InGameChanges.DISPLAY_MODE in own),
            InGameAction.SCALING_EFFECT to MenuValue(text = listOf(fromSettings, "Bilinear", "CAS", "FSR", "SGSR", "Lanczos", "CRT")
                [(scalingEffect.intValue + 1).coerceIn(0, 6)], own = SCALING_KEY in own),
            InGameAction.ANTIALIASING to MenuValue(text = when (image.antialiasing) {
                0 -> off; 1 -> "FXAA"; 2 -> stringResource(R.string.menu_aa_fxaa_extreme); else -> fromSettings
            }, own = AA_KEY in own),
            InGameAction.SHARPNESS to MenuValue(text = image.sharpness.takeIf { it in 0..4 }?.let { level ->
                stringResource(listOf(R.string.menu_sharpness_soft, R.string.menu_sharpness_low, R.string.menu_sharpness_medium,
                    R.string.menu_sharpness_high, R.string.menu_sharpness_max)[level])
            } ?: fromSettings, own = CAS_KEY in own,
                note = if (scalingEffect.intValue >= 0 && scalingEffect.intValue != 1 && scalingEffect.intValue != 2)
                    stringResource(R.string.menu_sharpness_needs) else null),
            InGameAction.DITHER to MenuValue(text = when (image.dither) { 0 -> off; 1 -> on; else -> fromSettings }, own = DITHER_KEY in own),
            InGameAction.COLOR_FILTER to MenuValue(text = if (presentation.colorError != 0) unavailable else listOf(off,
                stringResource(R.string.menu_color_grayscale), stringResource(R.string.menu_color_contrast), stringResource(R.string.menu_color_warm),
                stringResource(R.string.menu_color_vivid))[presentation.colorFilter.coerceIn(0, 4)],
                enabled = presentation.colorError == 0, own = InGameChanges.COLOR_FILTER in own),
            InGameAction.STRETCH to MenuValue(on = fullscreenStretchEnabled.value, text = stringResource(R.string.menu_t_stretch_note),
                enabled = !fps.loading && !fps.saving && fps.error == null),
            InGameAction.EXTERNAL_DISPLAY to MenuValue(text = externalDisplayLabel.value ?: stringResource(R.string.tv_phone)),
            InGameAction.TV_MARGIN to MenuValue(text = number.format(tvMargin.floatValue.toDouble()) + "%"),
            InGameAction.DRIVER_INFO to MenuValue(text = driverLine.value.let { (state, label) ->
                when (state) {
                    xendroid.compose.driver.DriverIdentity.InGame.UNKNOWN -> stringResource(R.string.menu_driver_unknown)
                    xendroid.compose.driver.DriverIdentity.InGame.AS_SELECTED -> stringResource(R.string.menu_driver, label)
                    xendroid.compose.driver.DriverIdentity.InGame.CUSTOM_DID_NOT_LOAD -> stringResource(R.string.menu_driver_fallback, label)
                    xendroid.compose.driver.DriverIdentity.InGame.OTHER_FOR_NEXT_START -> stringResource(R.string.menu_driver_next, label)
                }
            }),
            InGameAction.WINFG to MenuValue(on = presentation.requested && presentation.engine == 0),
            InGameAction.WINFG_PRESET to MenuValue(text = stringResource(listOf(R.string.menu_preset_quality, R.string.menu_preset_balanced,
                R.string.menu_preset_performance)[fgPreset.intValue.coerceIn(0, 2)])),
            InGameAction.LSFG to MenuValue(on = presentation.requested && presentation.engine == 1,
                enabled = lsfgCache.value != null && !importingLsfg),
            InGameAction.LSFG_MULTIPLIER to MenuValue(text = "${lsfgMultiplier.intValue}×"),
            InGameAction.LSFG_TARGET to MenuValue(text = lsfgTargetText()),
            // Performance
            InGameAction.FPS_LIMIT to MenuValue(options = FPS_CHOICES.map { if (it == 0) stringResource(R.string.menu_unlimited) else "$it" },
                selected = FPS_CHOICES.indexOf(fpsLimitState.intValue), own = FPS_KEY in own,
                note = when {
                    fps.saving -> stringResource(R.string.menu_saving_config)
                    fps.loading -> stringResource(R.string.menu_reading_config)
                    fps.error != null -> fps.error
                    fps.globalLimit != null -> stringResource(R.string.menu_next_launch, fpsLabel(fps.globalLimit),
                        if (fps.titleId == null) stringResource(R.string.menu_game_id_unavailable)
                        else stringResource(R.string.menu_game_limit, fps.gameLimit?.let { fpsLabel(it) } ?: stringResource(R.string.menu_inherits_global)))
                    else -> null
                }),
            InGameAction.REFRESH_RATE to MenuValue(text = requestedRefresh.value?.let { "${it.roundToInt()} Hz" } ?: stringResource(R.string.menu_auto),
                own = InGameChanges.REFRESH_HZ in own,
                note = stringResource(R.string.menu_refresh_rate_value, requestedRefresh.value?.let { "${it.roundToInt()} Hz" } ?: stringResource(R.string.menu_auto),
                    ((if (Build.VERSION.SDK_INT >= 30) display?.refreshRate else @Suppress("DEPRECATION") windowManager.defaultDisplay.refreshRate)
                        ?.roundToInt()?.let { "$it Hz" }) ?: "—")),
            InGameAction.SMOOTH_SHADERS to MenuValue(on = gpuValue(xendroid.compose.core.GpuLiveOption.ASYNC_SKIP_DRAWS) != 0,
                own = xendroid.compose.core.GpuLiveOption.ASYNC_SKIP_DRAWS.key in own, text = stringResource(R.string.menu_smooth_shaders_note)),
            InGameAction.MSAA_4X_AS_2X to MenuValue(on = gpuValue(xendroid.compose.core.GpuLiveOption.MSAA_4X_AS_2X) != 0,
                own = xendroid.compose.core.GpuLiveOption.MSAA_4X_AS_2X.key in own, text = stringResource(R.string.menu_msaa_2x_note)),
            InGameAction.CUTOUT_TRANSPARENCY to MenuValue(on = gpuValue(xendroid.compose.core.GpuLiveOption.ALPHA_TO_COVERAGE_AS_TEST) != 0,
                own = xendroid.compose.core.GpuLiveOption.ALPHA_TO_COVERAGE_AS_TEST.key in own, text = stringResource(R.string.menu_cutout_note)),
            InGameAction.SHADING_RATE to MenuValue(text = when (gpuValue(xendroid.compose.core.GpuLiveOption.SHADING_RATE)) {
                    1 -> "2×1"; 2 -> "1×2"; 3 -> "2×2"; else -> stringResource(R.string.menu_shading_rate_full) },
                own = xendroid.compose.core.GpuLiveOption.SHADING_RATE.key in own, note = stringResource(R.string.menu_shading_rate_note)),
            InGameAction.SCREENSHOT to MenuValue(text = stringResource(R.string.menu_screenshot_note)),
            InGameAction.SUSTAINED_PERFORMANCE to MenuValue(on = sustainedMode.value, enabled = sustainedAvailable.value,
                text = if (!sustainedAvailable.value) unavailable else null),
            InGameAction.PERFORMANCE_HINTS to MenuValue(on = performanceHints.requested, text = performanceHintsLabel.value),
            InGameAction.BACKGROUND_POLICY to MenuValue(text = backgroundPolicy.value.name),
            // HUD
            InGameAction.PERFORMANCE_HUD to MenuValue(on = performanceOverlayEnabled.value),
            InGameAction.HUD_LAYOUT to MenuValue(options = listOf(stringResource(R.string.menu_opt_vertical), stringResource(R.string.menu_opt_horizontal)),
                selected = style.layout.ordinal),
            InGameAction.HUD_STYLE to MenuValue(options = listOf(stringResource(R.string.menu_opt_fps_only), stringResource(R.string.menu_opt_metrics),
                stringResource(R.string.menu_hud_panel)), selected = HUD_DETAILS.indexOf(hudDetail.value)),
            InGameAction.HUD_METRICS to MenuValue(options = chipLabels, checked = chips.indices.filter { i ->
                chips[i]?.let { it in hudMetrics.value } ?: style.graph
            }.toSet()),
            InGameAction.HUD_POSITION to MenuValue(options = listOf(stringResource(R.string.menu_opt_top), stringResource(R.string.menu_opt_bottom)),
                selected = style.edge.ordinal, enabled = style.layout == xendroid.compose.core.HudLayout.HORIZONTAL,
                note = if (style.layout == xendroid.compose.core.HudLayout.VERTICAL) stringResource(R.string.menu_hud_position_note) else null),
            InGameAction.HUD_LOOK to MenuValue(options = listOf(stringResource(R.string.menu_opt_box), stringResource(R.string.menu_opt_outline),
                stringResource(R.string.menu_opt_text)), selected = hudLook.value.ordinal),
            InGameAction.HUD_SIZE to MenuValue(fraction = (hudScaleNow - xendroid.compose.core.HudPlacements.MIN_SCALE) / HUD_SCALE_SPAN, steps = 0,
                text = percent.format(hudScaleNow.toDouble())),
            InGameAction.HUD_OPACITY to MenuValue(fraction = style.opacity, steps = 0, text = percent.format(style.opacity.toDouble()),
                enabled = hudLook.value == xendroid.compose.core.HudLook.BOX),
            InGameAction.HUD_COLORS to MenuValue(fraction = style.colors, steps = 0, text = percent.format(style.colors.toDouble())),
            // Controls
            InGameAction.TOUCH_CONTROLS to MenuValue(on = showTouchOverlay.value == true && !overlayHiddenByController.value, own = TOUCH_KEY in own),
            InGameAction.CONTROL_STYLE to MenuValue(options = listOf(stringResource(R.string.menu_opt_modern), stringResource(R.string.menu_opt_classic)),
                selected = xendroid.compose.gamepad.ControlStyle.parse(cfg.globals.style).ordinal),
            InGameAction.ADAPTIVE_STICKS to MenuValue(on = adaptiveSticks.value, enabled = activeTitleState.value != null, own = true),
            InGameAction.TOUCH_CAMERA to MenuValue(on = touchCamera.value, text = stringResource(R.string.menu_t_touch_camera_note)),
            InGameAction.EDIT_TOUCH_LAYOUT to MenuValue(text = stringResource(R.string.menu_t_edit_layout_note)),
            InGameAction.SPLIT_SCREEN to MenuValue(text = stringResource(xendroid.compose.gamepad.splitScreenLabel(
                xendroid.compose.gamepad.SplitScreenMode.parse(cfg.globals.splitScreen)))),
            InGameAction.CONTROLLER_RUMBLE to MenuValue(text = rumbleNames.getValue(rumbleSettings.value.default),
                note = controllerSlots.players.withIndex()
                    .filter { it.value != null && !it.value!!.startsWith(xendroid.compose.companion.CompanionHost.KEY_PREFIX) }
                    .joinToString(", ") { player ->
                        // U08: a controller with its own intensity says it.
                        "P${player.index + 1}" + (player.value?.let { rumbleSettings.value.perDevice[it] }?.let { " (${rumbleNames.getValue(it)})" } ?: "")
                    }.ifEmpty { stringResource(R.string.menu_no_controller) }),
            InGameAction.PHONE_CONTROLLERS to MenuValue(text = phoneControllersLabel.value ?: stringResource(R.string.phone_ctl_off)),
            InGameAction.UNBUFFERED_INPUT to MenuValue(on = unbufferedInput.value),
            InGameAction.GYRO_CAMERA to MenuValue(on = gyroEnabled.value, enabled = gyroAvailable,
                text = if (!gyroAvailable) stringResource(R.string.menu_gyro_unavailable) else null),
            InGameAction.GYRO_AIM to MenuValue(text = stringResource(when (gyroAim.value) {
                xendroid.compose.gamepad.GyroAim.ALWAYS -> R.string.menu_gyro_aim_always
                xendroid.compose.gamepad.GyroAim.WHILE_LT -> R.string.menu_gyro_aim_lt
                xendroid.compose.gamepad.GyroAim.WHILE_LB -> R.string.menu_gyro_aim_lb
            }), enabled = gyroAvailable),
            InGameAction.GYRO_SENSITIVITY to MenuValue(text = listOf(stringResource(R.string.menu_low), stringResource(R.string.menu_normal),
                stringResource(R.string.menu_high))[gyroSensitivity.intValue.coerceIn(0, 2)], enabled = gyroAvailable),
            InGameAction.GYRO_CALIBRATE to MenuValue(text = stringResource(R.string.menu_t_gyro_calibrate_note), enabled = gyroAvailable),
            // Session
            InGameAction.VOLUME to MenuValue(fraction = audioVolume.intValue / 100f, steps = 0, text = "${audioVolume.intValue}%", own = VOLUME_KEY in own),
            InGameAction.MUTE to MenuValue(on = audioVolume.intValue == 0),
            InGameAction.AUTO_SAVE to MenuValue(on = autoSave.value, text = if (autoSave.value)
                stringResource(R.string.menu_t_autosave_on, loadingName ?: activeTitleState.value ?: stringResource(R.string.app_name)) else stringResource(R.string.menu_t_autosave_off)),
            InGameAction.UNDO_SESSION to MenuValue(enabled = sessionChanges.intValue > 0, text = if (sessionChanges.intValue > 0)
                pluralStringResource(R.plurals.menu_undo_note, sessionChanges.intValue, sessionChanges.intValue) else stringResource(R.string.menu_nothing_changed)),
            InGameAction.MAKE_GLOBAL to MenuValue(enabled = sessionChanges.intValue > 0, text = stringResource(R.string.menu_t_global_note)),
            InGameAction.PAUSE_ON_OPEN to MenuValue(on = pauseOnOpen.value, text = stringResource(R.string.menu_t_pause_note)),
            InGameAction.MARK_SCENE to MenuValue(text = stringResource(R.string.menu_t_mark_note, sceneMarkers.intValue)),
        )
        val developer = menuState.value.developer
        val graphicsNotes = buildList {
            if (developer) {
                add(presentation.label)
                fgBudgetLabel.value?.let(::add)
                addAll(fgNotes.value)
            }
            add(stringResource(R.string.menu_fg_experimental))
            add(stringResource(R.string.menu_image_live_note))
        }
        val notes = mapOf(
            InGamePage.GRAPHICS to graphicsNotes,
            InGamePage.HUD to if (developer) listOf(stringResource(R.string.menu_system_note)) else emptyList(),
            InGamePage.CONTROLS to listOfNotNull(phoneControllersDetails.value, stringResource(R.string.menu_adaptive_note),
                stringResource(R.string.menu_phones_note)),
            InGamePage.SESSION to listOf("${BuildConfig.VERSION_NAME}\n${gpuLabel.value.ifEmpty { "GPU information unavailable" }}"),
        )
        return InGameMenuModel(
            values = values, gameName = loadingName, art = loadingArt.value, paused = menuPaused.value, status = menuStatus.value,
            notes = notes, logSessions = menuLogSessions.value, savedChanges = if (autoSave.value) sessionChanges.intValue else 0,
            hudPreview = if (performanceOverlayEnabled.value) xendroid.compose.ui.ingame.HudPreview(hudDetail.value, hudMetrics.value, hudLook.value,
                style, hudScaleNow) else null,
        )
    }

    @androidx.compose.runtime.Composable
    private fun fpsLabel(fps: Int): String = if (fps == 0) stringResource(R.string.menu_unlimited) else stringResource(R.string.menu_fps, fps)

    /** Round 2: A on a row of the in-game menu, or a tap on it: a choice takes the next one, a cycle
     *  its next value, a chip (the controller's) flips, a switch flips, a button runs. */
    private fun performMenuAction(action: InGameAction) {
        when (action.kind) {
            RowKind.CHOICE -> { chooseMenuOption(action, (choiceIndex(action) + 1) % choiceCount(action)); return }
            RowKind.CYCLE -> { adjustMenuAction(action, 1); return }
            RowKind.MULTI -> { chooseMenuOption(action, menuState.value.chip); return }
            RowKind.SLIDER -> return
            else -> {}
        }
        if (action == InGameAction.MORE_OPTIONS) {
            menuState.value = menuState.value.toggleAdvanced()
            return
        }
        when (action) {
            InGameAction.PERFORMANCE_HINTS -> performanceHints.requested = !performanceHints.requested
            InGameAction.EXTERNAL_DISPLAY -> externalDisplay?.cycle()
            InGameAction.GYRO_CAMERA -> if (gyroCamera.available) {
                gyroEnabled.value = !gyroEnabled.value
                val on = gyroEnabled.value
                saveControlOptions { it.copy(gyroCamera = on) }
            }
            InGameAction.GYRO_CALIBRATE -> gyroCamera.calibrate()
            InGameAction.UNBUFFERED_INPUT -> {
                unbufferedInput.value = !unbufferedInput.value
                val unbuffered = unbufferedInput.value
                saveControlOptions { it.copy(unbufferedInput = unbuffered) }
                applyUnbufferedInput()
                recordEvent("input", "unbuffered " + if (unbufferedInput.value) "on" else "off")
            }
            InGameAction.PHONE_CONTROLLERS -> {
                // The native driver takes player slots only once the emulator runs a title.
                if (phoneControllers.host == null && activeTitleState.value == null) {
                    Toast.makeText(this, getString(R.string.host_phone_wait), Toast.LENGTH_SHORT).show()
                    return
                }
                phoneControllers.toggle()
                refreshPhoneControllers()
            }
            InGameAction.SMOOTH_SHADERS -> toggleGpuLive(xendroid.compose.core.GpuLiveOption.ASYNC_SKIP_DRAWS)
            InGameAction.MSAA_4X_AS_2X -> toggleGpuLive(xendroid.compose.core.GpuLiveOption.MSAA_4X_AS_2X)
            InGameAction.CUTOUT_TRANSPARENCY -> toggleGpuLive(xendroid.compose.core.GpuLiveOption.ALPHA_TO_COVERAGE_AS_TEST)
            InGameAction.SCREENSHOT -> takeScreenshot()
            InGameAction.SUSTAINED_PERFORMANCE -> {
                val enabled = !sustainedMode.value
                sustainedAvailable.value = sustainedPerformance(this, enabled)
                if (sustainedAvailable.value) sustainedMode.value = enabled
            }
            InGameAction.MUTE -> {
                val current = session.audioVolume()
                val volume = if (current == 0) volumeBeforeMute.takeIf { it > 0 } ?: 100 else { volumeBeforeMute = current; 0 }
                session.setAudioVolume(volume); audioVolume.intValue = session.audioVolume()
            }
            InGameAction.IMPORT_LSFG_DLL -> if (!importingLsfg) lsfgPicker.launch(arrayOf("application/octet-stream", "application/x-msdownload", "*/*"))
            InGameAction.CLEAR_LSFG_CACHE -> {
                if (session.presentationState().engine == 1) {
                    session.setFrameGeneration(false, fgPreset.intValue, currentOutputHz())
                    restoreGenerationCap()
                    releaseFrameGenerationRefresh()
                }
                lifecycleScope.launch {
                    val deleted = withContext(Dispatchers.IO) { runCatching { LsfgAssets.clear(applicationContext) } }
                    if (deleted.isSuccess) lsfgCache.value = null
                    else Toast.makeText(this@EmulatorHostActivity, getString(R.string.host_cache_not_removed), Toast.LENGTH_LONG).show()
                    presentationState.value = session.presentationState()
                }
            }
            InGameAction.LSFG -> runLsfg(action)
            InGameAction.WINFG -> runWinFg(action)
            InGameAction.EDIT_TOUCH_LAYOUT -> {
                if (session.booted && !session.isPaused()) {
                    session.pause()
                    menuState.value = menuState.value.copy(pausedByMenu = true)
                }
                menuPaused.value = session.isPaused()
                editorOpen.value = true
            }
            InGameAction.STRETCH -> {
                if (fpsConfig.value.loading || fpsConfig.value.saving || fpsConfig.value.error != null) return
                val enabled = !fullscreenStretchEnabled.value
                val before = fpsConfig.value
                fpsConfig.value = before.copy(saving = true)
                lifecycleScope.launch {
                    val saved = withContext(Dispatchers.IO) { persistFullscreenStretchConfig(enabled) }
                    if (saved) {
                        fullscreenStretchEnabled.value = enabled
                        getSharedPreferences(DISPLAY_SETTINGS_PREFS, MODE_PRIVATE).edit()
                            .putBoolean(FULLSCREEN_STRETCH_KEY, enabled).apply()
                    } else {
                        Toast.makeText(this@EmulatorHostActivity, getString(R.string.host_display_not_saved), Toast.LENGTH_LONG).show()
                    }
                    fpsConfig.value = before
                }
            }
            InGameAction.PERFORMANCE_HUD -> {
                val enabled = !performanceOverlayEnabled.value
                performanceOverlayEnabled.value = enabled
                getSharedPreferences("fps_overlay", MODE_PRIVATE).edit()
                    .putBoolean("performance_overlay_enabled", enabled).apply()
            }
            InGameAction.TOUCH_CONTROLS -> {
                val before = showTouchOverlay.value == true
                rememberLive(TOUCH_KEY, before)
                toggleTouchOverlay()
                val after = showTouchOverlay.value == true
                // Round 2: the setting itself, kept for this game; showing them over a controller is not a change.
                if (after != before) keepChange(TOUCH_KEY, after.toString())
            }
            InGameAction.ADAPTIVE_STICKS -> {
                val title = session.activeTitleId() ?: return
                val enabled = !adaptiveSticks.value
                adaptiveSticks.value = enabled
                controlsTitleId = title
                getSharedPreferences("touch_options", MODE_PRIVATE).edit()
                    .putBoolean("adaptive_$title", enabled).apply()
            }
            InGameAction.DRIVER_INFO -> Toast.makeText(this, getString(R.string.menu_driver_note), Toast.LENGTH_LONG).show()
            InGameAction.MARK_SCENE -> {
                sceneMarkers.intValue++
                recordEvent("marker", "scene ${sceneMarkers.intValue}", flush = true)
                Toast.makeText(this, getString(R.string.host_scene_marked, sceneMarkers.intValue), Toast.LENGTH_SHORT).show()
            }
            InGameAction.TOUCH_CAMERA -> {
                touchCamera.value = !touchCamera.value
                val on = touchCamera.value
                saveControlOptions { it.copy(touchCamera = on) }
            }
            InGameAction.PAUSE_ON_OPEN -> setPauseOnOpen(!pauseOnOpen.value)
            InGameAction.AUTO_SAVE -> setAutoSave(!autoSave.value)
            InGameAction.UNDO_SESSION -> undoSessionChanges()
            InGameAction.MAKE_GLOBAL -> makeChangesGlobal()
            InGameAction.RESUME -> closeMenuAndResume()
            InGameAction.SHARE_LOGS -> lifecycleScope.launch {
                menuLogSessions.value = withContext(Dispatchers.IO) { runCatching { SessionLogs.sessions() }.getOrDefault(emptyList()) }
                menuState.value = menuState.value.showLogs(menuLogSessions.value.size)
            }
            InGameAction.QUIT -> menuState.value = menuState.value.askToQuit()
            else -> Unit
        }
    }

    /** Round 2: ←→ on a row of the menu, or a tap on its ‹ ›: the value beside. */
    private fun adjustMenuAction(action: InGameAction, delta: Int) {
        val step = if (delta < 0) -1 else 1
        when (action.kind) {
            RowKind.CHOICE -> chooseMenuOption(action, (choiceIndex(action) + step).coerceIn(0, choiceCount(action) - 1))
            RowKind.MULTI -> menuState.value = menuState.value.moveChip(step, hudChipMetrics().size)
            RowKind.TOGGLE -> if (toggleOn(action) != (step > 0)) performMenuAction(action)
            RowKind.SLIDER -> { setMenuValue(action, sliderFraction(action) + step * sliderStep(action)); finishMenuValue(action) }
            RowKind.CYCLE -> cycleMenuValue(action, step)
            else -> {}
        }
    }

    /** Round 2: a pill or a chip of the menu chosen by touch (or a choice's next by A). */
    private fun chooseMenuOption(action: InGameAction, index: Int) {
        when (action) {
            InGameAction.DISPLAY_MODE -> setDisplayMode(index.coerceIn(0, 3))
            InGameAction.FPS_LIMIT -> FPS_CHOICES.getOrNull(index)?.let(::setMenuFps)
            InGameAction.HUD_LAYOUT -> updateHudStyle { it.copy(layout = xendroid.compose.core.HudLayout.entries[index.coerceIn(0, 1)]) }
            InGameAction.HUD_POSITION -> updateHudStyle { it.copy(edge = xendroid.compose.core.HudEdge.entries[index.coerceIn(0, 1)]) }
            InGameAction.HUD_STYLE -> setHudDetail(HUD_DETAILS[index.coerceIn(0, HUD_DETAILS.size - 1)])
            InGameAction.HUD_LOOK -> setHudLook(xendroid.compose.core.HudLook.entries[index.coerceIn(0, 2)])
            InGameAction.CONTROL_STYLE -> setControlStyle(xendroid.compose.gamepad.ControlStyle.entries[index.coerceIn(0, 1)])
            InGameAction.HUD_METRICS -> {
                menuState.value = menuState.value.copy(chip = index)
                toggleHudChip(index)
            }
            else -> {}
        }
    }

    /** Round 2: a slider of the menu moved to [fraction] (0..1): applied at once. */
    private fun setMenuValue(action: InGameAction, fraction: Float) {
        val f = fraction.coerceIn(0f, 1f)
        when (action) {
            InGameAction.HUD_SIZE -> {
                val scale = ((xendroid.compose.core.HudPlacements.MIN_SCALE + f * HUD_SCALE_SPAN) * 10).roundToInt() / 10f
                val store = HudPreferences.of(this)
                val title = activeTitleState.value
                xendroid.compose.core.HudPlacements.write(store, title, xendroid.compose.core.HudPlacements.read(store, title).copy(scale = scale))
                hudScale.value = scale
            }
            InGameAction.HUD_OPACITY -> updateHudStyle { it.copy(opacity = (f * 10).roundToInt() / 10f) }
            InGameAction.HUD_COLORS -> updateHudStyle { it.copy(colors = (f * 10).roundToInt() / 10f) }
            InGameAction.VOLUME -> {
                val volume = (f * 20).roundToInt() * 5
                rememberLive(VOLUME_KEY, session.audioVolume())
                session.setAudioVolume(volume)
                audioVolume.intValue = session.audioVolume()
            }
            else -> {}
        }
    }

    /** Round 2: a slider let go (or stepped by a controller): the volume is kept for this game. */
    private fun finishMenuValue(action: InGameAction) {
        if (action == InGameAction.VOLUME) keepChange(VOLUME_KEY, audioVolume.intValue.toString())
    }

    private fun choiceIndex(action: InGameAction): Int = when (action) {
        InGameAction.DISPLAY_MODE -> presentationState.value.displayMode.coerceIn(0, 3)
        InGameAction.FPS_LIMIT -> FPS_CHOICES.indexOf(fpsLimitState.intValue).coerceAtLeast(0)
        InGameAction.HUD_LAYOUT -> hudStyle.value.layout.ordinal
        InGameAction.HUD_POSITION -> hudStyle.value.edge.ordinal
        InGameAction.HUD_STYLE -> HUD_DETAILS.indexOf(hudDetail.value).coerceAtLeast(0)
        InGameAction.HUD_LOOK -> hudLook.value.ordinal
        InGameAction.CONTROL_STYLE -> controlStyle.value.ordinal
        else -> 0
    }

    private fun choiceCount(action: InGameAction): Int = when (action) {
        InGameAction.DISPLAY_MODE -> 4
        InGameAction.FPS_LIMIT -> FPS_CHOICES.size
        InGameAction.HUD_STYLE -> HUD_DETAILS.size
        InGameAction.HUD_LOOK -> xendroid.compose.core.HudLook.entries.size
        else -> 2
    }

    private fun toggleOn(action: InGameAction): Boolean = when (action) {
        InGameAction.STRETCH -> fullscreenStretchEnabled.value
        InGameAction.WINFG -> presentationState.value.let { it.requested && it.engine == 0 }
        InGameAction.LSFG -> presentationState.value.let { it.requested && it.engine == 1 }
        InGameAction.SUSTAINED_PERFORMANCE -> sustainedMode.value
        InGameAction.SMOOTH_SHADERS -> gpuValue(xendroid.compose.core.GpuLiveOption.ASYNC_SKIP_DRAWS) != 0
        InGameAction.MSAA_4X_AS_2X -> gpuValue(xendroid.compose.core.GpuLiveOption.MSAA_4X_AS_2X) != 0
        InGameAction.CUTOUT_TRANSPARENCY -> gpuValue(xendroid.compose.core.GpuLiveOption.ALPHA_TO_COVERAGE_AS_TEST) != 0
        InGameAction.PERFORMANCE_HINTS -> performanceHints.requested
        InGameAction.PERFORMANCE_HUD -> performanceOverlayEnabled.value
        InGameAction.TOUCH_CONTROLS -> showTouchOverlay.value == true && !overlayHiddenByController.value
        InGameAction.ADAPTIVE_STICKS -> adaptiveSticks.value
        InGameAction.TOUCH_CAMERA -> touchCamera.value
        InGameAction.UNBUFFERED_INPUT -> unbufferedInput.value
        InGameAction.GYRO_CAMERA -> gyroEnabled.value
        InGameAction.MUTE -> audioVolume.intValue == 0
        InGameAction.PAUSE_ON_OPEN -> pauseOnOpen.value
        InGameAction.AUTO_SAVE -> autoSave.value
        else -> false
    }

    private fun sliderFraction(action: InGameAction): Float = when (action) {
        InGameAction.HUD_SIZE -> ((hudScale.value ?: xendroid.compose.core.HudPlacements.read(HudPreferences.of(this), activeTitleState.value).scale) -
            xendroid.compose.core.HudPlacements.MIN_SCALE) / HUD_SCALE_SPAN
        InGameAction.HUD_OPACITY -> hudStyle.value.opacity
        InGameAction.HUD_COLORS -> hudStyle.value.colors
        InGameAction.VOLUME -> audioVolume.intValue / 100f
        else -> 0f
    }

    /** One step of a slider for ←→: 10% of the HUD's size, opacity and colours; 5 points of volume. */
    private fun sliderStep(action: InGameAction): Float = when (action) {
        InGameAction.HUD_SIZE -> 0.1f / HUD_SCALE_SPAN
        InGameAction.VOLUME -> 0.05f
        else -> 0.1f
    }

    /** Round 2: a value of a ‹ › row of the menu, the previous ([step] -1) or the next. */
    private fun cycleMenuValue(action: InGameAction, step: Int) {
        fun <T> around(list: List<T>, current: T): T = list[(list.indexOf(current).coerceAtLeast(0) + step + list.size) % list.size]
        when (action) {
            InGameAction.SHADING_RATE -> setGpuLive(xendroid.compose.core.GpuLiveOption.SHADING_RATE, around((0..3).toList(), gpuValue(xendroid.compose.core.GpuLiveOption.SHADING_RATE)))
            InGameAction.SCALING_EFFECT -> {
                rememberLive(SCALING_KEY, scalingEffect.intValue)
                scalingEffect.intValue = around((-1..5).toList(), scalingEffect.intValue)
                session.setScalingEffect(scalingEffect.intValue)
                keepChange(SCALING_KEY, xendroid.compose.core.ImageTuning.scalingValue(scalingEffect.intValue))
            }
            InGameAction.ANTIALIASING -> applyImageTuning(imageTuning.value.copy(antialiasing = around((-1..2).toList(), imageTuning.value.antialiasing)))
            InGameAction.SHARPNESS -> applyImageTuning(imageTuning.value.copy(sharpness = around((-1..4).toList(), imageTuning.value.sharpness)))
            InGameAction.DITHER -> applyImageTuning(imageTuning.value.copy(dither = around(listOf(-1, 1, 0), imageTuning.value.dither)))
            InGameAction.COLOR_FILTER -> {
                val current = session.presentationState().colorFilter
                rememberLive(InGameChanges.COLOR_FILTER, current)
                val next = around((0..4).toList(), current)
                session.setColorFilter(next)
                presentationState.value = session.presentationState()
                keepChange(InGameChanges.COLOR_FILTER, next.toString())
            }
            InGameAction.TV_MARGIN -> {
                val choices = xendroid.compose.core.TvMargin.CHOICES
                tvMargin.floatValue = around(choices, choices.minByOrNull { kotlin.math.abs(it - tvMargin.floatValue) } ?: choices.first())
                getSharedPreferences(DISPLAY_SETTINGS_PREFS, MODE_PRIVATE).edit().putFloat("tv_margin_percent", tvMargin.floatValue).apply()
                externalDisplay?.setMargin(tvMargin.floatValue)
            }
            InGameAction.REFRESH_RATE -> {
                @Suppress("DEPRECATION") val activeDisplay = if (Build.VERSION.SDK_INT >= 30) display else windowManager.defaultDisplay
                if (activeDisplay != null) {
                    val choices = listOf<Float?>(null) + refreshChoices(activeDisplay).map { it.hz }
                    rememberLive(InGameChanges.REFRESH_HZ, requestedRefresh.value)
                    requestedRefresh.value = around(choices, requestedRefresh.value)
                    selectRefresh(this, requestedRefresh.value)
                    keepChange(InGameChanges.REFRESH_HZ, requestedRefresh.value?.toString())
                }
            }
            InGameAction.BACKGROUND_POLICY -> backgroundPolicy.value = around(BackgroundPolicy.entries, backgroundPolicy.value)
            InGameAction.GYRO_AIM -> if (gyroCamera.available) {
                gyroAim.value = around(xendroid.compose.gamepad.GyroAim.entries, gyroAim.value)
                val aim = gyroAim.value
                saveControlOptions { it.copy(gyroAim = aim) }
            }
            InGameAction.GYRO_SENSITIVITY -> {
                val next = around(xendroid.compose.gamepad.GyroSensitivity.entries, xendroid.compose.gamepad.GyroSensitivity.entries[gyroSensitivity.intValue])
                gyroSensitivity.intValue = next.ordinal
                gyroCamera.sensitivity = next.scale
                saveControlOptions { it.copy(gyroSensitivity = next) }
            }
            InGameAction.CONTROLLER_RUMBLE -> {
                rumbleSettings.value = rumbleSettings.value.copy(default = around(xendroid.compose.gamepad.RumbleIntensity.entries, rumbleSettings.value.default))
                val rumble = rumbleSettings.value.default
                saveControlOptions { it.copy(rumble = rumble) }
                stopRumble()
            }
            InGameAction.SPLIT_SCREEN -> lifecycleScope.launch {
                // Saved with the touch controls (as the editor's "Split screen"): off, at a fold, always.
                runCatching {
                    gamepad.update { cfg ->
                        val mode = xendroid.compose.gamepad.SplitScreenMode.parse(cfg.globals.splitScreen)
                        cfg.copy(globals = cfg.globals.copy(splitScreen = around(xendroid.compose.gamepad.SplitScreenMode.entries, mode).key))
                    }
                }.onFailure { Log.w(TAG, "Saving the split screen mode failed", it) }
            }
            InGameAction.WINFG_PRESET -> {
                fgPreset.intValue = around(listOf(0, 1, 2), fgPreset.intValue)
                runWinFg(action)
            }
            InGameAction.LSFG_MULTIPLIER, InGameAction.LSFG_TARGET -> runLsfg(action)
            else -> {}
        }
    }

    /** Frame generation by LSFG: on or off, a multiplier by hand or an output target (15d). */
    private fun runLsfg(action: InGameAction) {
        when (action) {
            InGameAction.LSFG_MULTIPLIER -> {
                // A multiplier chosen by hand ends the target (15d).
                lsfgTarget.intValue = xendroid.compose.core.FrameGenerationTarget.OFF
                lsfgMultiplier.intValue = if (lsfgMultiplier.intValue == 4) 2 else lsfgMultiplier.intValue + 1
            }
            InGameAction.LSFG_TARGET -> lsfgTarget.intValue = xendroid.compose.core.FrameGenerationTarget.next(lsfgTarget.intValue)
            else -> {}
        }
        val cache = lsfgCache.value ?: return
        val current = session.presentationState()
        val running = current.requested && current.engine == 1
        if (action != InGameAction.LSFG && !running) return
        val enabled = if (action == InGameAction.LSFG) !running else true
        // 15d: a target picks the multiplier and caps the game at target ÷ multiplier, planned
        // from the player's own limit (never from a cap frame generation put there), against
        // what the display can do rather than the rate it shows right now.
        val plan = xendroid.compose.core.FrameGenerationTarget.plan(lsfgTarget.intValue,
            requestedRefresh.value ?: displayRates().maxOrNull() ?: currentOutputHz(),
            generationCap.playerLimit(session.fpsLimit()))
        if (plan != null) lsfgMultiplier.intValue = plan.multiplier
        val hz = when {
            !enabled -> currentOutputHz()
            plan != null -> frameGenerationRefresh(plan.output.toDouble())
            else -> frameGenerationRefresh(guestFpsNow() * lsfgMultiplier.intValue)
        }
        when {
            !enabled -> { restoreGenerationCap(); releaseFrameGenerationRefresh() }
            plan != null -> prepareExactCap(plan.cap)
            else -> prepareGenerationCap(hz, lsfgMultiplier.intValue)
        }
        session.setLsfg(enabled, cache, hz, lsfgMultiplier.intValue)
        if (enabled) recordEvent("lsfg", plan?.let { "target ${it.target}/s: ${it.multiplier}x from ${it.cap} FPS" }
            ?: "${lsfgMultiplier.intValue}x by hand")
        presentationState.value = session.presentationState()
    }

    /** Frame generation by Win-FG: on or off, or its preset while it runs. */
    private fun runWinFg(action: InGameAction) {
        val state = session.presentationState()
        if (action == InGameAction.WINFG_PRESET && state.engine == 1) return
        val enabled = if (action == InGameAction.WINFG) !(state.requested && state.engine == 0) else state.requested
        val hz = if (enabled) frameGenerationRefresh(guestFpsNow() * 2) else currentOutputHz()
        if (enabled) prepareGenerationCap(hz) else { restoreGenerationCap(); releaseFrameGenerationRefresh() }
        session.setFrameGeneration(enabled, fgPreset.intValue, hz)
        presentationState.value = session.presentationState()
    }

    /** The display's rates at its current resolution, lowest first. */
    private fun displayRates(): List<Float> =
        ((if (Build.VERSION.SDK_INT >= 30) display else @Suppress("DEPRECATION") windowManager.defaultDisplay)
            ?.let { refreshChoices(it).map { choice -> choice.hz } }).orEmpty()

    /** The game's frame rate now, or the last one seen running (the menu pauses it). */
    private fun guestFpsNow(): Double = session.averageFps().takeIf { it > 0 } ?: lastGuestFps.takeIf { it > 0 } ?: 30.0

    /**
     * The display rate frame generation gets for [outputFps] frames a second: the player's own
     * choice, or with Auto the display's lowest rate that holds them, asked of the display until
     * [releaseFrameGenerationRefresh]. Not the rate it shows right now: HyperOS's dynamic refresh
     * lowers it while the picture is still (the menu open), and generation then judged the game
     * too fast for the display and switched itself off.
     */
    private fun frameGenerationRefresh(outputFps: Double): Float {
        requestedRefresh.value?.let { return it }
        val rates = displayRates()
        val hz = rates.firstOrNull { it * 1.025f >= outputFps } ?: rates.maxOrNull() ?: return currentOutputHz()
        selectRefresh(this, hz)
        return hz
    }

    private fun releaseFrameGenerationRefresh() {
        selectRefresh(this, requestedRefresh.value)
    }

    private fun setDisplayMode(mode: Int) {
        rememberLive(InGameChanges.DISPLAY_MODE, presentationState.value.displayMode)
        session.setPresentationMode(mode)
        presentationState.value = session.presentationState()
        keepChange(InGameChanges.DISPLAY_MODE, mode.toString())
    }

    private fun setMenuFps(fps: Int) {
        rememberLive(FPS_KEY, session.fpsLimit())
        // Explicit manual changes supersede a temporary automatic FG cap.
        generationCap.forget()
        fpsLimitState.intValue = fps
        session.setFpsLimit(fps)
        keepChange(FPS_KEY, fps.toString())
    }

    private fun updateHudStyle(change: (xendroid.compose.core.HudStyle) -> xendroid.compose.core.HudStyle) {
        val next = change(hudStyle.value)
        hudStyle.value = next
        xendroid.compose.core.HudStyle.write(HudPreferences.of(this), next)
    }

    private fun setHudDetail(detail: xendroid.compose.core.HudDetail) {
        hudDetail.value = detail
        if (detail != xendroid.compose.core.HudDetail.PANEL) panelSnapshot.value = null
        getSharedPreferences("fps_overlay", MODE_PRIVATE).edit().putString("performance_overlay_detail", detail.key).apply()
    }

    private fun setHudLook(look: xendroid.compose.core.HudLook) {
        // 15g: kept with this game's HUD place and size.
        val store = HudPreferences.of(this)
        val title = activeTitleState.value
        xendroid.compose.core.HudPlacements.write(store, title, xendroid.compose.core.HudPlacements.read(store, title).copy(look = look))
        hudLook.value = look
    }

    /** The HUD's chips in the menu: its metrics, then the FPS graph (null), then (developer) Vulkan submissions. */
    private fun hudChipMetrics(): List<HudMetric?> = listOf(HudMetric.CPU, HudMetric.GPU, HudMetric.RAM, HudMetric.GPU_MEMORY,
        HudMetric.BATTERY_TEMPERATURE, HudMetric.SOC_TEMPERATURE, HudMetric.POWER, HudMetric.BATTERY_LEVEL, HudMetric.BATTERY_TIME, null) +
        if (menuState.value.developer) listOf(HudMetric.HOST_SUBMISSIONS) else emptyList()

    private fun toggleHudChip(index: Int) {
        val chips = hudChipMetrics()
        if (index !in chips.indices) return
        val metric = chips[index]
        if (metric == null) { updateHudStyle { it.copy(graph = !it.graph) }; return }
        val enabled = hudMetrics.value.toMutableSet()
        if (!enabled.remove(metric)) enabled.add(metric)
        hudMetrics.value = enabled.toSet()
        getSharedPreferences("fps_overlay", MODE_PRIVATE).edit()
            .putStringSet("hud_metrics", enabled.map { it.name }.toSet()).apply()
    }

    private fun setControlStyle(style: xendroid.compose.gamepad.ControlStyle) {
        controlStyle.value = style
        lifecycleScope.launch {
            runCatching { gamepad.update { cfg -> cfg.copy(globals = cfg.globals.copy(style = style.key)) } }
                .onFailure { Log.w(TAG, "Saving the touch controls' look failed", it) }
        }
    }

    private fun setPauseOnOpen(on: Boolean) {
        pauseOnOpen.value = on
        inGamePrefs.pauseOnOpen = on
        if (!menuState.value.open) return
        // Applies to the menu open now too: the game stops, or goes on behind the menu.
        if (on && session.booted && !session.isPaused()) {
            session.pause()
            menuState.value = menuState.value.copy(pausedByMenu = true)
        } else if (!on && menuState.value.pausedByMenu) {
            menuState.value = menuState.value.copy(pausedByMenu = false)
            pausedByLifecycle = true
            resumeForLifecycle()
        }
        menuPaused.value = session.isPaused()
    }

    private fun setAutoSave(on: Boolean) {
        autoSave.value = on
        inGamePrefs.autoSave = on
        val changes = inGameChanges ?: return
        if (on) changesScope.launch {
            runCatching { changes.keepAll() }.onFailure { Log.w(TAG, "Keeping this session's changes failed", it) }
            withContext(Dispatchers.Main) { refreshGameOwnKeys() }
        }
    }

    /** Round 2: a new title runs: its changes start empty, and its own display mode, colour filter and refresh rate apply. */
    private fun startGameChanges(title: String?) {
        liveBefore.clear()
        sessionChanges.intValue = 0
        gameOwnKeys.value = emptySet()
        inGameChanges = title?.let { InGameChanges(InGameChangesStore(ConfigStore(applicationContext), inGamePrefs, it)) }
        if (title == null) return
        inGamePrefs.value(InGameChanges.DISPLAY_MODE, title)?.toIntOrNull()?.takeIf { it in 0..3 }?.let { session.setPresentationMode(it) }
        inGamePrefs.value(InGameChanges.COLOR_FILTER, title)?.toIntOrNull()?.takeIf { it in 0..4 }?.let { session.setColorFilter(it) }
        inGamePrefs.value(InGameChanges.REFRESH_HZ, title)?.toFloatOrNull()?.let { hz ->
            requestedRefresh.value = hz
            selectRefresh(this, hz)
        }
        presentationState.value = session.presentationState()
        // The menu has no button over the game any more: how to open it, once per install.
        if (inGamePrefs.firstOpenTip()) Toast.makeText(this, getString(R.string.menu_open_tip), Toast.LENGTH_LONG).show()
    }

    /** Round 2: [key]'s live value before this session first changed it, for Undo. */
    private fun rememberLive(key: String, value: Any?) {
        if (key !in liveBefore) liveBefore[key] = value
    }

    /** Round 2: a value the menu changed, recorded for the game, and kept for it at once while "keep" is on. */
    private fun keepChange(key: String, raw: String?) {
        val changes = inGameChanges ?: return
        val keep = autoSave.value
        changesScope.launch {
            val count = runCatching { changes.change(key, raw, keep); changes.count }
            withContext(Dispatchers.Main) {
                count.onSuccess { sessionChanges.intValue = it }.onFailure {
                    Log.w(TAG, "Keeping $key for the game failed", it)
                    Toast.makeText(this@EmulatorHostActivity, getString(R.string.host_changes_not_saved), Toast.LENGTH_SHORT).show()
                }
                if (keep) refreshGameOwnKeys()
                if (key == FPS_KEY) refreshFpsConfig()
            }
        }
    }

    /** Round 2: which of the menu's settings this game has its own value for ("this game" in the menu). */
    private fun refreshGameOwnKeys() {
        val title = activeTitleState.value ?: return
        changesScope.launch {
            val own = runCatching {
                val handle = ConfigStore(applicationContext).openGameConfig(title)
                try { MENU_CONFIG_KEYS.filter { key -> key.split('|', limit = 2).let { (s, n) -> handle.getString(s, n) != null } }.toSet() }
                finally { handle.closeDiscard() }
            }.getOrDefault(emptySet()) + listOf(InGameChanges.DISPLAY_MODE, InGameChanges.COLOR_FILTER, InGameChanges.REFRESH_HZ)
                .filter { inGamePrefs.gameValue(it, title) != null }
            withContext(Dispatchers.Main) { if (activeTitleState.value == title) gameOwnKeys.value = own }
        }
    }

    /** Round 2: this session's changes undone: the game's own settings and the live values as they were. */
    private fun undoSessionChanges() {
        val changes = inGameChanges ?: return
        changesScope.launch {
            val undone = runCatching { changes.undo() }.onFailure { Log.w(TAG, "Undoing this session's changes failed", it) }
            withContext(Dispatchers.Main) {
                liveBefore.toMap().forEach { (key, value) -> restoreLive(key, value) }
                liveBefore.clear()
                sessionChanges.intValue = 0
                refreshGameOwnKeys()
                refreshFpsConfig()
                Toast.makeText(this@EmulatorHostActivity, getString(if (undone.isSuccess) R.string.host_changes_undone else R.string.host_config_not_saved),
                    Toast.LENGTH_SHORT).show()
            }
        }
    }

    /** Round 2: this session's changes become every game's (the global config and preferences). */
    private fun makeChangesGlobal() {
        val changes = inGameChanges ?: return
        changesScope.launch {
            val made = runCatching { changes.makeGlobal() }.onFailure { Log.w(TAG, "Making this session's changes global failed", it) }
            withContext(Dispatchers.Main) {
                if (made.isSuccess) {
                    liveBefore.clear()
                    sessionChanges.intValue = 0
                }
                refreshGameOwnKeys()
                refreshFpsConfig()
                Toast.makeText(this@EmulatorHostActivity, getString(if (made.isSuccess) R.string.host_changes_global else R.string.host_config_not_saved),
                    Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun restoreLive(key: String, value: Any?) {
        when (key) {
            FPS_KEY -> (value as? Int)?.let { generationCap.forget(); session.setFpsLimit(it); fpsLimitState.intValue = session.fpsLimit() }
            SCALING_KEY -> (value as? Int)?.let { scalingEffect.intValue = it; session.setScalingEffect(it) }
            IMAGE_LIVE -> (value as? xendroid.compose.core.ImageTuning)?.let { imageTuning.value = it; session.setImageTuning(it) }
            TOUCH_KEY -> (value as? Boolean)?.let { on -> if ((showTouchOverlay.value == true) != on) toggleTouchOverlay() }
            VOLUME_KEY -> (value as? Int)?.let { session.setAudioVolume(it); audioVolume.intValue = session.audioVolume() }
            InGameChanges.DISPLAY_MODE -> (value as? Int)?.let { session.setPresentationMode(it); presentationState.value = session.presentationState() }
            InGameChanges.COLOR_FILTER -> (value as? Int)?.let { session.setColorFilter(it); presentationState.value = session.presentationState() }
            InGameChanges.REFRESH_HZ -> { requestedRefresh.value = value as? Float; selectRefresh(this, requestedRefresh.value) }
            else -> xendroid.compose.core.GpuLiveOption.entries.firstOrNull { it.key == key }?.let { option ->
                (value as? Int)?.let { gpuLive.value = gpuLive.value + (option to it); session.setLiveOption(option, it) }
            }
        }
    }

    private fun gpuValue(option: xendroid.compose.core.GpuLiveOption): Int = gpuLive.value[option] ?: option.default

    /** A GPU option from the in-game menu: in the core from its next frame, and kept for the game. */
    private fun setGpuLive(option: xendroid.compose.core.GpuLiveOption, value: Int) {
        rememberLive(option.key, gpuValue(option))
        gpuLive.value = gpuLive.value + (option to value)
        session.setLiveOption(option, value)
        keepChange(option.key, if (option == xendroid.compose.core.GpuLiveOption.SHADING_RATE) value.toString()
            else xendroid.compose.settings.ConfigValueShape.bool(value != 0))
        recordEvent("gpu option", "${option.key} = $value")
    }

    private fun toggleGpuLive(option: xendroid.compose.core.GpuLiveOption) = setGpuLive(option, if (gpuValue(option) != 0) 0 else 1)

    /** The game's picture as its surface holds it (no menu, no HUD), saved to Pictures/Xendroid+. */
    private fun takeScreenshot() {
        val view = surfaceView ?: return
        if (view.width <= 0 || view.height <= 0) return
        val bitmap = android.graphics.Bitmap.createBitmap(view.width, view.height, android.graphics.Bitmap.Config.ARGB_8888)
        android.view.PixelCopy.request(view, bitmap, { result ->
            lifecycleScope.launch {
                val saved = result == android.view.PixelCopy.SUCCESS && withContext(Dispatchers.IO) {
                    runCatching { saveScreenshot(bitmap) }.onFailure { Log.w(TAG, "Saving the screenshot failed", it) }.isSuccess
                }
                bitmap.recycle()
                if (!saved) Log.w(TAG, "Screenshot not taken (PixelCopy result $result)")
                Toast.makeText(this@EmulatorHostActivity,
                    getString(if (saved) R.string.host_screenshot_saved else R.string.host_screenshot_failed), Toast.LENGTH_SHORT).show()
            }
        }, android.os.Handler(mainLooper))
    }

    private fun saveScreenshot(bitmap: android.graphics.Bitmap) {
        val stamp = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date())
        val values = android.content.ContentValues().apply {
            put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, "Xendroid+_${activeTitleState.value ?: "game"}_$stamp.png")
            put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
            put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/Xendroid+")
            put(android.provider.MediaStore.Images.Media.IS_PENDING, 1)
        }
        val resolver = contentResolver
        val uri = resolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
            ?: error("MediaStore refused the entry")
        try {
            resolver.openOutputStream(uri)?.use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
                ?: error("MediaStore gave no stream")
            values.clear()
            values.put(android.provider.MediaStore.Images.Media.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
        } catch (t: Throwable) {
            resolver.delete(uri, null, null)
            throw t
        }
    }

    /** The Image options tried from the in-game menu: the core applies them from the next frame, and the game keeps them. */
    private fun applyImageTuning(next: xendroid.compose.core.ImageTuning) {
        rememberLive(IMAGE_LIVE, imageTuning.value)
        val before = imageTuning.value.cvarValues()
        imageTuning.value = next
        session.setImageTuning(next)
        recordEvent("image", "aa ${next.antialiasing} sharpness ${next.sharpness} dither ${next.dither}")
        next.cvarValues().forEach { (key, raw) -> if (before[key] != raw) keepChange(key, raw) }
    }

    /** U01: what loaded (from the presenter) against the driver setting then and now. */
    private fun refreshDriverLine() {
        val active = session.activeDriverIdentity()
        val title = session.activeTitleId()
        lifecycleScope.launch {
            val now = withContext(Dispatchers.IO) { runCatching { inGameConfig.driverPath(title) }.getOrNull() }
            driverLine.value = xendroid.compose.driver.DriverIdentity.inGame(driverAtBoot.value, now, active) to (active?.label ?: "")
        }
    }

    private fun refreshFpsConfig() {
        if (fpsConfig.value.loading || fpsConfig.value.saving) return
        val title = session.activeTitleId()
        fpsConfig.value = FpsConfigSnapshot(titleId = title, loading = true)
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { inGameConfig.fpsSnapshot(title) } }
            fpsConfig.value = result.getOrElse {
                Log.w(TAG, "Reading saved FPS configuration failed", it)
                FpsConfigSnapshot(titleId = title, error = getString(R.string.host_config_unreadable))
            }
        }
    }

    private fun chooseLogSession(index: Int) {
        if (index == menuLogSessions.value.size + 1) {
            menuState.value = menuState.value.closeLogs(); return
        }
        val sessionId = if (index == 0) null else menuLogSessions.value.getOrNull(index - 1)?.id ?: return
        shareSessionLogs(sessionId)
    }

    private fun shareSessionLogs(selectedSession: String? = null) {
        if (sharingLogs) return
        sharingLogs = true
        if (session.booted && !session.isPaused()) {
            session.pause()
            menuState.value = menuState.value.copy(pausedByMenu = true)
        }
        lifecycleScope.launch {
            try {
                val file = withContext(Dispatchers.IO) {
                    runCatching { SessionLogs.exportRedactedForSharing(applicationContext, selectedSession) }
                        .onFailure { Log.w(TAG, "Sharing diagnostics failed", it) }.getOrNull()
                }
                if (file == null) {
                    Toast.makeText(this@EmulatorHostActivity, getString(R.string.host_no_diagnostics), Toast.LENGTH_SHORT).show()
                    return@launch
                }
                runCatching {
                    val uri = FileProvider.getUriForFile(
                        this@EmulatorHostActivity, "$packageName.share", file,
                    )
                    val send = Intent(Intent.ACTION_SEND).apply {
                        type = "application/zip"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        clipData = ClipData.newRawUri(getString(R.string.host_share_diagnostics), uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    startActivity(Intent.createChooser(send, getString(R.string.host_share_diagnostics)))
                }.onFailure {
                    Log.w(TAG, "Opening diagnostics share sheet failed", it)
                    Toast.makeText(this@EmulatorHostActivity, getString(R.string.host_share_failed), Toast.LENGTH_SHORT).show()
                }
            } finally {
                sharingLogs = false
            }
        }
    }

    /** Neutralize a held controller before routing its next events to the menu. */
    private fun releaseGuestInput() {
        consumedMenuKeys.addAll(sentGuestKeys.keys)
        sentGuestKeys.values.forEach { session.keyEvent(it, false, KEY_VALUE_UNUSED) }
        sentGuestKeys.clear()
        for (code in KC_LTHUMB_LEFT..KC_RTHUMB_DOWN) {
            if (axisPressed[code]) emitAxis(code, false, 0)
        }
        if (lTriggerDown) session.keyEvent(KC_TRIGGER_L, false, KEY_VALUE_UNUSED)
        if (rTriggerDown) session.keyEvent(KC_TRIGGER_R, false, KEY_VALUE_UNUSED)
        lTriggerDown = false
        rTriggerDown = false
        if (hatLeft) session.keyEvent(KC_DPAD_LEFT, false, KEY_VALUE_UNUSED)
        if (hatRight) session.keyEvent(KC_DPAD_RIGHT, false, KEY_VALUE_UNUSED)
        if (hatUp) session.keyEvent(KC_DPAD_UP, false, KEY_VALUE_UNUSED)
        if (hatDown) session.keyEvent(KC_DPAD_DOWN, false, KEY_VALUE_UNUSED)
        hatLeft = false
        hatRight = false
        hatUp = false
        hatDown = false
        panelNavPrev = false
        panelNavNext = false
        slotInput.releaseAll()
        stopRumble()
    }

    private fun isPanelKey(
        keyCode: Int
    ): Boolean =
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_L1,
            KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_MODE -> true

            else -> false
        }

    /** U04: keys mean what [xendroid.compose.gamepad.MenuButtons] says (A/B may be swapped);
     *  Esc used to activate the selection here (it was listed under confirm first). */
    private fun panelKeyDown(
        nav: PanelNav,
        keyCode: Int
    ): Boolean {
        // Round 2: in the menu's rows, left and right change the row's value; up and down move.
        if (menuRowsActive() && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)) {
            adjustSelectedRow(if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) -1 else 1)
            return true
        }
        return when (xendroid.compose.gamepad.MenuButtons.intentOf(keyCode, swapConfirm)) {
            xendroid.compose.gamepad.MenuButtons.Intent.PREVIOUS -> {
                movePanelSelection(nav, -1)
                true
            }
            xendroid.compose.gamepad.MenuButtons.Intent.NEXT -> {
                movePanelSelection(nav, 1)
                true
            }
            xendroid.compose.gamepad.MenuButtons.Intent.CONFIRM -> {
                nav.activate(
                    if (menuState.value.open && !hasGuestPrompt()) menuState.value.selected
                    else panelSelectedState.intValue
                )
                true
            }
            xendroid.compose.gamepad.MenuButtons.Intent.CANCEL -> {
                nav.cancel()
                true
            }
            else -> false
        }
    }

    /** The menu is open on its rows (not asking to quit or for a log): ←→ adjust there. */
    private fun menuRowsActive(): Boolean =
        menuState.value.open && !hasGuestPrompt() && !menuState.value.confirmingQuit && !menuState.value.logPicker

    private fun adjustSelectedRow(delta: Int) {
        menuState.value.action?.let { adjustMenuAction(it, delta) }
    }

    private var panelNavLeft = false
    private var panelNavRight = false

    /** U04: the menu button layout (read once per game) and the pace of a held direction. */
    private val swapConfirm by lazy { xendroid.compose.gamepad.MenuButtonPrefs.swapConfirm(this) }
    private val navRepeat = xendroid.compose.gamepad.NavRepeat()

    private var panelNavPrev = false
    private var panelNavNext = false

    private fun panelHat(
        nav: PanelNav,
        event: MotionEvent
    ) {
        if (menuRowsActive()) {
            val x = event.getAxisValue(MotionEvent.AXIS_HAT_X) + event.getAxisValue(MotionEvent.AXIS_X)
            val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y) + event.getAxisValue(MotionEvent.AXIS_Y)
            val now = SystemClock.uptimeMillis()
            val left = x < -0.5f && kotlin.math.abs(x) > kotlin.math.abs(y)
            val right = x > 0.5f && kotlin.math.abs(x) > kotlin.math.abs(y)
            if (left != panelNavLeft) { panelNavLeft = left; if (left && navRepeat.press(-2, now)) adjustSelectedRow(-1) }
            if (right != panelNavRight) { panelNavRight = right; if (right && navRepeat.press(2, now)) adjustSelectedRow(1) }
            val up = y < -0.5f && !left && !right
            val down = y > 0.5f && !left && !right
            if (up != panelNavPrev) { panelNavPrev = up; if (up && navRepeat.press(-1, now)) movePanelSelection(nav, -1) }
            if (down != panelNavNext) { panelNavNext = down; if (down && navRepeat.press(1, now)) movePanelSelection(nav, 1) }
            if (!panelNavPrev && !panelNavNext && !panelNavLeft && !panelNavRight) navRepeat.release()
            return
        }
        val y =
            event.getAxisValue(
                MotionEvent.AXIS_HAT_Y
            ) +
                event.getAxisValue(
                    MotionEvent.AXIS_Y
                )

        val x =
            event.getAxisValue(
                MotionEvent.AXIS_HAT_X
            ) +
                event.getAxisValue(
                    MotionEvent.AXIS_X
                )

        val prev =
            y < -0.5f ||
                x < -0.5f

        val next =
            y > 0.5f ||
                x > 0.5f

        if (prev != panelNavPrev) {
            panelNavPrev = prev
            if (prev && navRepeat.press(-1, SystemClock.uptimeMillis())) movePanelSelection(nav, -1)
        }

        if (next != panelNavNext) {
            panelNavNext = next
            if (next && navRepeat.press(1, SystemClock.uptimeMillis())) movePanelSelection(nav, 1)
        }
        if (!panelNavPrev && !panelNavNext) navRepeat.release()
    }

    private fun movePanelSelection(
        nav: PanelNav,
        delta: Int
    ) {
        if (nav.count <= 0) return

        if (menuState.value.open && !hasGuestPrompt()) {
            menuState.value = menuState.value.move(delta)
            return
        }

        val next =
            panelSelectedState.intValue +
                delta

        panelSelectedState.intValue =
            (
                (next % nav.count) +
                    nav.count
                ) % nav.count
    }

    private fun answerMessageBox(
        button: Int
    ) {
        val req =
            messageBoxRequestState.value
                ?: return

        session.messageBoxSubmit(
            req.id,
            button
        )

        messageBoxRequestState.value =
            null
    }

    private fun chooseDisc(
        path: String
    ) {
        val req =
            discRequestState.value
                ?: return

        session.discSubmit(
            req.id,
            true,
            path
        )
        discInDrive = path

        discRequestState.value =
            null
    }

    private fun cancelDisc() {
        val req =
            discRequestState.value
                ?: return

        session.discSubmit(
            req.id,
            false,
            ""
        )

        discRequestState.value =
            null
    }

    private fun isControllerEvent(event: KeyEvent): Boolean =
        event.source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            event.source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK

    /**
     * U10: the guest keyboard with a controller, Xbox 360 style: D-pad/stick move the highlight,
     * confirm types it, X deletes, Y adds a space, LB/RB move the cursor, L3 shift, R3 symbols,
     * Start answers, Back/Select cancels; cancel (B) deletes, and cancels once the text is empty.
     */
    private fun keyboardKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val grid = keyboardGrid.value
        val step = when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> 0 to -1
            KeyEvent.KEYCODE_DPAD_DOWN -> 0 to 1
            KeyEvent.KEYCODE_DPAD_LEFT -> -1 to 0
            KeyEvent.KEYCODE_DPAD_RIGHT -> 1 to 0
            else -> null
        }
        if (step != null) {
            if (event.repeatCount == 0) gridRepeat.release()
            if (gridRepeat.press(keyCode, event.eventTime)) keyboardGrid.value = grid.move(step.first, step.second)
            return true
        }
        if (event.repeatCount > 0) return true
        when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_X -> keyboardGrid.value = grid.backspace()
            KeyEvent.KEYCODE_BUTTON_Y -> keyboardGrid.value = grid.type(" ")
            KeyEvent.KEYCODE_BUTTON_L1 -> keyboardGrid.value = grid.caretLeft()
            KeyEvent.KEYCODE_BUTTON_R1 -> keyboardGrid.value = grid.caretRight()
            KeyEvent.KEYCODE_BUTTON_THUMBL -> keyboardGrid.value = grid.toggleShift()
            KeyEvent.KEYCODE_BUTTON_THUMBR -> keyboardGrid.value = grid.togglePage()
            KeyEvent.KEYCODE_BUTTON_START -> acceptKeyboard(grid.text)
            KeyEvent.KEYCODE_BUTTON_SELECT -> cancelKeyboard()
            else -> when (xendroid.compose.gamepad.MenuButtons.intentOf(keyCode, swapConfirm)) {
                xendroid.compose.gamepad.MenuButtons.Intent.CONFIRM -> {
                    val (next, outcome) = grid.press()
                    when (outcome) {
                        xendroid.compose.ui.keyboard.KeyboardGrid.Outcome.DONE -> acceptKeyboard(next.text)
                        xendroid.compose.ui.keyboard.KeyboardGrid.Outcome.CANCEL -> cancelKeyboard()
                        xendroid.compose.ui.keyboard.KeyboardGrid.Outcome.NONE -> keyboardGrid.value = next
                    }
                }
                xendroid.compose.gamepad.MenuButtons.Intent.CANCEL ->
                    if (grid.text.isEmpty()) cancelKeyboard() else keyboardGrid.value = grid.backspace()
                else -> return false
            }
        }
        return true
    }

    /** U10: the stick or hat over the keyboard grid, in two dimensions, at the menu pace. */
    private fun keyboardHat(event: MotionEvent) {
        val x = event.getAxisValue(MotionEvent.AXIS_HAT_X) + event.getAxisValue(MotionEvent.AXIS_X)
        val y = event.getAxisValue(MotionEvent.AXIS_HAT_Y) + event.getAxisValue(MotionEvent.AXIS_Y)
        val dx = if (x < -0.5f) -1 else if (x > 0.5f) 1 else 0
        val dy = if (y < -0.5f) -1 else if (y > 0.5f) 1 else 0
        val direction = (dy + 1) * 3 + (dx + 1)
        if (direction == gridHatDirection) return
        gridHatDirection = direction
        gridRepeat.release()
        if (direction != 4 && gridRepeat.press(direction, SystemClock.uptimeMillis())) {
            keyboardGrid.value = keyboardGrid.value.move(dx, dy)
        }
    }

    private fun acceptKeyboard(
        text: String
    ) {
        val req =
            keyboardRequestState.value
                ?: return

        // U10: valid UTF-16 only (a lone surrogate would become invalid UTF-8 in the guest).
        session.keyboardSubmit(
            req.id,
            true,
            xendroid.compose.ui.keyboard.KeyboardGrid.sanitize(text)
        )

        keyboardRequestState.value =
            null
    }

    private fun cancelKeyboard() {
        val req =
            keyboardRequestState.value
                ?: return

        session.keyboardSubmit(
            req.id,
            false,
            ""
        )

        keyboardRequestState.value =
            null
    }
}

@Composable
private fun rememberOverlayContrast(
    brightness: StateFlow<Float>
): State<Float> {
    val anim =
        remember {
            Animatable(0f)
        }

    LaunchedEffect(brightness) {
        brightness.collect { b ->
            val target =
                (
                    (b - 0.5f) / 0.4f
                )
                    .coerceIn(
                        0f,
                        1f
                    )
                    .let {
                        (
                            it * 8f
                        )
                            .roundToInt() / 8f
                    }

            if (
                target !=
                    anim.targetValue
            ) {
                launch {
                    anim.animateTo(
                        target,
                        tween(400)
                    )
                }
            }
        }
    }

    return anim.asState()
}
