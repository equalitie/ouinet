#import <Foundation/Foundation.h>

@interface Client : NSObject

- (id)initWithConfig:(Config*)conf;

- (NSNumber*)getClientState;

- (NSString*)getHelloOuinet;

- (void)start;

@end