package ie.equalit.ouinet;

import android.content.Context;
import android.content.res.AssetManager;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.powermock.api.mockito.PowerMockito;
import org.powermock.core.classloader.annotations.PrepareForTest;
import org.powermock.core.classloader.annotations.SuppressStaticInitializationFor;
import org.powermock.modules.junit4.PowerMockRunner;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

import static org.assertj.core.api.Assertions.contentOf;
import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.mockito.Mockito.when;

@RunWith(PowerMockRunner.class)
@PrepareForTest({Ouinet.class})
@SuppressStaticInitializationFor("ie.equalit.ouinet.Ouinet")
public class ConfigTest {
    private static String INDEX_BEP_44_PUB_KEY = "bep44index1234567890";
    private static String INDEX_IPNS_ID = "indexipnsid1234567";
    private static String INJECTOR_ENDPOINT = "injectorendpoint789123";
    private static String INJECTOR_CREDENTIALS = "injectorcredentials29384293847928498492849284";
    private static String INJECTOR_TLS_CERT = "injectortlscert123123123123123";
    private static String TLS_CA_CERT_PATH = "file:///android_asset/tls-ca-cert.pem";
    private static String TLS_CA_CERT = "tlscacertcontent";
    private static String OBFS_PROXY_CONTENT = "obfs4assetcontent";

    @Mock
    private Context mockContext;
    @Mock
    private AssetManager mockAssetManager;
    @Rule
    public TemporaryFolder tmpDir = new TemporaryFolder();

    @Test
    public void test_build() throws IOException {
        File filesDir = tmpDir.newFolder("files/");
        String ouinetDir = filesDir.getPath() + "/ouinet";
        String caRootCertPath = ouinetDir + "/ssl-ca-cert.pem";
        String injectorTlsCertPath = ouinetDir + "/injector-tls-cert.pem";
        String tlsCaCertPath = ouinetDir + "/assets/tls-ca-cert.pem";
        String obfsFilePath = ouinetDir + "/" + Config.OBFS4_PROXY;

        when(mockContext.getFilesDir()).thenReturn(filesDir);
        when(mockContext.getAssets()).thenReturn(mockAssetManager);
        InputStream obfsproxyIs =
                new ByteArrayInputStream(OBFS_PROXY_CONTENT.getBytes(StandardCharsets.UTF_8));
        when(mockAssetManager.open(Config.OBFS4_PROXY)).thenReturn(obfsproxyIs);
        InputStream tlsCaCertIs =
                new ByteArrayInputStream(TLS_CA_CERT.getBytes(StandardCharsets.UTF_8));
        when(mockAssetManager.open("tls-ca-cert.pem")).thenReturn(tlsCaCertIs);

        PowerMockito.mockStatic(Ouinet.class);
        when(Ouinet.getCARootCert(ouinetDir)).thenReturn(caRootCertPath);

        Config config = new Config.ConfigBuilder(mockContext)
                .setIndexBep44PubKey(INDEX_BEP_44_PUB_KEY)
                .setIndexIpnsId(INDEX_IPNS_ID)
                .setInjectorEndpoint(INJECTOR_ENDPOINT)
                .setInjectorCredentials(INJECTOR_CREDENTIALS)
                .setInjectorTlsCert(INJECTOR_TLS_CERT)
                .setTlsCaCertStorePath(TLS_CA_CERT_PATH)
                .build();
        assertThat(config.getOuinetDirectory(), is(ouinetDir));
        assertThat(config.getIndexBep44PubKey(), is(INDEX_BEP_44_PUB_KEY));
        assertThat(config.getIndexIpnsId(), is(INDEX_IPNS_ID));
        assertThat(config.getInjectorEndpoint(), is(INJECTOR_ENDPOINT));
        assertThat(config.getInjectorCredentials(), is(INJECTOR_CREDENTIALS));
        assertThat(config.getCaRootCertPath(), is(caRootCertPath));

        assertThat(config.getTlsCaCertStorePath(), is(tlsCaCertPath));
        assertThat(contentOf(new File(config.getTlsCaCertStorePath())), is(TLS_CA_CERT));

        assertThat(config.getInjectorTlsCertPath(), is(injectorTlsCertPath));
        assertThat(contentOf(new File(config.getInjectorTlsCertPath())), is(INJECTOR_TLS_CERT));

        assertThat(config.getObfs4ProxyPath(), is(ouinetDir));
        assertThat(contentOf(new File(obfsFilePath)), is(OBFS_PROXY_CONTENT));
    }
}