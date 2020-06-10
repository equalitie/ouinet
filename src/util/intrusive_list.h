#pragma once

#include <boost/intrusive/list.hpp>

namespace ouinet { namespace util { namespace intrusive {

//
// Simplify use of intrusive list by predefining template parameters which
// we always use.
//
// Define hook example:
//   struct Foo {
//      intrusive::list_hook _hook;
//   };
//
// Define list example:
//    intrusive::list<Foo, &Foo::_hook> _foos;
//

using list_hook = boost::intrusive::list_base_hook
                  <boost::intrusive::link_mode
                      <boost::intrusive::auto_unlink>>;

template<class Item, list_hook Item::* HookPtr>
using list = boost::intrusive::list
        < Item
        , boost::intrusive::member_hook<Item, list_hook, HookPtr>
        , boost::intrusive::constant_time_size<false>
        >;

}}} // namespace
