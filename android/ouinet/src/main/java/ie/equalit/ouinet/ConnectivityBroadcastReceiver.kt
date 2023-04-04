package ie.equalit.ouinet

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.NetworkInfo
import android.util.Log
import ie.equalit.ouinet.OuinetNotification.Companion.MILLISECOND

class ConnectivityBroadcastReceiver (
    background: OuinetBackground
    ) : BroadcastReceiver() {

    private var background : OuinetBackground? = null

    init {
        this.background = background
    }

    override fun onReceive (context: Context?, intent: Intent) {
        val info = intent.getParcelableExtra<NetworkInfo>(ConnectivityManager.EXTRA_NETWORK_INFO)
            ?: return
        restartOnConnectivityChange(info.state)
    }

    private fun restartOnConnectivityChange (state : NetworkInfo.State) {
        if (state == NetworkInfo.State.CONNECTED || state == NetworkInfo.State.DISCONNECTED) {
            try {
                Log.d(TAG, "Stopping OuinetService on connectivity change")
                background?.stop()!!.join(10 * MILLISECOND)
            } catch (ex: Exception) {
                Log.w(TAG, "stopOuinetService failed with exception: $ex")
            }
            // TODO: Insert a pause / check client state.
            try {
                Log.d(TAG, "Starting OuinetService on connectivity change")
                background?.start()
            } catch (ex: Exception) {
                // TODO: if the start fails, we should try restarting the service later
                Log.w(TAG, "startOuinetService failed with exception: $ex")
            }
        }
    }

    companion object {
        const val TAG = "ConnectivityReceiver"
    }
}