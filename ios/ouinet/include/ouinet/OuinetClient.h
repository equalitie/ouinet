#import <Foundation/Foundation.h>

@interface OuinetClient : NSObject

- (id)initWithConfig:(OuinetConfig*)conf;

- (NSNumber*)getClientState;

- (void)start;
- (void)stop;

- (NSString*)getProxyEndpoint;
- (NSString*)getFrontendEndpoint;

@end