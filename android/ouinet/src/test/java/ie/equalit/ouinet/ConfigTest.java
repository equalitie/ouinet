package ie.equalit.ouinet;

import android.content.Context;
import android.content.res.AssetManager;

import org.junit.Test;
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

import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.CoreMatchers.nullValue;
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
    private static String TLS_CA_CERT_PATH = "/tls/store/path.pem";
    private static String FILES_DIR = "/data/ie.equalit.ceno/files/";
    private static String OUINET_DIR = "/data/ie.equalit.ceno/files/ouinet";
    private static String CA_ROOT_CERT_PATH = "/data/ie.equalit.ceno/files/ouinet/ssl-ca-cert.pem";

    @Mock
    private Context mockContext;
    @Mock
    private AssetManager mockAssetManager;

    @Test
    public void test_build() throws IOException {
        when(mockContext.getFilesDir()).thenReturn(new File(FILES_DIR));

        when(mockContext.getAssets()).thenReturn(mockAssetManager);
        InputStream is = new ByteArrayInputStream("obfs4assetcontent".getBytes(StandardCharsets.UTF_8));
        when(mockAssetManager.open(Config.OBFS4_PROXY)).thenReturn(is);

        PowerMockito.mockStatic(Ouinet.class);
        when(Ouinet.getCARootCert(OUINET_DIR)).thenReturn(CA_ROOT_CERT_PATH);

        Config config = new Config.ConfigBuilder(mockContext)
                .setIndexBep44PubKey(INDEX_BEP_44_PUB_KEY)
                .setIndexIpnsId(INDEX_IPNS_ID)
                .setInjectorEndpoint(INJECTOR_ENDPOINT)
                .setInjectorCredentials(INJECTOR_CREDENTIALS)
                .setInjectorTlsCert(INJECTOR_TLS_CERT)
                .setTlsCaCertStorePath(TLS_CA_CERT_PATH)
                .build();
        assertThat(config.getOuinetDirectory(), is(OUINET_DIR));
        assertThat(config.getIndexBep44PubKey(), is(INDEX_BEP_44_PUB_KEY));
        assertThat(config.getIndexIpnsId(), is(INDEX_IPNS_ID));
        assertThat(config.getInjectorEndpoint(), is(INJECTOR_ENDPOINT));
        assertThat(config.getInjectorCredentials(), is(INJECTOR_CREDENTIALS));
        assertThat(config.getInjectorTlsCertPath(), is(nullValue())); // Cannot write to file in unit test
        assertThat(config.getTlsCaCertStorePath(), is(TLS_CA_CERT_PATH));
        assertThat(config.getCaRootCertPath(), is(CA_ROOT_CERT_PATH));
        assertThat(config.getObfs4ProxyPath(), is(nullValue())); // Cannot write to file in unit test
    }
}