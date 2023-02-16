package ie.equalit.ouinet;

import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import android.support.annotation.NonNull;
import android.support.annotation.Nullable;
import android.support.annotation.VisibleForTesting;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * Contains the configuration settings for the Ouinet service. The config is immutable and can
 * be passed around as a Parcelable. The Config has a private constructor and should only be
 * created by the ConfigBuilder, as building the config generates certificates and copies files
 * from the APK onto the filesystem. This should only be done once in the life time of the
 * app to avoid race conditions.
 */
public class Config implements Parcelable {
    public enum LogLevel {
        SILLY,
        DEBUG,
        VERBOSE,
        INFO,
        WARN,
        ERROR,
        ABORT
    }

    private static final String TAG = "OuinetConfig";

    private static final String ASSET_PATH = "file:///android_asset/";
    @VisibleForTesting static final String OBFS4_PROXY = "obfs4proxy";
    private static final String OUINET_DIR = "/ouinet";

    public static class ConfigBuilder {
        private Context context;
        private Set<String> btBootstrapExtras = null;
        private String cacheHttpPubKey;
        private String injectorCredentials;
        private String injectorTlsCert;
        private String tlsCaCertStorePath;
        private String cacheType;
        private boolean cachePrivate = false;
        private String cacheStaticPath;
        private String cacheStaticContentPath;
        private String listenOnTcp;
        private String frontEndEp;
        private String maxCachedAge;
        private String localDomain;
        private String originDohBase;
        private boolean disableOriginAccess   = false;
        private boolean disableProxyAccess    = false;
        private boolean disableInjectorAccess = false;
        private LogLevel logLevel = null;
        private boolean enableLogFile = false;

        public ConfigBuilder(Context context) {
            Ouinet.maybeLoadLibraries(context);

            this.context = context;
        }

        public ConfigBuilder addBtBootstrapExtra(String btBootstrapExtra){
            if (btBootstrapExtra == null)
                return this;

            if (this.btBootstrapExtras == null)
                this.btBootstrapExtras = new HashSet<>();
            // Leave validation to the client.
            this.btBootstrapExtras.add(btBootstrapExtra);
            return this;
        }

        public ConfigBuilder setBtBootstrapExtras(Set<String> btBootstrapExtras){
            // Leave validation to the client.
            this.btBootstrapExtras = (btBootstrapExtras == null ? null : new HashSet<>(btBootstrapExtras));
            return this;
        }

        public ConfigBuilder setCacheHttpPubKey(String cacheHttpPubKey){
            this.cacheHttpPubKey = cacheHttpPubKey;
            return this;
        }
        public ConfigBuilder setInjectorCredentials(String injectorCredentials){
            this.injectorCredentials = injectorCredentials;
            return this;
        }
        public ConfigBuilder setInjectorTlsCert(String injectorTlsCert){
            this.injectorTlsCert = injectorTlsCert;
            return this;
        }
        /**
         * Path to a .pem file with CA certificates.
         * One can get it from e.g.: https://curl.haxx.se/docs/caextract.html
         */
        public ConfigBuilder setTlsCaCertStorePath(@Nullable String tlsCaCertStorePath){
            this.tlsCaCertStorePath = tlsCaCertStorePath;
            return this;
        }
        public ConfigBuilder setCacheType(String cacheType){
            this.cacheType = cacheType;
            return this;
        }
        public ConfigBuilder setCachePrivate(boolean cachePrivate){
            this.cachePrivate = cachePrivate;
            return this;
        }
        public ConfigBuilder setCacheStaticPath(String cacheStaticPath){
            this.cacheStaticPath = cacheStaticPath;
            return this;
        }
        public ConfigBuilder setCacheStaticContentPath(String cacheStaticContentPath){
            this.cacheStaticContentPath = cacheStaticContentPath;
            return this;
        }
        public ConfigBuilder setListenOnTcp(String listenOnTcp){
            this.listenOnTcp = listenOnTcp;
            return this;
        }
        public ConfigBuilder setFrontEndEp(String frontEndEp){
            this.frontEndEp = frontEndEp;
            return this;
        }
        public ConfigBuilder setMaxCachedAge(String maxCachedAge){
            this.maxCachedAge = maxCachedAge;
            return this;
        }
        public ConfigBuilder setLocalDomain(String localDomain){
            this.localDomain = localDomain;
            return this;
        }
        public ConfigBuilder setOriginDohBase(String originDohBase){
            this.originDohBase = originDohBase;
            return this;
        }
        public ConfigBuilder setDisableOriginAccess(boolean disableOriginAccess){
            this.disableOriginAccess = disableOriginAccess;
            return this;
        }
        public ConfigBuilder setDisableProxyAccess(boolean disableProxyAccess){
            this.disableProxyAccess = disableProxyAccess;
            return this;
        }
        public ConfigBuilder setDisableInjectorAccess(boolean disableInjectorAccess){
            this.disableInjectorAccess = disableInjectorAccess;
            return this;
        }
        public ConfigBuilder setLogLevel(LogLevel logLevel){
            this.logLevel = logLevel;
            return this;
        }
        public ConfigBuilder setEnableLogFile(boolean enableLogFile){
            this.enableLogFile = enableLogFile;
            return this;
        }

        /**
         * Writes the injector TLS Cert Store to the filesystem if necessary and returns the path to the
         * certificate.
         */
        private @Nullable String setupInjectorTlsCert(String ouinetDirectory) {
            if (injectorTlsCert == null) {
                return null;
            }
            try {
                String tlsCertPath = ouinetDirectory + "/injector-tls-cert.pem";
                writeToFile(tlsCertPath, injectorTlsCert.getBytes());
                return tlsCertPath;
            } catch (IOException e) {
                Log.d(TAG, "Exception thrown while creating injector's cert file: ", e);
            }
            return null;
        }

        /**
         * Copies the TLS CA Cert Store to the filesystem if necessary and returns the path to the
         * certificate.
         */
        private @Nullable String setupTlsCaCertStore(String ouinetDirectory) {
            if (tlsCaCertStorePath == null || !tlsCaCertStorePath.startsWith(ASSET_PATH)) {
                // Nothing to be done.
                return tlsCaCertStorePath;
            }

            // TODO: Ouinet's C++ code doesn't yet have a way to read asset
            // files from the apk. As a temporary workaround we copy the
            // asset file to a regular file and the pass path to that to
            // the C++ code.
            String filename = tlsCaCertStorePath.substring(ASSET_PATH.length());
            String dest = ouinetDirectory + "/assets/" + filename;
            if (copyAssetToFile(tlsCaCertStorePath, dest)) {
                return dest;
            }
            return null;
        }

        /**
         * Generates the CA root certificate if it does not exist.
         * @param ouinetDirectory
         * @return The path to the certificate.
         */
        private @NonNull String setupCaRootCert(String ouinetDirectory) {
            return Ouinet.getCARootCert(ouinetDirectory);
        }

        /**
         * Copies the objfs proxy executable from the APK assets onto the local filesystem and
         * makes it executable.
         * @param ouinetDirectory
         * @return Returns the directory where the executable was placed, or null if it could not
         * write to the file.
         */
        private @Nullable String setupObfs4ProxyExecutable(String ouinetDirectory) {
            String src = ASSET_PATH + OBFS4_PROXY;
            String dest = ouinetDirectory + "/" + OBFS4_PROXY;
            if (copyExecutableToFile(src, dest)) {
                Log.d(TAG, "obfs4proxy copied to " + dest);
                return ouinetDirectory;
            }
            Log.d(TAG, "obfs4proxy not copied");
            return null;
        }

        private boolean copyAssetToFile(String asset, String dest){
            if (!asset.startsWith(ASSET_PATH)) {
                throw new IllegalArgumentException("Invalid asset path: " + asset);
            }
            String assetPath = asset.substring(ASSET_PATH.length());
            try {
                java.io.InputStream stream = context.getAssets().open(assetPath);
                int size = stream.available();
                byte[] buffer = new byte[size];
                stream.read(buffer);
                stream.close();
                writeToFile(dest, buffer);
            } catch (IOException e) {
                Log.d(TAG, "Failed to write asset \"" + asset + "\" to file \"" + dest + "\"", e);
                return false;
            }

            return true;
        }

        private void writeToFile(String path, byte[] bytes) throws IOException {
            File file = new File(path);

            if (!file.exists()) {
                File dir = file.getParentFile();
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                file.createNewFile();
            }

            FileOutputStream stream = new FileOutputStream(file);
            stream.write(bytes);
            stream.close();
        }

        private boolean copyExecutableToFile(String asset, String path) {
            if (!copyAssetToFile(asset, path)) {
                return false;
            }
            File executable = new File(path);
            if (!executable.setExecutable(true)) {
                Log.d(TAG, "Failed to set executable for file: " + path);
                return false;
            }
            return true;
        }

        private String getOuinetDirectory() {
            return context.getFilesDir().getAbsolutePath() + OUINET_DIR;
        }

        public Config build() {
            String ouinetDirectory = getOuinetDirectory();
            new File(ouinetDirectory).mkdirs();

            return new Config(
                    ouinetDirectory,
                    btBootstrapExtras,
                    cacheHttpPubKey,
                    injectorCredentials,
                    setupInjectorTlsCert(ouinetDirectory),
                    setupTlsCaCertStore(ouinetDirectory),
                    setupCaRootCert(ouinetDirectory),
                    setupObfs4ProxyExecutable(ouinetDirectory),
                    cacheType,
                    cachePrivate,
                    cacheStaticPath,
                    cacheStaticContentPath,
                    listenOnTcp,
                    frontEndEp,
                    maxCachedAge,
                    localDomain,
                    originDohBase,
                    disableOriginAccess,
                    disableProxyAccess,
                    disableInjectorAccess,
                    logLevel,
                    enableLogFile);
        }
    }

    private String ouinetDirectory;
    private Set<String> btBootstrapExtras;
    private String cacheHttpPubKey;
    private String injectorCredentials;
    private String injectorTlsCertPath;
    private String tlsCaCertStorePath;
    private String caRootCertPath;
    private String obfs4ProxyPath;
    private String cacheType;
    private boolean cachePrivate;
    private String cacheStaticPath;
    private String cacheStaticContentPath;
    private String listenOnTcp;
    private String frontEndEp;
    private String maxCachedAge;
    private String localDomain;
    private String originDohBase;
    private boolean disableOriginAccess;
    private boolean disableProxyAccess;
    private boolean disableInjectorAccess;
    private LogLevel logLevel;
    private boolean enableLogFile;

    private Config(String ouinetDirectory,
                  Set<String> btBootstrapExtras,
                  String cacheHttpPubKey,
                  String injectorCredentials,
                  String injectorTlsCertPath,
                  String tlsCaCertStorePath,
                  String caRootCertPath,
                  String obfs4ProxyPath,
                  String cacheType,
                  boolean cachePrivate,
                  String cacheStaticPath,
                  String cacheStaticContentPath,
                  String listenOnTcp,
                  String frontEndEp,
                  String maxCachedAge,
                  String localDomain,
                  String originDohBase,
                  boolean disableOriginAccess,
                  boolean disableProxyAccess,
                  boolean disableInjectorAccess,
                  LogLevel logLevel,
                  boolean enableLogFile) {
        this.ouinetDirectory = ouinetDirectory;
        this.btBootstrapExtras = (btBootstrapExtras == null ? null : new HashSet<>(btBootstrapExtras));
        this.cacheHttpPubKey = cacheHttpPubKey;
        this.injectorCredentials = injectorCredentials;
        this.injectorTlsCertPath = injectorTlsCertPath;
        this.tlsCaCertStorePath = tlsCaCertStorePath;
        this.caRootCertPath = caRootCertPath;
        this.obfs4ProxyPath = obfs4ProxyPath;
        this.cacheType = cacheType;
        this.cachePrivate = cachePrivate;
        this.cacheStaticPath = cacheStaticPath;
        this.cacheStaticContentPath = cacheStaticContentPath;
        this.listenOnTcp = listenOnTcp;
        this.frontEndEp = frontEndEp;
        this.maxCachedAge = maxCachedAge;
        this.localDomain = localDomain;
        this.originDohBase = originDohBase;
        this.disableOriginAccess = disableOriginAccess;
        this.disableProxyAccess = disableProxyAccess;
        this.disableInjectorAccess = disableInjectorAccess;
        this.logLevel = logLevel;
        this.enableLogFile = enableLogFile;
    }
    public String getOuinetDirectory() {
        return ouinetDirectory;
    }
    public Set<String> getBtBootstrapExtras() {
        return (btBootstrapExtras == null ? null : new HashSet<>(btBootstrapExtras));
    }
    public String getCacheHttpPubKey() {
        return cacheHttpPubKey;
    }
    public String getInjectorCredentials() {
        return injectorCredentials;
    }
    public String getInjectorTlsCertPath() {
        return injectorTlsCertPath;
    }
    public String getTlsCaCertStorePath() {
        return tlsCaCertStorePath;
    }
    public String getCaRootCertPath() {
        return caRootCertPath;
    }
    public String getObfs4ProxyPath() {
        return obfs4ProxyPath;
    }
    public String getCacheType() {
        return cacheType;
    }
    public boolean getCachePrivate() {
        return cachePrivate;
    }
    public String getCacheStaticPath() {
        return cacheStaticPath;
    }
    public String getCacheStaticContentPath() {
        return cacheStaticContentPath;
    }
    public String getListenOnTcp() {
        return listenOnTcp;
    }
    public String getFrontEndEp() {
        return frontEndEp;
    }
    public String getMaxCachedAge() {
        return maxCachedAge;
    }
    public String getLocalDomain() {
        return localDomain;
    }
    public String getOriginDohBase() {
        return originDohBase;
    }
    public boolean getDisableOriginAccess() {
        return disableOriginAccess;
    }
    public boolean getDisableProxyAccess() {
        return disableProxyAccess;
    }
    public boolean getDisableInjectorAccess() {
        return disableInjectorAccess;
    }
    public LogLevel getLogLevel() {
        return logLevel;
    }
    public boolean getEnableLogFile() {
        return enableLogFile;
    }

    public static final Parcelable.Creator<Config> CREATOR
            = new Parcelable.Creator<Config>() {
        public Config createFromParcel(Parcel in) {
            return new Config(in);
        }

        public Config[] newArray(int size) {
            return new Config[size];
        }
    };

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel out, int flags) {
        out.writeString(ouinetDirectory);
        out.writeStringArray(btBootstrapExtras == null ? null : btBootstrapExtras.toArray(new String[0]));
        out.writeString(cacheHttpPubKey);
        out.writeString(injectorCredentials);
        out.writeString(injectorTlsCertPath);
        out.writeString(tlsCaCertStorePath);
        out.writeString(caRootCertPath);
        out.writeString(obfs4ProxyPath);
        out.writeString(cacheType);
        out.writeInt(cachePrivate ? 1 : 0);
        out.writeString(cacheStaticPath);
        out.writeString(cacheStaticContentPath);
        out.writeString(listenOnTcp);
        out.writeString(frontEndEp);
        out.writeString(maxCachedAge);
        out.writeString(localDomain);
        out.writeString(originDohBase);
        out.writeInt(disableOriginAccess ? 1 : 0);
        out.writeInt(disableProxyAccess ? 1 : 0);
        out.writeInt(disableInjectorAccess ? 1 : 0);
        // https://stackoverflow.com/a/48533385/273348
        out.writeInt(logLevel == null ? -1 : logLevel.ordinal());
        out.writeInt(enableLogFile ? 1 : 0);
    }
    private Config(Parcel in) {
        ouinetDirectory = in.readString();

        String[] btBootstrapExtrasArray = in.createStringArray();
        if (btBootstrapExtrasArray == null)
            btBootstrapExtras = null;
        else {
            btBootstrapExtras = new HashSet<>();
            Collections.addAll(btBootstrapExtras, btBootstrapExtrasArray);
        }

        cacheHttpPubKey = in.readString();
        injectorCredentials = in.readString();
        injectorTlsCertPath = in.readString();
        tlsCaCertStorePath = in.readString();
        caRootCertPath = in.readString();
        obfs4ProxyPath = in.readString();
        cacheType = in.readString();
        cachePrivate = in.readInt() != 0;
        cacheStaticPath = in.readString();
        cacheStaticContentPath = in.readString();
        listenOnTcp= in.readString();
        frontEndEp = in.readString();
        maxCachedAge = in.readString();
        localDomain = in.readString();
        originDohBase = in.readString();

        disableOriginAccess   = in.readInt() != 0;
        disableProxyAccess    = in.readInt() != 0;
        disableInjectorAccess = in.readInt() != 0;

        int logLevelInt = in.readInt();
        logLevel = (logLevelInt == -1 ? null : LogLevel.values()[logLevelInt]);

        enableLogFile = in.readInt() != 0;
    }

}
