package ie.equalit.ouinet.browser;

import android.content.Context;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.File;
import android.util.Log;

class Util {
    public static void saveToFile(Context ctx, String filename, String value) {
        FileOutputStream outputStream;
        try {
            outputStream = new FileOutputStream(new File(filename), false);
            outputStream.write(value.getBytes());
            outputStream.close();
        } catch (Exception e) {
            log("3");
            e.printStackTrace();
        }
    }

    public static String readFromFile(Context ctx,
                                      String filename,
                                      String default_) {
        FileInputStream inputStream;
        try {
            inputStream = new FileInputStream(new File(filename));
            StringBuffer content = new StringBuffer("");

            byte[] buffer = new byte[1024];

            int n;
            while ((n = inputStream.read(buffer)) != -1) {
                content.append(new String(buffer, 0, n));
            }

            return content.toString();
        } catch (Exception e) {
            return default_;
        }
    }

    public static void log(String s) {
        Log.d("Ouinet", s);
    }
}
