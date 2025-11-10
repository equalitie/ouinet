#import <Foundation/Foundation.h>

@interface OuinetConfig : NSObject

- (OuinetConfig*)init;

- (OuinetConfig*)setCacheHttpPubKey:(NSString*)key;

- (OuinetConfig*)setInjectorCredentials:(NSString*)credentials;

- (OuinetConfig*)setInjectorTlsCert:(NSString*)cert;

- (OuinetConfig*)setCacheType:(NSString*)type;

- (OuinetConfig*)setListenOnTcp:(NSString*)address;

- (OuinetConfig*)setFrontEndEp:(NSString*)address;

- (OuinetConfig*)setDisableOriginAccess:(BOOL)value;

- (OuinetConfig*)setDisableProxyAccess:(BOOL)value;

- (OuinetConfig*)setDisableInjectorAccess:(BOOL)value;

- (OuinetConfig*)setDisableBridgeAnnouncement:(BOOL)value;

- (OuinetConfig*)setLogLevel:(NSString*)level;

- (NSString*)getOuinetDirectory;

- (NSString*)getCacheHttpPubKey;

- (NSString*)getInjectorCredentials;

- (NSString*)getInjectorTlsCertPath;

- (NSString*)getTlsCaCertStorePath;

- (NSString*)getCacheType;

- (NSString*)getListenOnTcp;

- (NSString*)getFrontEndEp;

- (NSString*)getLogLevel;

- (BOOL)getDisableOriginAccess;

- (BOOL)getDisableProxyAccess;

- (BOOL)getDisableInjectorAccess;

- (BOOL)getDisableBridgeAnnouncement;

@end
