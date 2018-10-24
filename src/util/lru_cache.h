#pragma once

#include <unordered_map>
#include <list>
#include <cstddef>
#include <stdexcept>

namespace ouinet { namespace util {

template<typename Key, typename Value>
class LruCache {
private:
    using KeyVal = std::pair<Key, Value>;
    using ListIter = typename std::list<KeyVal>::iterator;

public:
	LruCache(size_t max_size)
        : _max_size(max_size) { }
	
	Value* put(const Key& key, Value value) {
		auto it = _map.find(key);

		_list.push_front(KeyVal(key, std::move(value)));

		if (it != _map.end()) {
			_list.erase(it->second);
			_map.erase(it);
		}

		_map[key] = _list.begin();
		
		if (_map.size() > _max_size) {
			auto last = _list.end();
			last--;
			_map.erase(last->first);
			_list.pop_back();
		}

        return &_list.begin()->second;
	}
	
	Value* get(const Key& key) {
		auto it = _map.find(key);

		if (it == _map.end()) return nullptr;

        _list.splice(_list.begin(), _list, it->second);

        assert(it->second == _list.begin());

		return &it->second->second;
	}
	
	bool exists(const Key& key) const {
		return _map.count(key) != 0;
	}
	
	size_t size() const {
		return _map.size();
	}
	
private:
	std::list<KeyVal> _list;
	std::unordered_map<Key, ListIter> _map;
	size_t _max_size;
};

}} // namespaces
