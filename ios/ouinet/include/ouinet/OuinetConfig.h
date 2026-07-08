#import <Foundation/Foundation.h>

@interface OuinetConfig : NSObject

- (OuinetConfig*)init;

- (OuinetConfig*)setBtBootstrapExtras:(NSArray<NSString*>*)extras;

- (OuinetConfig*)setCacheHttpPubKey:(NSString*)key;

- (OuinetConfig*)setInjectorCredentials:(NSString*)credentials;

- (OuinetConfig*)setInjectorTlsCert:(NSString*)cert;

- (OuinetConfig*)setTlsCaCertStorePath:(NSString*)path;

- (OuinetConfig*)setClientCredentials:(NSString*)credentials;

- (OuinetConfig*)setCacheType:(NSString*)type;

- (OuinetConfig*)setCachePrivate:(BOOL)value;

- (OuinetConfig*)setCacheStaticPath:(NSString*)path;

- (OuinetConfig*)setCacheStaticContentPath:(NSString*)path;

- (OuinetConfig*)setMaxCachedAge:(NSString*)maxCachedAge;

- (OuinetConfig*)setListenOnTcp:(NSString*)address;

- (OuinetConfig*)setUdpMuxPort:(NSString*)port;

- (OuinetConfig*)setUdpMuxRxLimit:(NSString*)limit;

- (OuinetConfig*)setFrontEndEp:(NSString*)address;

- (OuinetConfig*)setFrontEndAccessToken:(NSString*)token;

- (OuinetConfig*)setProxyAccessToken:(NSString*)token;

- (OuinetConfig*)setRequestBodyLimit:(NSString*)limit;

- (OuinetConfig*)setLocalDomain:(NSString*)domain;

- (OuinetConfig*)setDnsProtocols:(NSArray<NSString*>*)protocols;

- (OuinetConfig*)setDisableCacheAccess:(BOOL)value;

- (OuinetConfig*)setEnableLogFile:(BOOL)value;

- (OuinetConfig*)setMetricsEnableOnStart:(BOOL)value;

- (OuinetConfig*)setMetricsServerUrl:(NSString*)url;

- (OuinetConfig*)setMetricsServerToken:(NSString*)token;

- (OuinetConfig*)setMetricsServerTlsCaCert:(NSString*)caCert;

- (OuinetConfig*)setMetricsEncryptionKey:(NSString*)key;

- (OuinetConfig*)setMetricsDeleteAfter:(NSString*)duration;

- (OuinetConfig*)setDisableOriginAccess:(BOOL)value;

- (OuinetConfig*)setDisableProxyAccess:(BOOL)value;

- (OuinetConfig*)setDisableInjectorAccess:(BOOL)value;

- (OuinetConfig*)setDisableBridgeAnnouncement:(BOOL)value;

- (OuinetConfig*)setDisableDoH:(BOOL)value;

- (OuinetConfig*)setLogLevel:(NSString*)level;

- (NSString*)getOuinetDirectory;

- (NSArray<NSString*>*)getBtBootstrapExtras;

- (NSString*)getCacheHttpPubKey;

- (NSString*)getInjectorCredentials;

- (NSString*)getInjectorTlsCertPath;

- (NSString*)getTlsCaCertStorePath;

- (NSString*)getClientCredentials;

- (NSString*)getCacheType;

- (BOOL)getCachePrivate;

- (NSString*)getCacheStaticPath;

- (NSString*)getCacheStaticContentPath;

- (NSString*)getMaxCachedAge;

- (NSString*)getListenOnTcp;

- (NSString*)getUdpMuxPort;

- (NSString*)getUdpMuxRxLimit;

- (NSString*)getFrontEndEp;

- (NSString*)getFrontEndAccessToken;

- (NSString*)getProxyAccessToken;

- (NSString*)getRequestBodyLimit;

- (NSString*)getLocalDomain;

- (NSArray<NSString*>*)getDnsProtocols;

- (BOOL)getDisableCacheAccess;

- (BOOL)getEnableLogFile;

- (BOOL)getMetricsEnableOnStart;

- (NSString*)getMetricsServerUrl;

- (NSString*)getMetricsServerToken;

- (NSString*)getMetricsServerTlsCaCertPath;

- (NSString*)getMetricsEncryptionKey;

- (NSString*)getMetricsDeleteAfter;

- (NSString*)getLogLevel;

- (BOOL)getDisableOriginAccess;

- (BOOL)getDisableProxyAccess;

- (BOOL)getDisableInjectorAccess;

- (BOOL)getDisableBridgeAnnouncement;

- (BOOL)getDisableDoH;

@end
