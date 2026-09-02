package ie.equalit.ouinet;

import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
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

    private static final String FILE_PROTO = "file://";
    private static final String ASSET_PATH = FILE_PROTO + "/android_asset/";
    @VisibleForTesting static final String OBFS4_PROXY = "obfs4proxy";
    private static final String OUINET_DIR = "/ouinet";
    private static final String ERROR_PAGE_FILE = "error-page.html";

    public static class ConfigBuilder {
        private Context context;
        private Set<String> btBootstrapExtras = null;
        private String cacheHttpPubKey;
        private String injectorCredentials;
        private String injectorTlsCert;
        private String tlsCaCertStoreDir;
        private String errorPagePath;
        private String cacheType;
        private boolean cachePrivate = false;
        private String cacheStaticPath;
        private String cacheStaticContentPath;
        private String i2pBep3Tracker;
        private String enableI2pServiceExe;
        private String enableI2pServiceExt;
        private boolean enableI2pServiceLib = false;
        private String listenOnTcp;
        private String frontEndEp;
        private String frontEndAccessToken;
        private String proxyAccessToken;
        private String clientCredentials;
        private boolean debugFrontEndAccessToken;
        private String udpMuxPort;
        private String udpMuxRxLimit;
        private boolean disableBridgeAnnouncement = false;
        private String requestBodyLimit;
        private String maxCachedAge;
        private String localDomain;
        private Set<String> dnsProtocols = null;
        private boolean disableOriginAccess   = false;
        private boolean disableProxyAccess    = false;
        private boolean disableInjectorAccess = false;
        private LogLevel logLevel = null;
        private boolean enableLogFile = false;

        private boolean metricsEnableOnStart = false;
        // Parallel lists: one entry per metrics server, in the same order.
        // Use "." for token/caCert to mean "no token"/"default cert" for that
        // server, and a "@/path/to/file" caCert value to read the certificate
        // from a file instead of passing it inline.
        private List<String> metricsServerUrls = null;
        private List<String> metricsServerTokens = null;
        private List<String> metricsServerCaCerts = null;
        private List<Integer> metricsServerPriorities = null;
        private String metricsEncryptionKey;
        private String metricsDeleteAfter;

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
        public ConfigBuilder setTlsCaCertStoreDir(@Nullable String tlsCaCertStoreDir){
            this.tlsCaCertStoreDir = tlsCaCertStoreDir;
            return this;
        }
        /**
         * Path to a html + css webpage to be displayed on 500 error.
         */
        public ConfigBuilder setErrorPagePath(@Nullable String errorPagePath){
            this.errorPagePath = errorPagePath;
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
        public ConfigBuilder setI2pBep3Tracker(String i2pBep3Tracker){
            this.i2pBep3Tracker = i2pBep3Tracker;
            return this;
        }
        public ConfigBuilder setEnableI2pServiceExe(String enableI2pServiceExe){
            this.enableI2pServiceExe = enableI2pServiceExe;
            return this;
        }
        public ConfigBuilder setEnableI2pServiceExt(String enableI2pServiceExt){
            this.enableI2pServiceExt = enableI2pServiceExt;
            return this;
        }
        public ConfigBuilder setEnableI2pServiceLib(boolean enableI2pServiceLib){
            this.enableI2pServiceLib = enableI2pServiceLib;
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
        public ConfigBuilder setFrontEndAccessToken(String token){
            this.frontEndAccessToken = token;
            return this;
        }
        public ConfigBuilder setProxyAccessToken(String token){
            this.proxyAccessToken = token;
            return this;
        }
        public ConfigBuilder setClientCredentials(String clientCredentials){
            this.clientCredentials = clientCredentials;
            return this;
        }
        public ConfigBuilder setDebugFrontEndAccessToken(boolean enable){
            this.debugFrontEndAccessToken = enable;
            return this;
        }
        public ConfigBuilder setUdpMuxPort(String udpMuxPort){
            this.udpMuxPort = udpMuxPort;
            return this;
        }
        public ConfigBuilder setUdpMuxRxLimit(String udpMuxRxLimit){
            this.udpMuxRxLimit = udpMuxRxLimit;
            return this;
        }
        public ConfigBuilder setDisableBridgeAnnouncement(boolean disableBridgeAnnouncement){
            this.disableBridgeAnnouncement = disableBridgeAnnouncement;
            return this;
        }
        public ConfigBuilder setRequestBodyLimit(String requestBodyLimit){
            this.requestBodyLimit = requestBodyLimit;
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

        public ConfigBuilder addDnsProtocol(String dnsProtocol){
            if (dnsProtocol == null)
                return this;

            if (this.dnsProtocols == null)
                this.dnsProtocols = new HashSet<>();
            // Leave validation to the client.
            this.dnsProtocols.add(dnsProtocol);
            return this;
        }
        public ConfigBuilder setDnsProtocols(Set<String> dnsProtocols){
            // Leave validation to the client.
            this.dnsProtocols = (dnsProtocols == null ? null : new HashSet<>(dnsProtocols));
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
        public ConfigBuilder setMetricsEnableOnStart(boolean enable) {
            this.metricsEnableOnStart = enable;
            return this;
        }
        /**
         * Adds one metrics server to send statistics/metrics records to.
         * Can be called multiple times to send records to several servers.
         *
         * @param url URL of the metrics server.
         * @param token Token sent as the 'token: &lt;TOKEN&gt;' HTTP header, or null for no token.
         * @param caCert TLS CA certificate (inline PEM), a "@/path/to/file" reference to read it
         *               from a file, or null to use the default certificate.
         */
        public ConfigBuilder addMetricsServer(String url, @Nullable String token, @Nullable String caCert) {
            return addMetricsServer(url, token, caCert, 0);
        }
        /**
         * Adds one metrics server to send statistics/metrics records to, with an explicit
         * priority tier. Can be called multiple times to send records to several servers.
         *
         * Records are sent concurrently to every server in the highest-priority tier
         * (lowest priority value) that has at least one server; the next tier is only
         * contacted if every server in the current tier fails. Servers sharing the same
         * priority (including the default, 0) fall in the same tier.
         *
         * @param url URL of the metrics server.
         * @param token Token sent as the 'token: &lt;TOKEN&gt;' HTTP header, or null for no token.
         * @param caCert TLS CA certificate (inline PEM), a "@/path/to/file" reference to read it
         *               from a file, or null to use the default certificate.
         * @param priority Priority tier for this server: servers with a lower value are tried
         *                 first. Use 0 for the default priority (a single, flat tier).
         */
        public ConfigBuilder addMetricsServer(String url, @Nullable String token, @Nullable String caCert, int priority) {
            if (url == null)
                return this;

            if (this.metricsServerUrls == null) {
                this.metricsServerUrls = new ArrayList<>();
                this.metricsServerTokens = new ArrayList<>();
                this.metricsServerCaCerts = new ArrayList<>();
                this.metricsServerPriorities = new ArrayList<>();
            }
            this.metricsServerUrls.add(url);
            this.metricsServerTokens.add(token == null ? "." : token);
            this.metricsServerCaCerts.add(caCert == null ? "." : caCert);
            this.metricsServerPriorities.add(priority);
            return this;
        }
        public ConfigBuilder setMetricsEncryptionKey(String key) {
            this.metricsEncryptionKey = key;
            return this;
        }
        public ConfigBuilder setMetricsDeleteAfter(String metricsDeleteAfter){
            this.metricsDeleteAfter = metricsDeleteAfter;
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
            if (tlsCaCertStoreDir == null || !tlsCaCertStoreDir.startsWith(ASSET_PATH)) {
                // Nothing to be done.
                return tlsCaCertStoreDir;
            }

            // TODO: Ouinet's C++ code doesn't yet have a way to read asset
            // files from the apk. As a temporary workaround we copy the
            // asset file to a regular file and the pass path to that to
            // the C++ code.
            String filename = tlsCaCertStoreDir.substring(ASSET_PATH.length());
            String dest = ouinetDirectory + "/assets/" + filename;
            if (copyAssetToFile(tlsCaCertStoreDir, dest)) {
                return dest;
            }
            return null;
        }

        /**
         * Copies the error page to the filesystem if necessary and returns the path
         */
        private @Nullable String setupErrorPage(String ouinetDirectory) {
            if (errorPagePath == null) {
                // Nothing to be done.
                return errorPagePath;
            }
            String dest = ouinetDirectory + "/" + ERROR_PAGE_FILE;
            if (copyAssetToFile(errorPagePath, dest)) {
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
            String dataPath = context.getApplicationInfo().dataDir;
            String dataFilePath = FILE_PROTO + dataPath;
            if (asset.startsWith(dataFilePath)) {
                String dataFile = asset.substring(dataFilePath.length());
                File file = new File(dataPath , dataFile);
                try {
                    streamToFile(new FileInputStream(file), dest);
                } catch (IOException e) {
                    Log.d(TAG, "Failed to write data \"" + asset + "\" to file \"" + dest + "\"", e);
                    return false;
                }
            }
            else if (asset.startsWith(ASSET_PATH)) {
                String assetPath = asset.substring(ASSET_PATH.length());
                try {
                    streamToFile(context.getAssets().open(assetPath), dest);
                } catch (IOException e) {
                    Log.d(TAG, "Failed to write asset \"" + asset + "\" to file \"" + dest + "\"", e);
                    return false;
                }
            }
            else {
                throw new IllegalArgumentException("Invalid asset path: " + asset);
            }

            return true;
        }

        private void streamToFile(java.io.InputStream stream, String dest) throws IOException {
            int size = stream.available();
            byte[] buffer = new byte[size];
            stream.read(buffer);
            stream.close();
            writeToFile(dest, buffer);
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
                    setupErrorPage(ouinetDirectory),
                    setupCaRootCert(ouinetDirectory),
                    setupObfs4ProxyExecutable(ouinetDirectory),
                    cacheType,
                    cachePrivate,
                    cacheStaticPath,
                    cacheStaticContentPath,
                    i2pBep3Tracker,
                    enableI2pServiceExe,
                    enableI2pServiceExt,
                    enableI2pServiceLib,
                    listenOnTcp,
                    frontEndEp,
                    frontEndAccessToken,
                    proxyAccessToken,
                    clientCredentials,
                    debugFrontEndAccessToken,
                    udpMuxPort,
                    udpMuxRxLimit,
                    disableBridgeAnnouncement,
                    requestBodyLimit,
                    maxCachedAge,
                    localDomain,
                    dnsProtocols,
                    disableOriginAccess,
                    disableProxyAccess,
                    disableInjectorAccess,
                    logLevel,
                    enableLogFile,
                    metricsEnableOnStart,
                    metricsServerUrls,
                    metricsServerTokens,
                    metricsServerCaCerts,
                    metricsServerPriorities,
                    metricsEncryptionKey,
                    metricsDeleteAfter);
        }
    }

    private String ouinetDirectory;
    private Set<String> btBootstrapExtras;
    private String cacheHttpPubKey;
    private String injectorCredentials;
    private String injectorTlsCertPath;
    private String tlsCaCertStoreDir;
    private String errorPagePath;
    private String caRootCertPath;
    private String obfs4ProxyPath;
    private String cacheType;
    private boolean cachePrivate;
    private String cacheStaticPath;
    private String cacheStaticContentPath;
    private String i2pBep3Tracker;
    private String enableI2pServiceExe;
    private String enableI2pServiceExt;
    private boolean enableI2pServiceLib;
    private String listenOnTcp;
    private String frontEndEp;
    private String frontEndAccessToken;
    private String proxyAccessToken;
    private String clientCredentials;
    private boolean debugFrontEndAccessToken;
    private String udpMuxPort;
    private String udpMuxRxLimit;
    private boolean disableBridgeAnnouncement;
    private String requestBodyLimit;
    private String maxCachedAge;
    private String localDomain;
    private Set<String> dnsProtocols;
    private boolean disableOriginAccess;
    private boolean disableProxyAccess;
    private boolean disableInjectorAccess;
    private LogLevel logLevel;
    private boolean enableLogFile;
    private boolean metricsEnableOnStart;
    private List<String> metricsServerUrls;
    private List<String> metricsServerTokens;
    private List<String> metricsServerCaCerts;
    private List<Integer> metricsServerPriorities;
    private String metricsEncryptionKey;
    private String metricsDeleteAfter;

    private Config(String ouinetDirectory,
                  Set<String> btBootstrapExtras,
                  String cacheHttpPubKey,
                  String injectorCredentials,
                  String injectorTlsCertPath,
                  String tlsCaCertStoreDir,
                  String errorPagePath,
                  String caRootCertPath,
                  String obfs4ProxyPath,
                  String cacheType,
                  boolean cachePrivate,
                  String cacheStaticPath,
                  String cacheStaticContentPath,
                  String i2pBep3Tracker,
                  String enableI2pServiceExe,
                  String enableI2pServiceExt,
                  boolean enableI2pServiceLib,
                  String listenOnTcp,
                  String frontEndEp,
                  String frontEndAccessToken,
                  String proxyAccessToken,
                  String clientCredentials,
                  boolean debugFrontEndAccessToken,
                  String udpMuxPort,
                  String udpMuxRxLimit,
                  boolean disableBridgeAnnouncement,
                  String requestBodyLimit,
                  String maxCachedAge,
                  String localDomain,
                  Set<String> dnsProtocols,
                  boolean disableOriginAccess,
                  boolean disableProxyAccess,
                  boolean disableInjectorAccess,
                  LogLevel logLevel,
                  boolean enableLogFile,
                  boolean metricsEnableOnStart,
                  List<String> metricsServerUrls,
                  List<String> metricsServerTokens,
                  List<String> metricsServerCaCerts,
                  List<Integer> metricsServerPriorities,
                  String metricsEncryptionKey,
                  String metricsDeleteAfter) {
        this.ouinetDirectory = ouinetDirectory;
        this.btBootstrapExtras = (btBootstrapExtras == null ? null : new HashSet<>(btBootstrapExtras));
        this.cacheHttpPubKey = cacheHttpPubKey;
        this.injectorCredentials = injectorCredentials;
        this.injectorTlsCertPath = injectorTlsCertPath;
        this.tlsCaCertStoreDir = tlsCaCertStoreDir;
        this.errorPagePath = errorPagePath;
        this.caRootCertPath = caRootCertPath;
        this.obfs4ProxyPath = obfs4ProxyPath;
        this.cacheType = cacheType;
        this.cachePrivate = cachePrivate;
        this.cacheStaticPath = cacheStaticPath;
        this.cacheStaticContentPath = cacheStaticContentPath;
        this.i2pBep3Tracker = i2pBep3Tracker;
        this.enableI2pServiceExe = enableI2pServiceExe;
        this.enableI2pServiceExt = enableI2pServiceExt;
        this.enableI2pServiceLib = enableI2pServiceLib;
        this.listenOnTcp = listenOnTcp;
        this.frontEndEp = frontEndEp;
        this.frontEndAccessToken = frontEndAccessToken;
        this.proxyAccessToken = proxyAccessToken;
        this.clientCredentials = clientCredentials;
        this.debugFrontEndAccessToken = debugFrontEndAccessToken;
        this.udpMuxPort = udpMuxPort;
        this.udpMuxRxLimit = udpMuxRxLimit;
        this.disableBridgeAnnouncement = disableBridgeAnnouncement;
        this.requestBodyLimit = requestBodyLimit;
        this.maxCachedAge = maxCachedAge;
        this.localDomain = localDomain;
        this.dnsProtocols = (dnsProtocols == null ? null : new HashSet<>(dnsProtocols));
        this.disableOriginAccess = disableOriginAccess;
        this.disableProxyAccess = disableProxyAccess;
        this.disableInjectorAccess = disableInjectorAccess;
        this.logLevel = logLevel;
        this.enableLogFile = enableLogFile;
        this.metricsEnableOnStart = metricsEnableOnStart;
        this.metricsServerUrls = (metricsServerUrls == null ? null : new ArrayList<>(metricsServerUrls));
        this.metricsServerTokens = (metricsServerTokens == null ? null : new ArrayList<>(metricsServerTokens));
        this.metricsServerCaCerts = (metricsServerCaCerts == null ? null : new ArrayList<>(metricsServerCaCerts));
        this.metricsServerPriorities = (metricsServerPriorities == null ? null : new ArrayList<>(metricsServerPriorities));
        this.metricsEncryptionKey = metricsEncryptionKey;
        this.metricsDeleteAfter = metricsDeleteAfter;
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
    public String getTlsCaCertStoreDir() {
        return tlsCaCertStoreDir;
    }
    public String getErrorPagePath() {
        return errorPagePath;
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
    public String getI2pBep3Tracker() {
        return i2pBep3Tracker;
    }
    public String getEnableI2pServiceExe() {
        return enableI2pServiceExe;
    }
    public String getEnableI2pServiceExt() {
        return enableI2pServiceExt;
    }
    public boolean getEnableI2pServiceLib() {
        return enableI2pServiceLib;
    }
    public String getListenOnTcp() {
        return listenOnTcp;
    }
    public String getFrontEndEp() {
        return frontEndEp;
    }
    public String getFrontEndAccessToken() {
        return frontEndAccessToken;
    }
    public String getProxyAccessToken() {
        return proxyAccessToken;
    }
    public String getClientCredentials() {
        return clientCredentials;
    }
    public boolean getDebugFrontEndAccessToken() {
        return debugFrontEndAccessToken;
    }
    public String getUdpMuxPort() {
        return udpMuxPort;
    }
    public String getUdpMuxRxLimit() {
        return udpMuxRxLimit;
    }
    public boolean getDisableBridgeAnnouncement() {
        return disableBridgeAnnouncement;
    }
    public String getRequestBodyLimit() {
        return requestBodyLimit;
    }
    public String getMaxCachedAge() {
        return maxCachedAge;
    }
    public String getLocalDomain() {
        return localDomain;
    }
    public Set<String> getDnsProtocols() {
        return (dnsProtocols == null ? null : new HashSet<>(dnsProtocols));
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
    public boolean getMetricsEnableOnStart() {
        return metricsEnableOnStart;
    }
    public List<String> getMetricsServerUrls() {
        return (metricsServerUrls == null ? null : new ArrayList<>(metricsServerUrls));
    }
    public List<String> getMetricsServerTokens() {
        return (metricsServerTokens == null ? null : new ArrayList<>(metricsServerTokens));
    }
    public List<String> getMetricsServerCaCerts() {
        return (metricsServerCaCerts == null ? null : new ArrayList<>(metricsServerCaCerts));
    }
    public List<Integer> getMetricsServerPriorities() {
        return (metricsServerPriorities == null ? null : new ArrayList<>(metricsServerPriorities));
    }
    public String getMetricsEncryptionKey() {
        return metricsEncryptionKey;
    }
    public String getMetricsDeleteAfter() {
        return metricsDeleteAfter;
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
        out.writeString(tlsCaCertStoreDir);
        out.writeString(errorPagePath);
        out.writeString(caRootCertPath);
        out.writeString(obfs4ProxyPath);
        out.writeString(cacheType);
        out.writeInt(cachePrivate ? 1 : 0);
        out.writeString(cacheStaticPath);
        out.writeString(cacheStaticContentPath);
        out.writeString(i2pBep3Tracker);
        out.writeString(enableI2pServiceExe);
        out.writeString(enableI2pServiceExt);
        out.writeInt(enableI2pServiceLib ? 1 : 0);
        out.writeString(listenOnTcp);
        out.writeString(frontEndEp);
        out.writeString(frontEndAccessToken);
        out.writeString(proxyAccessToken);
        out.writeString(clientCredentials);
        out.writeInt(debugFrontEndAccessToken ? 1 : 0);
        out.writeString(udpMuxPort);
        out.writeString(udpMuxRxLimit);
        out.writeInt(disableBridgeAnnouncement ? 1 : 0);
        out.writeString(requestBodyLimit);
        out.writeString(maxCachedAge);
        out.writeString(localDomain);
        out.writeStringArray(dnsProtocols == null ? null : dnsProtocols.toArray(new String[0]));
        out.writeInt(disableOriginAccess ? 1 : 0);
        out.writeInt(disableProxyAccess ? 1 : 0);
        out.writeInt(disableInjectorAccess ? 1 : 0);
        // https://stackoverflow.com/a/48533385/273348
        out.writeInt(logLevel == null ? -1 : logLevel.ordinal());
        out.writeInt(enableLogFile ? 1 : 0);
        out.writeInt(metricsEnableOnStart ? 1 : 0);
        out.writeStringList(metricsServerUrls);
        out.writeStringList(metricsServerTokens);
        out.writeStringList(metricsServerCaCerts);
        out.writeList(metricsServerPriorities);
        out.writeString(metricsEncryptionKey);
        out.writeString(metricsDeleteAfter);
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
        tlsCaCertStoreDir = in.readString();
        errorPagePath = in.readString();
        caRootCertPath = in.readString();
        obfs4ProxyPath = in.readString();
        cacheType = in.readString();
        cachePrivate = in.readInt() != 0;
        cacheStaticPath = in.readString();
        cacheStaticContentPath = in.readString();
        i2pBep3Tracker = in.readString();
        enableI2pServiceExe = in.readString();
        enableI2pServiceExt = in.readString();
        enableI2pServiceLib = in.readInt() != 0;
        listenOnTcp= in.readString();
        frontEndEp = in.readString();
        frontEndAccessToken = in.readString();
        proxyAccessToken = in.readString();
        clientCredentials = in.readString();
        debugFrontEndAccessToken = in.readInt() != 0;
        udpMuxPort = in.readString();
        udpMuxRxLimit = in.readString();
        disableBridgeAnnouncement = in.readInt() != 0;
        requestBodyLimit = in.readString();
        maxCachedAge = in.readString();
        localDomain = in.readString();

        String[] dnsProtocolsArray = in.createStringArray();
        if (dnsProtocolsArray == null)
            dnsProtocols = null;
        else {
            dnsProtocols = new HashSet<>();
            Collections.addAll(dnsProtocols, dnsProtocolsArray);
        }

        disableOriginAccess   = in.readInt() != 0;
        disableProxyAccess    = in.readInt() != 0;
        disableInjectorAccess = in.readInt() != 0;

        int logLevelInt = in.readInt();
        logLevel = (logLevelInt == -1 ? null : LogLevel.values()[logLevelInt]);

        enableLogFile = in.readInt() != 0;

        metricsEnableOnStart = in.readInt() != 0;
        metricsServerUrls = in.createStringArrayList();
        metricsServerTokens = in.createStringArrayList();
        metricsServerCaCerts = in.createStringArrayList();
        //noinspection unchecked
        metricsServerPriorities = in.readArrayList(Integer.class.getClassLoader());
        metricsEncryptionKey = in.readString();
        metricsDeleteAfter = in.readString();
    }

}
