#import "Ouinet.h"
#include "native-lib.hpp"

@implementation Ouinet

NativeLib _n;

- (NSNumber*)getClientState
{
  return [NSNumber numberWithInt: _n.getClientState()];
}

- (NSString*)getHelloOuinet
{
  return [NSString stringWithUTF8String: _n.helloOuinet().c_str()];
}

@end
