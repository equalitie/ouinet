package ie.equalit.ouinet;

import android.content.Context;

import android.webkit.WebViewClient;
import android.webkit.WebView;
import android.webkit.HttpAuthHandler;
import android.webkit.WebViewDatabase;

import android.widget.EditText;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.view.View.OnClickListener;
import android.text.InputType;

class OuiWebViewClient extends WebViewClient {
    class Credentials {
        String username;
        String password;
    }

    private Context _context;
    private Credentials _saved_credentials;
    private String _cred_file = "injector.credentials.txt";

    public OuiWebViewClient(Context context) {
        super();

        _context = context;

        _saved_credentials
            = parse(Util.readFromFile(_context, _cred_file, null));
    }

    public void forgetCredentials() {
        _saved_credentials = null;
        Util.saveToFile(_context, _cred_file, "");
        WebViewDatabase.getInstance(_context).clearHttpAuthUsernamePassword();
    }

    @Override
    public void onReceivedHttpAuthRequest(WebView view,
                                          final HttpAuthHandler handler,
                                          String host,
                                          String realm)
    {
        // TODO: Android system sees our client as being the main proxy for
        // which credentials are being asked (not the injector!). This will
        // become dangerous once we enable injector switching and  when we
        // allow more than one injector.
        //
        // I think the proper solution for this is to remove this
        // onReceivedHttpAuthRequest and let the ouinet/client make a request
        // to java to create this credential prompt. That way the client will
        // know for which injector the credentials are being asked for.
        if (_saved_credentials != null) {
            Credentials c = _saved_credentials;
            _saved_credentials = null;
            handler.proceed(c.username, c.password);
            return;
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(_context);

        builder.setTitle("Credentials required for Ouinet injector");
        builder.setMessage("Insert credentials in the <USENAME>:<PASSWORD> format");

        final EditText credentials = new EditText(_context);

        credentials.setInputType(InputType.TYPE_CLASS_TEXT);

        builder.setView(credentials);

        builder.setPositiveButton("OK", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                String s = credentials.getText().toString();
                Credentials c = parse(s);

                if (c == null) { return; }

                Util.saveToFile(_context, _cred_file, s);
                handler.proceed(c.username, c.password);
            }
        });

        builder.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
            @Override
            public void onClick(DialogInterface dialog, int which) {
                dialog.cancel();
            }
        });

        builder.show();
    }

    private Credentials parse(String str) {
        if (str == null) return null;

        String[] keyval = str.split(":", 2);

        if (keyval.length != 2) { return null; }

        Credentials c = new Credentials();

        c.username = keyval[0];
        c.password = keyval[1];

        return c;
    }
}

