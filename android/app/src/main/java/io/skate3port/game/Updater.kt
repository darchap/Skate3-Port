@file:JvmName("Updater")
package io.skate3port.game

import android.content.Context
import android.content.pm.PackageManager
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets

/**
 * Asks GitHub whether a newer release exists. The stable channel is the latest
 * non-prerelease, the beta channel is simply the newest release of either kind,
 * so a beta tester still receives stable releases.
 */

private const val PREFERENCES = "updater"
private const val CHANNEL_PREFERENCE = "channel"
private const val LAST_CHECK_PREFERENCE = "last_check"
private const val RELEASES = "https://api.github.com/repos/darchap/Skate3-Port/releases"
private const val AUTO_CHECK_INTERVAL_MILLIS = 24L * 60 * 60 * 1000
private const val RESPONSE_LIMIT = 512 * 1024
private const val TIMEOUT_MILLIS = 15000

enum class UpdateChannel(val label: String) {
    STABLE("Stable"),
    BETA("Beta"),
}

/** [version] is the tag with any leading "v" removed, for display. */
class Release(
    @JvmField val version: String,
    @JvmField val notes: String,
    @JvmField val pageUrl: String,
)

fun channel(context: Context): UpdateChannel {
    val stored = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
        .getString(CHANNEL_PREFERENCE, null)
    return UpdateChannel.entries.firstOrNull { it.name == stored } ?: UpdateChannel.STABLE
}

fun setChannel(context: Context, channel: UpdateChannel) {
    context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
        .putString(CHANNEL_PREFERENCE, channel.name).apply()
}

fun installedVersion(context: Context): String =
    try {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty()
    } catch (exception: PackageManager.NameNotFoundException) {
        ""
    }

/** True once a day, so a launch that is offline or rate-limited costs nothing. */
fun shouldAutoCheck(context: Context): Boolean {
    val last = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
        .getLong(LAST_CHECK_PREFERENCE, 0)
    val now = System.currentTimeMillis()
    return now < last || now - last >= AUTO_CHECK_INTERVAL_MILLIS
}

/** Only the silent check moves the window, so "Check now" cannot cancel tomorrow's. */
fun markAutoChecked(context: Context) {
    context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
        .putLong(LAST_CHECK_PREFERENCE, System.currentTimeMillis()).apply()
}

/**
 * Blocking; call from a worker thread. Returns the release to offer, or null
 * when this build is current. A release older than the installed build is not
 * offered: Android refuses to install it, and uninstalling first would delete
 * the extracted game files.
 */
@Throws(IOException::class)
fun checkForUpdate(context: Context): Release? {
    // Without a version of our own every release looks newer, so offer nothing.
    val installed = installedVersion(context)
    if (installed.isBlank()) return null
    val release = try {
        when (channel(context)) {
            UpdateChannel.STABLE -> parseRelease(JSONObject(fetch("$RELEASES/latest")))
            UpdateChannel.BETA -> newestRelease(JSONArray(fetch("$RELEASES?per_page=10")))
        }
    } catch (exception: JSONException) {
        // A captive portal answers 200 with a login page; unread, this kills the process.
        throw IOException("GitHub sent a reply this app could not read.", exception)
    }
    if (release == null) return null
    return if (compareVersions(release.version, installed) > 0) release else null
}

internal fun newestRelease(releases: JSONArray): Release? {
    var newest: Release? = null
    for (i in 0 until releases.length()) {
        val entry = releases.optJSONObject(i) ?: continue
        val candidate = parseRelease(entry) ?: continue
        if (newest == null || compareVersions(candidate.version, newest.version) > 0) {
            newest = candidate
        }
    }
    return newest
}

private fun parseRelease(release: JSONObject): Release? {
    val tag = release.optString("tag_name").ifBlank { return null }
    val page = release.optString("html_url").ifBlank { return null }
    // optString answers "null" for a JSON null, which GitHub sends for empty notes.
    val notes = if (release.isNull("body")) "" else release.optString("body")
    return Release(displayVersion(tag), plainText(notes), page)
}

/**
 * Release notes are written as plain paragraphs (TASKS.md has the rules) so one
 * text reads correctly both on GitHub and here, where nothing renders Markdown.
 * The trailing Installing / SHA-256 block is for someone downloading the file by
 * hand and is not shown.
 */
internal fun plainText(notes: String): String {
    val text = notes.replace("\r\n", "\n")
    val downloadBlock = Regex("(?m)^\\s*(Installing:|SHA-256:)").find(text)
    return text.substring(0, downloadBlock?.range?.first ?: text.length).trim()
}

/** Tolerates the "v." spelling one early tag used, and a capital V. */
private fun displayVersion(tag: String): String =
    tag.trim().removePrefix("v").removePrefix("V").removePrefix(".")

@Throws(IOException::class)
private fun fetch(url: String): String {
    val connection = URL(url).openConnection() as HttpURLConnection
    try {
        connection.connectTimeout = TIMEOUT_MILLIS
        connection.readTimeout = TIMEOUT_MILLIS
        connection.setRequestProperty("User-Agent", "Skate3Port-Android")
        connection.setRequestProperty("Accept", "application/vnd.github+json")
        if (connection.responseCode != HttpURLConnection.HTTP_OK) {
            throw IOException("GitHub returned " + connection.responseCode + ".")
        }
        val body = connection.inputStream.readNBytes(RESPONSE_LIMIT + 1)
        // Truncating instead would hand the parser a torn document.
        if (body.size > RESPONSE_LIMIT) throw IOException("GitHub sent more than expected.")
        return String(body, StandardCharsets.UTF_8)
    } finally {
        connection.disconnect()
    }
}

/**
 * Semantic-version precedence over the release tags. Release candidates share a
 * versionCode with the release they lead to, so comparing those would never
 * offer 1.3.3 to someone running 1.3.3-rc1; this does.
 */
internal fun compareVersions(left: String, right: String): Int {
    val (leftCore, leftPre) = splitVersion(left)
    val (rightCore, rightPre) = splitVersion(right)
    for (i in 0 until maxOf(leftCore.size, rightCore.size)) {
        val difference = leftCore.getOrElse(i) { 0 }.compareTo(rightCore.getOrElse(i) { 0 })
        if (difference != 0) return difference
    }
    if (leftPre.isEmpty() || rightPre.isEmpty()) {
        // A release outranks any of its pre-releases.
        return rightPre.size.compareTo(leftPre.size)
    }
    for (i in 0 until maxOf(leftPre.size, rightPre.size)) {
        val a = leftPre.getOrNull(i) ?: return -1
        val b = rightPre.getOrNull(i) ?: return 1
        val difference = compareIdentifiers(a, b)
        if (difference != 0) return difference
    }
    return 0
}

private fun compareIdentifiers(left: String, right: String): Int {
    val leftNumber = left.toIntOrNull()
    val rightNumber = right.toIntOrNull()
    if (leftNumber != null && rightNumber != null) return leftNumber.compareTo(rightNumber)
    // Our tags spell a candidate "rc1", so the trailing digits are a count rather
    // than text. Semver would compare them as text and sort rc10 before rc9.
    val leftWord = left.trimEnd { it.isDigit() }
    if (leftWord == right.trimEnd { it.isDigit() }) {
        val leftCount = left.drop(leftWord.length).toIntOrNull()
        val rightCount = right.drop(leftWord.length).toIntOrNull()
        if (leftCount != null && rightCount != null) return leftCount.compareTo(rightCount)
    }
    return left.compareTo(right)
}

private fun splitVersion(version: String): Pair<List<Int>, List<String>> {
    val trimmed = displayVersion(version.trim())
    val core = trimmed.substringBefore('-')
    val pre = trimmed.substringAfter('-', "")
    return Pair(
        core.split('.').map { it.toIntOrNull() ?: 0 },
        if (pre.isEmpty()) emptyList() else pre.split('.', '-'),
    )
}
