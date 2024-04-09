package ie.equalit.ouinet

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.app.NotificationCompat

class OuinetNotification (context: Context, config: NotificationConfig) {

    private val context: Context
    private val config: NotificationConfig
    private val handler : Handler

    init {
        this.context = context
        this.config = config
        this.handler = Handler(Looper.myLooper()!!)
    }

    private fun getBroadcastPendingIntent(
        requestCode: Int
    ) : PendingIntent {
        Intent().also { intent ->
            intent.action = NotificationBroadcastReceiver.NOTIFICATION_ACTION
            intent.putExtra(NotificationBroadcastReceiver.CODE_EXTRA, requestCode)
            intent.setPackage(context.packageName)
            return PendingIntent.getBroadcast(
                context,
                requestCode,
                intent,
                getFlags()
            )
        }
    }

    private fun getActivityPendingIntent(
        activityName: String
    ): PendingIntent {
        Intent(context, Class.forName(activityName)).also { intent ->
            intent.action = Intent.ACTION_MAIN
            intent.addCategory(Intent.CATEGORY_LAUNCHER)
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            intent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
            intent.putExtra(FROM_NOTIFICATION_EXTRA, 1)
            return PendingIntent.getActivity(
                context,
                HOME_CODE,
                intent,
                getFlags()
            )
        }
    }

    private val showConfirmCallback = Runnable {
        try {
            getServicePendingIntent(context, HIDE_CODE, config).send()
        } catch (_: PendingIntent.CanceledException) {
        }
    }

    fun create(showConfirmAction: Boolean, state: String): Notification {
        var channelId = CHANNEL_ID
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            // Create a notification channel for Ouinet notifications. Recreating a notification
            // that already exists has no effect.
            val channel = NotificationChannel(
                CHANNEL_ID,
                config.channelName,
                NotificationManager.IMPORTANCE_LOW)
            channelId = channel.id
            val notificationManager = context.getSystemService(NotificationManager::class.java)
            notificationManager.createNotificationChannel(channel)
        }
        var contentTitle = "${config.title}"
        if (!config.disableStatus) {
            contentTitle += ": $state"
        }
        val stopPIntent = getBroadcastPendingIntent(STOP_CODE)
        val homePIntent = getActivityPendingIntent(config.activityName!!)
        val showConfirmPIntent = getServicePendingIntent(context, SHOW_CODE, config)
        val confirmPIntent = getBroadcastPendingIntent(CONFIRM_CODE)
        val notificationBuilder = NotificationCompat.Builder(context, channelId)
            .setSmallIcon(config.statusIcon)
            .setContentTitle(contentTitle)
            .setContentText(config.description)
            .setPriority(NotificationCompat.PRIORITY_DEFAULT)
            .setContentIntent(stopPIntent)
            .setAutoCancel(true) // Close on tap.
            .addAction(config.homeIcon, config.homeText, homePIntent)
            .addAction(config.clearIcon, config.clearText, showConfirmPIntent)
        if (showConfirmAction) {
            notificationBuilder
                .addAction(config.clearIcon, config.confirmText, confirmPIntent)
            handler.postDelayed(
                showConfirmCallback,
                3 * MILLISECOND
            )
        }
        return notificationBuilder.build()
    }

    fun removeCallbacks() {
        handler.removeCallbacks(showConfirmCallback)
    }

    companion object {
        private const val TAG = "OuinetNotification"
        const val MILLISECOND : Long = 1000
        const val CONFIG_EXTRA = "notification-config"
        const val STATE_EXTRA = "state-extra"
        const val CODE_EXTRA = "code-extra"
        const val FROM_NOTIFICATION_EXTRA = "from-notification"
        private const val CHANNEL_ID = "ouinet-notification-channel"
        const val DEFAULT_STATE = "Stopped"
        const val DEFAULT_INTERVAL = 5 * MILLISECOND
        const val STOP_CODE = 0
        const val CONFIRM_CODE = 1
        private const val HOME_CODE = 2
        const val SHOW_CODE = 3
        const val HIDE_CODE = 4
        const val UPDATE_CODE = 5

        private fun getFlags() : Int {
            var flags = PendingIntent.FLAG_CANCEL_CURRENT
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                flags = flags or PendingIntent.FLAG_IMMUTABLE
            }
            return flags
        }

        fun getServicePendingIntent(
            context: Context,
            requestCode: Int,
            config: NotificationConfig,
            state: String? = null
        ) : PendingIntent {
            val intent = Intent(context, OuinetService::class.java)
            intent.putExtra(CONFIG_EXTRA, config)
            intent.putExtra(CODE_EXTRA, requestCode)
            if (requestCode == UPDATE_CODE) {
                intent.putExtra(STATE_EXTRA, state)
            }
            return PendingIntent.getService(
                context,
                requestCode,
                intent,
                getFlags()
            )
        }
    }
}