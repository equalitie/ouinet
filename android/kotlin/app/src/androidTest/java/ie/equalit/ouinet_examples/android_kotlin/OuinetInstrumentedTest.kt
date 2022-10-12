package ie.equalit.ouinet_examples.android_kotlin

import ie.equalit.ouinet.Config
import ie.equalit.ouinet.Ouinet

import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4

import org.junit.Test
import org.junit.runner.RunWith

import org.junit.Assert.*

/**
 * Instrumented test, which will execute on an Android device.
 *
 * See [testing documentation](http://d.android.com/tools/testing).
 */
@RunWith(AndroidJUnit4::class)
class OuinetInstrumentedTest {
    @Test
    fun useAppContext() {
        // Context of the app under test.
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        assertEquals("ie.equalit.ouinet_examples.android_kotlin", appContext.packageName)
    }

    @Test
    fun testStartStop() {
        val appContext = InstrumentationRegistry.getInstrumentation().targetContext
        var config = Config.ConfigBuilder(appContext)
            .setCacheType("bep5-http")
            .setCacheHttpPubKey(BuildConfig.CACHE_PUB_KEY)
            .build()

        var ouinet = Ouinet(appContext, config)

        for (i in 1..5) {
            ouinet.start()
            Thread.sleep(1000);
            assertTrue(ouinet.state.toString().startsWith("Start"))

            ouinet.stop()
            assertEquals("Stopped", ouinet.state.toString());
        }

        for (i in 1..3) {
            Thread.sleep(100);
            ouinet.stop()
            assertEquals("Stopped", ouinet.state.toString());
        }

        ouinet.start()
        Thread.sleep(1000);
        assertTrue(ouinet.state.toString().startsWith("Start"))
        ouinet.stop()
        assertEquals("Stopped", ouinet.state.toString());
    }
}
