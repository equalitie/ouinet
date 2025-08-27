package ie.equalit.ouinet

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
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

    fun create(): Notification {
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
        val notificationBuilder = NotificationCompat.Builder(context, channelId)
            .setSmallIcon(config.statusIcon)
            .setContentTitle(contentTitle)
            .setContentText(config.description)
            .setPriority(NotificationCompat.PRIORITY_DEFAULT)
            .setAutoCancel(true) // Close on tap.
        return notificationBuilder.build()
    }

    companion object {
        private const val TAG = "OuinetNotification"
        private const val CHANNEL_ID = "ouinet-notification-channel"
    }
}