package ie.equalit.ouinet

import android.app.*
import android.content.Intent
import android.os.*
import android.util.Log

open class OuinetService : Service(){
    // To see whether this service is running, you may try this command:
    // adb -s $mi shell dumpsys activity services OuinetService

    private lateinit var ouinetNotification : OuinetNotification
    private var ouinetState = Constants.DEFAULT_STATE

    private fun getConfigExtra(intent: Intent) : NotificationConfig {
        require(intent.hasExtra(Constants.CONFIG_EXTRA)) {
            "Service intent missing config extra"
        }
        return intent.getParcelableExtra<NotificationConfig>(Constants.CONFIG_EXTRA)!!
    }

    private fun getShowConfirmExtra(intent: Intent) : Boolean {
        val extra = intent.getIntExtra(Constants.CODE_EXTRA, Constants.HIDE_CODE)
        return (extra == Constants.SHOW_CODE)
    }

    private fun getStateExtra(intent: Intent) : String? {
        val extra = intent.getIntExtra(Constants.CODE_EXTRA, Constants.HIDE_CODE)
        var state : String? = null
        if (extra == Constants.UPDATE_CODE) {
            state = intent.getStringExtra(Constants.STATE_EXTRA)
        }
        return state
    }

    override fun onCreate() {
        super.onCreate()
        Log.d(TAG, "Service created")
    }

    override fun onStartCommand(intent: Intent, flags: Int, startId: Int): Int {
        val config = getConfigExtra(intent)
        val showConfirm = getShowConfirmExtra(intent)
        ouinetState = getStateExtra(intent)?:ouinetState
        ouinetNotification = OuinetNotification(this, config)
        try {
            startForeground(
                NOTIFICATION_ID,
                ouinetNotification.create(showConfirm, ouinetState)
            )
        } catch (_: Exception) {
            stopSelf()
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "Destroying service")
        ouinetNotification.removeCallbacks()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        }
        else {
            stopForeground(true)
        }
        Log.d(TAG, "Service destroyed")
    }

    override fun onBind(p0: Intent?): IBinder? {
        //TODO("Not yet implemented")
        return null
    }

    companion object {
        private const val TAG = "OuinetService"
        private const val NOTIFICATION_ID = 1
    }
}