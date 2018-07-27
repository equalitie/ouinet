package ie.equalit.ouinet.service;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;

import android.content.Intent;
import android.widget.Toast;
import android.widget.Button;
import android.widget.TextView;
import android.view.ViewGroup;
import android.view.View;

public class Control extends AppCompatActivity {
    Button _button;
    boolean _running;

    protected void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        _button = new Button(this);

        _running = false;
        _button.setText(R.string.start_ouinet);

        _button.setLayoutParams(new ViewGroup.LayoutParams
                ( ViewGroup.LayoutParams.WRAP_CONTENT
                , ViewGroup.LayoutParams.WRAP_CONTENT));

        _button.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (_running) {
                    _running = false;
                    _button.setText(R.string.stop_ouinet);
                }
                else {
                    _running = true;
                    _button.setText(R.string.start_ouinet);
                }
            }
        });

        setContentView(_button);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        super.onBackPressed();
    }
}
