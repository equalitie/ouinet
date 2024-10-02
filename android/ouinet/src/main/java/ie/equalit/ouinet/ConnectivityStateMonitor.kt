package ie.equalit.ouinet

import android.content.Context
import android.net.ConnectivityManager
import android.net.ConnectivityManager.NetworkCallback
import android.net.Network
import android.net.NetworkRequest
import android.net.NetworkRequest.Builder
import android.net.NetworkCapabilities
import android.os.Handler
import android.os.Looper
import android.util.Log
import ie.equalit.ouinet.OuinetNotification.Companion.MILLISECOND

class ConnectivityStateMonitor (
    context : Context,
    background : OuinetBackground
) : NetworkCallback() {

    private val context : Context
    private val background : OuinetBackground
    private val mHandler = Handler(Looper.myLooper()!!)
    private var isStarted : Boolean = false
    private var startupTimeout : Int = 0
    private var availableNetworks : MutableList<Network> = mutableListOf()
    val networkRequest: NetworkRequest

    init {
        this.context = context
        this.background = background
        networkRequest = Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()
    }

    private var checkForOuinetStarted: Runnable = object : Runnable {
        override fun run() {
            try {
                if (background.getState() == Ouinet.RunningState.Started.toString() ||
                    background.getState() == Ouinet.RunningState.Degraded.toString() ||
                    startupTimeout >= INIT_TIMEOUT
                ) {
                    isStarted = true
                    startupTimeout = INIT_TIMEOUT
                }
                else {
                    startupTimeout++
                }
            } finally {
                if (!isStarted){
                    mHandler.postDelayed(
                        this,
                        1 * MILLISECOND)
                }
            }
        }
    }

    fun enable() {
        isStarted = false
        val connectivityManager = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivityManager.registerNetworkCallback(networkRequest, this)
        checkForOuinetStarted.run()
    }

    fun disable() {
        val connectivityManager = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        connectivityManager.unregisterNetworkCallback(this)
        mHandler.removeCallbacks(checkForOuinetStarted)
    }

    override fun onAvailable(network: Network) {
        availableNetworks.add(network)
        Log.d(TAG,"Network available: $network, currently available networks: $availableNetworks");
        if (isStarted) {
            Log.d(TAG, "Network state changed, restart ouinet")
            background.restartOuinet()
        }
        else {
            Log.d(TAG, "Network available for first time, setting isStarted flag")
            isStarted = true
        }
    }

    override fun onLost(network: Network) {
        availableNetworks.remove(network)
        Log.d(TAG,"Network lost: $network, remaining available networks: $availableNetworks");
        if (availableNetworks.isEmpty()) {
            Log.d(TAG,"No networks available, stop ouinet")
            background.stopOuinet()
        }
    }

    companion object {
        const val TAG = "ConnectivityMonitor"
        const val INIT_TIMEOUT = 10
    }
}