#import <Foundation/Foundation.h>

@interface OuinetClient : NSObject

- (id)initWithConfig:(OuinetConfig*)conf;

- (NSNumber*)getClientState;

- (void)start;

@end