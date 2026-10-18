package xendroid.compose.ui.design

import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import xendroid.compose.R

@Immutable
data class XdColors(
    val bg: Color,
    val s1: Color,
    val s2: Color,
    val s3: Color,
    val s4: Color,
    val sheet: Color,
    val line: Color,
    val line2: Color,
    val fg: Color,
    val fg2: Color,
    val fg3: Color,
    val acc: Color,
    val onAcc: Color,
    val dyn: Color,
    val controller: Boolean,
) {
    val playable = Color(0xFF6FA8F0)
    val ingame = Color(0xFFE8C55A)
    val intro = Color(0xFFEC9B52)
    val boots = Color(0xFFE8765C)
    val nothing = Color(0xFFE4555C)
    val none = Color(0xFF7D8981)
    val errText = Color(0xFFFF9EA2)
    val dangerText = Color(0xFFFFB3B5)
    val exposed = Color(0xFF80B8FF)
    val changed = Color(0xFFC7A0FF)
    val warn get() = ingame
    val ok get() = playable
    val coverRadius: Dp get() = if (controller) 12.dp else 7.dp

    companion object {
        val Touch = XdColors(
            bg = Color(0xFF0F1214), s1 = Color(0xFF15191C), s2 = Color(0xFF1B2024), s3 = Color(0xFF252B30),
            s4 = Color(0xFF30373D), sheet = Color(0xFF171C1F),
            line = Color(0x14CDDCE8), line2 = Color(0x29CDDCE8),
            fg = Color(0xFFEDF1F3), fg2 = Color(0xFFB2BDC4), fg3 = Color(0xFF7E8A92),
            acc = Color(0xFF639CE8), onAcc = Color(0xFF071A2A), dyn = Color(0xFF1D3C4F), controller = false,
        )
        val Controller = XdColors(
            bg = Color(0xFF050706), s1 = Color(0x11FFFFFF), s2 = Color(0x1AFFFFFF), s3 = Color(0x26FFFFFF),
            s4 = Color(0x38FFFFFF), sheet = Color(0xFF121715),
            line = Color(0x1AFFFFFF), line2 = Color(0x33FFFFFF),
            fg = Color(0xFFF5F8F6), fg2 = Color(0xC7F5F8F6), fg3 = Color(0x8FF5F8F6),
            acc = Color(0xFF7FB8E3), onAcc = Color(0xFF081620), dyn = Color(0xFF1D3C4F), controller = true,
        )

        fun of(mode: InputMode) = if (mode == InputMode.CONTROLLER) Controller else Touch
    }

    fun withCover(dyn: Color, accent: Color): XdColors {
        var acc = accent
        if (acc.luminance() < 0.3f) acc = lerp(acc, Color.White, 0.45f)
        val on = if (acc.luminance() > 0.42f) Color(0xFF0B0F0D) else Color.White
        return copy(dyn = dyn, acc = acc, onAcc = on)
    }

    fun solid(c: Color): Color = c.compositeOver(bg)

    fun status(s: CompatTone): Color = when (s) {
        CompatTone.PLAYABLE -> playable
        CompatTone.INGAME -> ingame
        CompatTone.INTRO -> intro
        CompatTone.BOOTS -> boots
        CompatTone.NOTHING -> nothing
        CompatTone.NONE -> none
    }
}

/** The five ratings of the compatibility scale, plus "not rated". */
enum class CompatTone { PLAYABLE, INGAME, INTRO, BOOTS, NOTHING, NONE }

fun lerp(a: Color, b: Color, t: Float) = Color(
    red = a.red + (b.red - a.red) * t,
    green = a.green + (b.green - a.green) * t,
    blue = a.blue + (b.blue - a.blue) * t,
    alpha = a.alpha + (b.alpha - a.alpha) * t,
)

object XdFonts {
    val body = FontFamily(
        Font(R.font.barlow_regular, FontWeight.Normal),
        Font(R.font.barlow_medium, FontWeight.Medium),
        Font(R.font.barlow_semibold, FontWeight.SemiBold),
        Font(R.font.barlow_bold, FontWeight.Bold),
    )
    val display = FontFamily(
        Font(R.font.barlow_semi_condensed_semibold, FontWeight.SemiBold),
        Font(R.font.barlow_semi_condensed_bold, FontWeight.Bold),
    )
    @OptIn(androidx.compose.ui.text.ExperimentalTextApi::class)
    val mono = FontFamily(
        Font(R.font.jetbrains_mono, FontWeight.Normal, variationSettings = FontVariation.Settings(FontVariation.weight(400))),
        Font(R.font.jetbrains_mono, FontWeight.Medium, variationSettings = FontVariation.Settings(FontVariation.weight(500))),
        Font(R.font.jetbrains_mono, FontWeight.SemiBold, variationSettings = FontVariation.Settings(FontVariation.weight(600))),
    )
}

object XdText {
    private fun s(family: FontFamily, size: TextUnit, weight: FontWeight, line: TextUnit = TextUnit.Unspecified,
                  spacing: TextUnit = TextUnit.Unspecified) =
        TextStyle(fontFamily = family, fontSize = size, fontWeight = weight, lineHeight = line, letterSpacing = spacing)

    val body = s(XdFonts.body, 14.sp, FontWeight.Normal, 19.6.sp)
    val bodySm = s(XdFonts.body, 13.sp, FontWeight.Normal, 18.sp)
    val note = s(XdFonts.body, 12.5.sp, FontWeight.Normal, 17.sp)
    val small = s(XdFonts.body, 12.sp, FontWeight.Normal, 16.sp)
    val tiny = s(XdFonts.body, 11.5.sp, FontWeight.Normal, 15.sp)
    val label = s(XdFonts.body, 14.sp, FontWeight.SemiBold, 18.sp)
    val labelSm = s(XdFonts.body, 13.sp, FontWeight.SemiBold, 17.sp)
    val button = s(XdFonts.body, 13.5.sp, FontWeight.SemiBold, 16.sp)
    val chip = s(XdFonts.body, 12.5.sp, FontWeight.SemiBold, 15.sp)
    val eyebrow = s(XdFonts.body, 11.5.sp, FontWeight.Bold, 14.sp, 0.13.em)
    val cardHead = s(XdFonts.body, 11.5.sp, FontWeight.Bold, 14.sp, 0.1.em)
    val railLabel = s(XdFonts.body, 10.sp, FontWeight.SemiBold, 12.sp)
    val h1 = s(XdFonts.display, 21.sp, FontWeight.Bold, 22.sp)
    val h2 = s(XdFonts.display, 22.sp, FontWeight.Bold, 24.sp)
    val h2c = s(XdFonts.display, 23.sp, FontWeight.Bold, 25.sp)
    val hero = s(XdFonts.display, 34.sp, FontWeight.Bold, 35.sp)
    val heroSm = s(XdFonts.display, 26.sp, FontWeight.Bold, 27.sp)
    val sheetTitle = s(XdFonts.display, 22.sp, FontWeight.Bold, 24.sp)
    val kpi = s(XdFonts.display, 24.sp, FontWeight.Bold, 25.sp)
    val blade = s(XdFonts.display, 15.5.sp, FontWeight.SemiBold, 16.sp, 0.06.em)
    val mono = s(XdFonts.mono, 11.5.sp, FontWeight.Medium, 15.sp)
    val monoSm = s(XdFonts.mono, 10.5.sp, FontWeight.Medium, 13.sp)
    val monoNum = s(XdFonts.mono, 11.sp, FontWeight.SemiBold, 13.sp)
}

val LocalXd = compositionLocalOf { XdColors.Touch }

object Xd {
    val colors: XdColors
        @Composable @ReadOnlyComposable get() = LocalXd.current
    val controller: Boolean
        @Composable @ReadOnlyComposable get() = LocalXd.current.controller
}

private fun scheme(c: XdColors): ColorScheme {
    val s1 = c.solid(c.s1)
    val s2 = c.solid(c.s2)
    val s3 = c.solid(c.s3)
    return darkColorScheme(
        primary = c.acc, onPrimary = c.onAcc,
        primaryContainer = c.acc.copy(alpha = 0.2f).compositeOver(s2), onPrimaryContainer = c.fg,
        inversePrimary = Color(0xFF107C10),
        secondary = c.fg2, onSecondary = c.bg,
        secondaryContainer = s3, onSecondaryContainer = c.fg,
        tertiary = c.exposed, onTertiary = c.bg,
        tertiaryContainer = c.exposed.copy(alpha = 0.2f).compositeOver(s2), onTertiaryContainer = c.fg,
        background = c.bg, onBackground = c.fg,
        surface = c.bg, onSurface = c.fg,
        surfaceVariant = s2, onSurfaceVariant = c.fg2,
        surfaceTint = Color.Transparent,
        inverseSurface = c.fg, inverseOnSurface = c.bg,
        error = c.errText, onError = Color(0xFF3A0508),
        errorContainer = c.nothing.copy(alpha = 0.22f).compositeOver(s2), onErrorContainer = c.dangerText,
        outline = c.solid(c.line2), outlineVariant = c.solid(c.line),
        scrim = Color.Black,
        surfaceBright = s3, surfaceDim = c.bg,
        surfaceContainerLowest = c.bg, surfaceContainerLow = s1, surfaceContainer = s1,
        surfaceContainerHigh = c.sheet, surfaceContainerHighest = s3,
    )
}

private val typography = Typography(
    displayLarge = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 48.sp, lineHeight = 50.sp),
    displayMedium = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 40.sp, lineHeight = 42.sp),
    displaySmall = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 34.sp, lineHeight = 36.sp),
    headlineLarge = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 30.sp, lineHeight = 32.sp),
    headlineMedium = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 26.sp, lineHeight = 28.sp),
    headlineSmall = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 23.sp, lineHeight = 25.sp),
    titleLarge = TextStyle(fontFamily = XdFonts.display, fontWeight = FontWeight.Bold, fontSize = 22.sp, lineHeight = 24.sp),
    titleMedium = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.SemiBold, fontSize = 15.sp, lineHeight = 20.sp),
    titleSmall = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.SemiBold, fontSize = 13.5.sp, lineHeight = 18.sp),
    bodyLarge = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.Normal, fontSize = 15.sp, lineHeight = 21.sp),
    bodyMedium = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.Normal, fontSize = 14.sp, lineHeight = 19.6.sp),
    bodySmall = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.Normal, fontSize = 12.5.sp, lineHeight = 17.sp),
    labelLarge = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.SemiBold, fontSize = 13.5.sp, lineHeight = 17.sp),
    labelMedium = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.SemiBold, fontSize = 12.5.sp, lineHeight = 16.sp),
    labelSmall = TextStyle(fontFamily = XdFonts.body, fontWeight = FontWeight.SemiBold, fontSize = 11.sp, lineHeight = 14.sp),
)

@Composable
fun XdTheme(mode: InputMode, colors: XdColors? = null, content: @Composable () -> Unit) {
    val c = colors ?: XdColors.of(mode)
    val scheme = remember(c) { scheme(c) }
    CompositionLocalProvider(LocalXd provides c, LocalInputMode provides mode) {
        MaterialTheme(colorScheme = scheme, typography = typography, content = content)
    }
}

@Composable
fun WithCoverColors(dyn: Color?, accent: Color?, content: @Composable () -> Unit) {
    val base = LocalXd.current
    if (!base.controller || dyn == null || accent == null) { content(); return }
    val tinted = remember(base, dyn, accent) { base.withCover(dyn, accent) }
    val scheme = remember(tinted) { scheme(tinted) }
    CompositionLocalProvider(LocalXd provides tinted) {
        MaterialTheme(colorScheme = scheme, typography = typography, content = content)
    }
}
