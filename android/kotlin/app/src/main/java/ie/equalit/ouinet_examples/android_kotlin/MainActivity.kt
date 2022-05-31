package ie.equalit.ouinet_examples.android_kotlin

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import ie.equalit.ouinet.Config

class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        var config = Config.ConfigBuilder(this)
            .setCacheType("bep5-http")
            .setTlsCaCertStorePath("file:///android_asset/cacert.pem")
            .build()
    }
}