package ie.equalit.ouinet.service;

import android.content.Intent;
import android.widget.Toast;
import android.app.IntentService;

import ie.equalit.ouinet.Ouinet;

public class OuinetService extends IntentService {
    private Ouinet ouinet;

    public OuinetService() {
        super("OuinetService");
    }

    /**
     * The IntentService calls this method from the default worker thread with
     * the intent that started the service. When this method returns, IntentService
     * stops the service, as appropriate.
     */
    @Override
    protected void onHandleIntent(Intent intent) {
        //// Normally we would do some work here, like download a file.
        //// For our sample, we just sleep for 5 seconds.
        //try {
        //    Thread.sleep(5000);
        //} catch (InterruptedException e) {
        //    // Restore interrupt status.
        //    Thread.currentThread().interrupt();
        //}
    }
}
