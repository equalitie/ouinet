package ie.equalit.ouinet

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import ie.equalit.ouinet.Config
import ie.equalit.ouinet.Ouinet
import ie.equalit.ouinet.OuinetEndpoint
import ie.equalit.ouinet.Constants.MILLISECOND
import kotlin.system.exitProcess

class OuinetBackground() {
    class Builder(context: Context) {
        private val context : Context
        private lateinit var ouinetConfig : Config
        private var notificationConfig: NotificationConfig? = null
        private var connectivityMonitorEnabled : Boolean = true

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
            this.connectivityMonitorEnabled = enabled
            return this
        }

        fun build() = OuinetBackground(
            context,
            ouinetConfig,
            connectivityMonitorEnabled,
            notificationConfig,
        )
    }

    lateinit var context: Context
        private set
    lateinit var ouinetConfig: Config
        private set
    var notificationConfig: NotificationConfig? = null
        private set
    var connectivityMonitorEnabled: Boolean = true
        private set
    lateinit var connectivityMonitor: ConnectivityStateMonitor
        private set

    private constructor(
        context: Context,
        ouinetConfig : Config,
        connectivityMonitorEnabled : Boolean,
        notificationConfig: NotificationConfig?,
    ) : this() {
        this.context = context
        this.ouinetConfig = ouinetConfig
        this.connectivityMonitorEnabled = connectivityMonitorEnabled
        this.notificationConfig = notificationConfig
        this.connectivityMonitor = ConnectivityStateMonitor(context, this)
    }

    private var mOuinet: Ouinet? = null
    private val mHandler = Handler(Looper.myLooper()!!)
    private var mCurrentState : String = Constants.DEFAULT_STATE
    private var stopThread : Thread? = null
    var isStopped : Boolean = false

    @Synchronized
    fun startOuinet(
        callback : (() -> Unit )? = null
    ) : Thread {
        mOuinet = Ouinet(context, ouinetConfig)
        val thread = Thread(Runnable {
            if (mOuinet == null) return@Runnable
            mOuinet!!.start()
            isStopped = false
            stopThread = null
            callback?.invoke()
        })
        thread.start()
        return thread
    }

    @Synchronized
    fun stopOuinet(
        callback : (() -> Unit )? = null
    ) : Thread {
        var thread = Thread()
        if (stopThread == null) {
            thread = Thread(Runnable {
                if (mOuinet == null) return@Runnable
                val ouinet: Ouinet = mOuinet as Ouinet
                mOuinet = null
                ouinet.stop()
                isStopped = true
                callback?.invoke()
            })
            stopThread = thread
            thread.start()
        }
        return thread
    }

    fun restartOuinet() : Thread {
        val thread = Thread( Runnable {
            Log.d(TAG, "Stopping Ouinet for restart")
            if (stopThread != null) {
                if (!isStopped) {
                    Log.d(TAG,"Ouinet stop already called, join thread until finishes or times out")
                    stopThread!!.join(10 * MILLISECOND)
                }
            }
            else {
                Log.d(TAG,"Call Ouinet stop and then restart")
                stopOuinet().join(10 * MILLISECOND)
            }
            Log.d(TAG, "Starting Ouinet for restart")
            startOuinet()
        })
        thread.start()
        return thread
    }

    private fun register() {
        if (connectivityMonitorEnabled)
            connectivityMonitor.enable()
    }

    private fun unregister() {
        if (connectivityMonitorEnabled)
            connectivityMonitor.disable()
    }

    private var updateOuinetState: Runnable = object : Runnable {
        override fun run() {
            try {
                val newState = getState()
                if (newState != mCurrentState) {
                    mCurrentState = newState
                }
            } finally {
                mHandler.postDelayed(
                    this,
                    notificationConfig!!.updateInterval.toLong())
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
        if (notificationConfig != null) {
            Intent(context, OuinetService::class.java).also {
                it.putExtra(Constants.CONFIG_EXTRA, notificationConfig)
                context.startService(it)
            }
        }
        return startOuinet(callback)
    }

    @Synchronized
    fun stop(
        callback : (() -> Unit)? = null
    ) : Thread {
        if (notificationConfig != null) {
            Intent(context, OuinetService::class.java).also {
                context.stopService(it)
            }
        }
        return stopOuinet(callback)
    }

    fun startup(
        callback : (() -> Unit)? = null
    ) : Thread {
        val thread = start(callback)
        register()
        if (notificationConfig != null) {
            if (notificationConfig!!.disableStatus == false)
                startUpdatingState()
        }
        return thread
    }

    fun getState() : String {
        return if (mOuinet != null)
            mOuinet!!.getState().toString()
        else
            Constants.DEFAULT_STATE
    }

    fun getProxyEndpoint() : OuinetEndpoint? {
        if (mOuinet != null) {
            val endpointStr = mOuinet!!.getProxyEndpoint()
            if (endpointStr != "")
                return OuinetEndpoint(endpointStr)
            return null
        } else {
            return null
        }
    }

    fun getFrontendEndpoint() : OuinetEndpoint? {
        if (mOuinet != null) {
            val endpointStr = mOuinet!!.getFrontendEndpoint()
            if (endpointStr != "")
                return OuinetEndpoint(endpointStr)
            return null
        } else {
            return null
        }
    }

    fun shutdown(
        doClear : Boolean,
        callback : (() -> Unit)? = null
    ) : Thread {
        if (notificationConfig != null) {
            if (notificationConfig!!.disableStatus == false)
                stopUpdatingState()
        }
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

    companion object {
        private const val TAG = "OuinetBackground"
    }
}
