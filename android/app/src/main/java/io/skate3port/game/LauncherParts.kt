package io.skate3port.game

import androidx.compose.animation.Crossfade
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Icon, wordmark and one line of positioning. The only centred block on the screen. */
@Composable
fun BrandHeader() {
    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Image(
            painter = painterResource(R.mipmap.ic_launcher),
            contentDescription = null,
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .size(64.dp)
                .clip(RoundedCornerShape(18.dp)),
        )
        Text(
            "SKATE 3",
            color = Skate3.TextPrimary,
            fontSize = 34.sp,
            fontWeight = FontWeight.Black,
            letterSpacing = 2.sp,
            modifier = Modifier.padding(top = Skate3.GapM),
        )
        Text(
            "ANDROID PORT",
            color = Skate3.Orange,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 3.sp,
            modifier = Modifier.padding(top = Skate3.GapXs),
        )
    }
}

/**
 * Three-step rail. [current] is 1-based; anything below it reads as done.
 * Turns an opaque installer into a visible sequence.
 */
@Composable
fun StepRail(current: Int, modifier: Modifier = Modifier) {
    val labels = listOf("Select ISO", "Title update", "Play")
    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceEvenly,
        verticalAlignment = Alignment.Top,
    ) {
        labels.forEachIndexed { index, label ->
            val step = index + 1
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                StepDot(step = step, current = current)
                Text(
                    label,
                    color = when {
                        step == current -> Skate3.TextPrimary
                        step < current -> Skate3.TextSecondary
                        else -> Skate3.TextFaint
                    },
                    fontSize = 11.sp,
                    fontWeight = if (step == current) FontWeight.SemiBold else FontWeight.Normal,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(top = Skate3.GapS),
                )
            }
        }
    }
}

@Composable
private fun StepDot(step: Int, current: Int) {
    val done = step < current
    val active = step == current
    Box(
        modifier = Modifier
            .size(28.dp)
            .clip(CircleShape)
            .background(
                when {
                    done -> Skate3.Success.copy(alpha = 0.16f)
                    active -> Skate3.Orange
                    else -> Skate3.Card
                }
            ),
        contentAlignment = Alignment.Center,
    ) {
        if (done) {
            Icon(
                Icons.Filled.Check,
                contentDescription = null,
                tint = Skate3.Success,
                modifier = Modifier.size(16.dp),
            )
        } else {
            Text(
                step.toString(),
                color = if (active) Color.Black else Skate3.TextFaint,
                fontSize = 13.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/**
 * The "what is happening now" panel. Headline centred, body left-aligned —
 * centred prose is harder to read once it wraps.
 */
@Composable
fun StatusCard(headline: String, detail: String, progress: Float?, modifier: Modifier = Modifier) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(Skate3.CornerCard),
        colors = CardDefaults.cardColors(containerColor = Skate3.Card),
        border = BorderStroke(1.dp, Skate3.CardBorder),
    ) {
        Column(modifier = Modifier.padding(Skate3.GapL)) {
            // Only the headline fades. Detail and progress must track live, or a
            // progress tick every 150ms would restart the animation.
            Crossfade(targetState = headline, label = "headline") { text ->
                Text(
                    text,
                    color = Skate3.TextPrimary,
                    fontSize = 19.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 0.5.sp,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center,
                )
            }
            if (detail.isNotBlank()) {
                Text(
                    detail,
                    color = Skate3.TextSecondary,
                    fontSize = 14.sp,
                    lineHeight = 20.sp,
                    modifier = Modifier.padding(top = Skate3.GapM),
                )
            }
            if (progress != null) {
                Row(
                    modifier = Modifier.padding(top = Skate3.GapL),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    LinearProgressIndicator(
                        progress = { progress },
                        color = Skate3.Orange,
                        trackColor = Skate3.CardBorder,
                        strokeCap = StrokeCap.Round,
                        modifier = Modifier
                            .weight(1f)
                            .height(8.dp),
                    )
                    Text(
                        "${(progress * 100).toInt()}%",
                        color = Skate3.TextSecondary,
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier.padding(start = Skate3.GapM),
                    )
                }
            }
        }
    }
}

/**
 * One dark palette, built around the port's orange. Kept deliberately narrow:
 * a background, two raised surfaces, one accent, and three text weights.
 */
object Skate3 {
    val Orange = Color(0xFFFF6818)
    val Background = Color(0xFF08080A)
    val Card = Color(0xFF141418)
    val CardBorder = Color(0xFF232329)
    val Success = Color(0xFF4ADE80)

    val TextPrimary = Color(0xFFF5F5F7)
    val TextSecondary = Color(0xFFA1A1AA)
    val TextFaint = Color(0xFF6B6B75)

    /** 8dp rhythm. Every gap in the launcher is one of these. */
    val GapXs = 4.dp
    val GapS = 8.dp
    val GapM = 16.dp
    val GapL = 24.dp
    val GapXl = 40.dp

    val CornerCard = 20.dp
    val CornerButton = 14.dp

    /** Keeps the column readable on tablets and unfolded devices. */
    val MaxContentWidth = 440.dp
}

@Composable
fun Skate3Theme(content: @Composable () -> Unit) {
    MaterialTheme(
        // Only these two are read: TextButton/OutlinedButton take their content
        // colour from primary, OutlinedButton its border from outline. Every other
        // colour on the screen is passed explicitly at the call site.
        colorScheme = darkColorScheme(
            primary = Skate3.Orange,
            outline = Skate3.CardBorder,
        ),
        content = content,
    )
}
