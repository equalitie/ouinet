package ie.equalit.ouinet

import android.app.ActivityManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import ie.equalit.ouinet.Config
import ie.equalit.ouinet.Ouinet
import kotlin.system.exitProcess

class OuinetBackground() : NotificationListener {
    class Builder(context: Context) {
        private val context : Context
        private lateinit var ouinetConfig : Config
        private lateinit var notificationConfig: NotificationConfig
        private var onNotificationTapped : (() -> Unit)? = null
        private var onConfirmTapped : (() -> Unit)? = null
        private var connectivityReceiverEnabled : Boolean = true

        init {
            this.context = context
        }

        fun setOuinetConfig(
            ouinetConfig: Config
        ) : Builder {
            this.ouinetConfig = ouinetConfig
            return this
        }

        fun setNotificationConfig(
            notificationConfig: NotificationConfig
        ) : Builder {
            this.notificationConfig = notificationConfig
            return this
        }

        fun restartOnConnectivityChange(
            enabled : Boolean
        ) : Builder {
            this.connectivityReceiverEnabled = enabled
            return this
        }

        fun setOnNotifiactionTappedListener(
            onNotificationTapped : () -> Unit
        ) : Builder {
            this.onNotificationTapped = onNotificationTapped
            return this
        }

        fun setOnConfirmTappedListener(
            onConfirmTapped: () -> Unit
        ) : Builder {
            this.onConfirmTapped = onConfirmTapped
            return this
        }

        fun build() = OuinetBackground(
            context,
            ouinetConfig,
            connectivityReceiverEnabled,
            notificationConfig,
            onNotificationTapped,
            onConfirmTapped,
        )
    }

    lateinit var context: Context
        private set
    lateinit var activity: AppCompatActivity
        private set
    lateinit var ouinetConfig: Config
        private set
    lateinit var notificationConfig: NotificationConfig
        private set
    var connectivityReceiverEnabled: Boolean = true
        private set
    var onNotificationTapped: (() -> Unit)? = null
        private set
    var onConfirmTapped: (() -> Unit)? = null
        private set
    lateinit var connectivityReceiver: ConnectivityBroadcastReceiver
        private set
    lateinit var notificationReceiver: NotificationBroadcastReceiver
        private set

    private constructor(
        context: Context,
        ouinetConfig : Config,
        connectivityReceiverEnabled : Boolean,
        notificationConfig: NotificationConfig,
        onNotificationTapped: (() -> Unit)?,
        onConfirmTapped: (() -> Unit)?
    ) : this() {
        this.context = context
        this.ouinetConfig = ouinetConfig
        this.connectivityReceiverEnabled = connectivityReceiverEnabled
        this.notificationConfig = notificationConfig
        this.onNotificationTapped = onNotificationTapped
        this.onConfirmTapped = onConfirmTapped
        this.connectivityReceiver = ConnectivityBroadcastReceiver(this)
        this.notificationReceiver = NotificationBroadcastReceiver(this)
    }

    private var mOuinet: Ouinet? = null
    private val mHandler = Handler(Looper.myLooper()!!)
    private var mCurrentState : String = OuinetNotification.DEFAULT_STATE

    private fun startOuinet(
        callback : (() -> Unit )? = null
    ) : Thread {
        mOuinet = Ouinet(context, ouinetConfig)
        val thread = Thread(Runnable {
            if (mOuinet == null) return@Runnable
            mOuinet!!.start()
            callback?.invoke()
        })
        thread.start()
        return thread
    }

    private fun stopOuinet(
        callback : (() -> Unit )? = null
    ) : Thread {
        val thread = Thread(Runnable {
            if (mOuinet == null) return@Runnable
            val ouinet: Ouinet = mOuinet as Ouinet
            mOuinet = null
            ouinet.stop()
            callback?.invoke()
        })
        thread.start()
        return thread
    }

    private fun register() {
        if (connectivityReceiverEnabled) {
            val connectivityIntentFilter = IntentFilter()
            connectivityIntentFilter.addAction(ConnectivityManager.CONNECTIVITY_ACTION)
            context.registerReceiver(
                connectivityReceiver,
                connectivityIntentFilter
            )
        }
        val notificationIntentFilter = IntentFilter()
        notificationIntentFilter.addAction(NotificationBroadcastReceiver.NOTIFICATION_ACTION)
        context.registerReceiver(
            notificationReceiver,
            notificationIntentFilter)
    }

    private fun unregister() {
        if (connectivityReceiverEnabled) {
            context.unregisterReceiver(connectivityReceiver)
        }
        context.unregisterReceiver(notificationReceiver)
    }

    private fun sendOuinetStatePendingIntent() {
        OuinetNotification.getServicePendingIntent(
            context,
            OuinetNotification.UPDATE_CODE,
            notificationConfig,
            mCurrentState
        ).send()
    }

    private var updateOuinetState: Runnable = object : Runnable {
        override fun run() {
            try {
                val newState = getState()
                if (newState != mCurrentState) {
                    mCurrentState = newState
                    sendOuinetStatePendingIntent()
                }
            } finally {
                mHandler.postDelayed(
                    this,
                    notificationConfig.updateInterval.toLong())
            }
        }
    }

    private fun startUpdatingState() {
        updateOuinetState.run()
    }

    private fun stopUpdatingState() {
        mHandler.removeCallbacks(updateOuinetState)
    }

    @Synchronized
    fun start(
        callback : (() -> Unit)? = null
    ) : Thread {
        Intent(context, OuinetService::class.java).also {
            it.putExtra(OuinetNotification.CONFIG_EXTRA, notificationConfig)
            context.startService(it)
        }
        return startOuinet(callback)
    }

    @Synchronized
    fun stop(
        callback : (() -> Unit)? = null
    ) : Thread {
        Intent(context, OuinetService::class.java).also {
            context.stopService(it)
        }
        return stopOuinet(callback)
    }

    fun startup(
        callback : (() -> Unit)? = null
    ) : Thread {
        register()
        if (!notificationConfig.disableStatus)
            startUpdatingState()
        return start(callback)
    }

    fun getState() : String {
        return if (mOuinet != null)
            mOuinet!!.getState().toString()
        else
            OuinetNotification.DEFAULT_STATE
    }

    fun shutdown(
        doClear : Boolean,
        callback : (() -> Unit)? = null
    ) : Thread {
        if (!notificationConfig.disableStatus)
            stopUpdatingState()
        unregister()
        return stop {
            if (callback != null) {
                callback.invoke()
            }
            else {
                if (doClear) {
                    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
                    am.clearApplicationUserData()
                }
                exitProcess(0)
            }
        }
    }

    override fun onNotificationTapped() {
        if (onNotificationTapped == null) {
            Log.d("OUINET_BACKGROUND", "onNotificationTapped")
            shutdown(doClear = false)
        }
        else {
            onNotificationTapped?.invoke()
        }
    }

    override fun onConfirmTapped() {
        if (onConfirmTapped == null) {
            Log.d("OUINET_BACKGROUND", "onConfirmTapped")
            shutdown(doClear = true)
        }
        else {
            onConfirmTapped?.invoke()
        }
    }

    companion object {
        private const val TAG = "OuinetBackground"
    }
}
