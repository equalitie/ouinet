#import <Foundation/Foundation.h>

@interface Config : NSObject

- (Config*)init;

- (Config*)setCacheHttpPubKey:(NSString*)key;

- (Config*)setInjectorCredentials:(NSString*)credentials;

- (Config*)setInjectorTlsCert:(NSString*)cert;

- (Config*)setCacheType:(NSString*)type;

- (Config*)setListenOnTcp:(NSString*)address;

- (Config*)setFrontEndEp:(NSString*)address;

- (Config*)setDisableOriginAccess:(BOOL)value;

- (NSString*)getOuinetDirectory;

- (NSString*)getCacheHttpPubKey;

- (NSString*)getInjectorCredentials;

- (NSString*)getInjectorTlsCertPath;

- (NSString*)getTlsCaCertStorePath;

- (NSString*)getCacheType;

- (NSString*)getListenOnTcp;

- (NSString*)getFrontEndEp;

- (BOOL)getDisableOriginAccess;

@end
