package ie.equalit.ouinet

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

class NotificationBroadcastReceiver(listener: NotificationListener) : BroadcastReceiver() {

    private var listener : NotificationListener? = null

    init {
        this.listener = listener
    }

    override fun onReceive(context: Context, intent: Intent) {
        require(intent.hasExtra(CODE_EXTRA))
        val code = intent.getIntExtra(CODE_EXTRA, OuinetNotification.STOP_CODE)
        Log.i(TAG, "Receive intent with code $code")
        if (code == OuinetNotification.STOP_CODE) {
            listener?.onNotificationTapped()
        }
        else if (code == OuinetNotification.CONFIRM_CODE) {
            listener?.onConfirmTapped()
        }
    }

    companion object {
        private const val TAG = "NotificationReceiver"
        const val NOTIFICATION_ACTION = "ie.equalit.ouinet_components.NotificationBroadcastReceiver.NOTIFICATION"
        const val CODE_EXTRA = "code-extra"
    }
}