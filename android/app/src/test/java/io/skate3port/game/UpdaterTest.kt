package io.skate3port.game

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Offering a downgrade sends a user to an install Android refuses, so the ordering matters. */
class UpdaterTest {
    private fun newer(left: String, right: String) =
        assertTrue("$left should outrank $right", compareVersions(left, right) > 0)

    private fun same(left: String, right: String) =
        assertEquals("$left should equal $right", 0, compareVersions(left, right))

    private fun release(tag: String, body: Any = "notes") = JSONObject()
        .put("tag_name", tag)
        .put("html_url", "https://example.invalid/$tag")
        .put("body", body)

    @Test
    fun ordersReleases() {
        newer("1.3.3", "1.3.2")
        newer("1.3.2", "1.2.9")
        newer("2.0.0", "1.9.9")
        newer("1.3.2.1", "1.3.2")
        newer("1.4", "1.3.3")
        same("1.3.2", "1.3.2")
        same("v1.3.2", "1.3.2")
    }

    @Test
    fun ordersPreReleases() {
        newer("1.3.3-rc1", "1.3.2")
        newer("1.3.3", "1.3.3-rc1")
        newer("1.3.3-rc2", "1.3.3-rc1")
        newer("1.3.3-rc10", "1.3.3-rc9")
        same("1.3.3-rc1", "1.3.3-rc1")
    }

    @Test
    fun toleratesTheTagsWeHaveShipped() {
        // v.1.3.1 is spelled with a stray dot on GitHub.
        same("v.1.3.1", "1.3.1")
        newer("1.3.2", "v.1.3.1")
        same("V1.3.2", "1.3.2")
        // A debug or qa build carries a suffix; it must neither miss nor invent an update.
        newer("1.3.3-rc1-debug", "1.3.3-rc1")
        newer("1.3.3", "1.3.3-rc1-debug")
    }

    @Test
    fun betaTakesTheNewestOfEitherKind() {
        // GitHub returns newest-first by date, which is not newest by version.
        val releases = JSONArray()
            .put(release("v1.3.2"))
            .put(release("v1.3.3-rc1"))
        assertEquals("1.3.3-rc1", newestRelease(releases)?.version)
    }

    @Test
    fun skipsEntriesThatCannotBeOffered() {
        val unusable = JSONArray()
            .put(JSONObject().put("tag_name", "").put("html_url", "https://example.invalid/x"))
            .put(JSONObject().put("tag_name", "v9.9.9"))
        assertNull(newestRelease(unusable))
    }

    @Test
    fun emptyNotesAreEmpty() {
        // GitHub sends a JSON null for a release published without notes.
        val blank = JSONArray().put(release("v1.3.4", JSONObject.NULL))
        assertEquals("", newestRelease(blank)?.notes)
    }

    @Test
    fun showsTheChangesWithoutTheDownloadBlock() {
        // Notes as TASKS.md says to write them, with GitHub's line endings.
        val notes = "One thing changed.\r\n\r\nSo did another.\r\n\r\n" +
            "Installing: install over your current version.\r\n\r\nSHA-256: 4008a60c\r\n"
        assertEquals("One thing changed.\n\nSo did another.", plainText(notes))
    }

    @Test
    fun keepsNotesThatHaveNoDownloadBlock() {
        assertEquals("Just the one fix.", plainText("Just the one fix.\r\n"))
    }
}
